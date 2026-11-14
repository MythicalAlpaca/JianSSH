#include "jianssh/ui/workspace.hpp"
#include "jianssh/ui/motion.hpp"
#include "jianssh/ui/theme.hpp"
#include "jianssh/ui/layout.hpp"
#include "jianssh/ui/system_theme.hpp"
#include "jianssh/ui/interaction.hpp"
#include "jianssh/app/terminal.hpp"
#include <imgui.h>
#include <atomic>
#include "key_file_picker.hpp"
#include "jianssh/app/private_key_file.hpp"
#include <mutex>
#include <thread>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jianssh::ui {
namespace {
// Visual weight of a button: primary commands wear the system accent, danger
// actions the destructive red, quiet actions only a tinted hover.
enum class ButtonKind { Secondary, Primary, Danger, Quiet };
std::atomic<bool> key_worker_busy{false};
void wipe(char* p, std::size_t n) { volatile char* v = p; while (n--) *v++ = 0; }
template<std::size_t N> void copy(std::array<char,N>& b, const std::string& s) {
    std::snprintf(b.data(), b.size(), "%s", s.c_str());
}
ImVec4 tone(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }
// Shorten a string until it fits the width reserved for it. Hand drawn rows
// must never spill past the column they belong to.
std::string elide(ImFont* font, float size, const std::string& text, float max_width) {
    if (!font || max_width <= 0.f || text.empty()) return {};
    if (font->CalcTextSizeA(size, 1000.f, 0.f, text.c_str()).x <= max_width) return text;
    std::string head;
    for (std::size_t n = 0; n < text.size();) {
        std::size_t next = n + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
        std::string candidate = text.substr(0, next);
        if (font->CalcTextSizeA(size, 1000.f, 0.f, candidate.c_str()).x > max_width) break;
        head = std::move(candidate);
        n = next;
    }
    while (!head.empty() && font->CalcTextSizeA(size, 1000.f, 0.f, (head + "…").c_str()).x > max_width) {
        do { head.pop_back(); }
        while (!head.empty() && (static_cast<unsigned char>(head.back()) & 0xC0) == 0x80);
    }
    return head + "…";
}
} // namespace

struct Workspace::Impl {
    std::uint64_t selected = 0;
    std::uint64_t close_session = 0;
    std::string close_title;
    bool close_pending = false;
    std::int64_t selected_host = 0;
    std::array<char,160> search{};
    std::string group;
    store::Host draft;
    std::array<char,160> name{}, host{}, user{}, host_group{};
    std::array<char,4097> key{};
    std::array<char,512> note{};
    std::array<char,256> password{};
    int port=22, auth=0, remember=0;
    int connection_type=0, serial_baud=115200, serial_data_bits=8, serial_stop_bits=1, serial_parity=0, serial_flow=0;
    std::array<char,4097> serial_port{};
    bool save_to_workspace=false, show_password=false;
    bool has_saved_password=false;
    int view=0;
    std::array<char,1024> local_path{}, remote_path{}, operation_target{};
    std::string local_selection, remote_selection;
    bool local_directory=false, remote_directory=false;
    std::uint64_t files_session=0;
    app::FileOperationRequest operation;
    bool operation_pending=false;
    std::array<char,16> permissions{};
    void clear_connection_secret() { wipe(password.data(),password.size()); show_password=false; }
    void prepare_connection(const store::Host& h,const app::WorkspaceModel& m) {
        prepare(h); remember=0;save_to_workspace=false;has_saved_password=false;
        for(const auto& s:m.connection_settings)if(h.id&&s.host_id==h.id){remember=static_cast<int>(s.policy);has_saved_password=s.has_saved_password;break;}
    }
    bool edit_pending=false, connect_pending=false, delete_pending=false, settings_pending=false;
    bool terminal_focus=false, first=true;
    bool terminal_input_active=false;
    bool restore_terminal_focus=false;
    bool terminal_menu_open=false;
    std::uint64_t selection_session=0;
    int selection_anchor=0, selection_end=0;
    bool selecting=false, selection_valid=false;
    bool paste_pending=false;
    std::uint64_t paste_session=0;
    std::weak_ptr<app::Terminal> paste_terminal;
    std::string paste_text, paste_error;
    void clear_paste() {
        if(!paste_text.empty()) wipe(paste_text.data(),paste_text.size());
        paste_text.clear(); paste_error.clear(); paste_terminal.reset(); paste_session=0;
    }
    void request_paste(const app::SessionView& s) {
        clear_paste(); paste_session=s.id; paste_terminal=s.terminal;
        const char* text=ImGui::GetClipboardText();
        constexpr std::size_t limit=64*1024;
        std::size_t n=0;
        if(text) while(n<=limit&&text[n])++n;
        if(!n) paste_error="剪贴板为空，没有发送任何内容。";
        else if(n>limit) paste_error="剪贴板超过 64 千字节安全上限，已拒绝粘贴。请分段并检查内容。";
        else {
            for(std::size_t i=0;i<n;++i) {
                const unsigned char c=static_cast<unsigned char>(text[i]);
                if((c<32&&c!='\n'&&c!='\r'&&c!='\t')||c==127) {
                    paste_error="内容包含不允许的控制字符（例如转义字符），已拒绝粘贴。";break;
                }
                if(c>=128) {
                    const unsigned count=c>=0xF0&&c<=0xF4?4:(c>=0xE0&&c<=0xEF?3:(c>=0xC2&&c<=0xDF?2:0));
                    bool valid=count&&i+count<=n;
                    for(unsigned j=1;valid&&j<count;++j)valid=(static_cast<unsigned char>(text[i+j])&0xC0)==0x80;
                    if(valid&&count>=3) {
                        const auto second=static_cast<unsigned char>(text[i+1]);
                        valid=!(c==0xE0&&second<0xA0)&&!(c==0xED&&second>=0xA0)&&!(c==0xF0&&second<0x90)&&!(c==0xF4&&second>=0x90);
                    }
                    if(!valid) {paste_error="剪贴板不是有效 UTF-8 文本，已拒绝粘贴。";break;}
                    // Reject Unicode C1 controls as well as ASCII terminal controls.
                    if(c==0xC2&&static_cast<unsigned char>(text[i+1])<=0x9F) {paste_error="内容包含高位控制字符，已拒绝粘贴。";break;}
                    i+=count-1;
                }
            }
            if(paste_error.empty())paste_text.assign(text,n);
        }
        paste_pending=true;
    }
    struct KeyTask { std::mutex mutex; bool done=false; KeyFilePickerResult result; };
    std::shared_ptr<KeyTask> key_task;
    int key_action=0; // 0 picker/preview, 1 save, 2 connect
    void discard_key_task() { key_task.reset();key_action=0; }
    void start_key_task(bool picker,int action) {
        discard_key_task();
        if(key_worker_busy.exchange(true)) {form_error="上一次文件选择或校验尚未结束，请稍后重试。";return;}
        auto task=std::make_shared<KeyTask>(); key_task=task;key_action=action;
        std::string existing=key.data();
        try { std::thread([task,picker,existing] {
            KeyFilePickerResult result;
            try {
                result=picker?choose_key_file():KeyFilePickerResult{existing,{}};
                if(!result.path.empty()&&result.error.empty()) {
                    if(result.path.size()>=4097) {result.path.clear();result.error="私钥路径过长，无法保存。";}
                    else result.error=app::validate_private_key_file(result.path);
                }
            } catch(...) { result.error="文件选择或校验失败。"; }
            { std::lock_guard<std::mutex> lock(task->mutex); task->result=std::move(result);task->done=true; }
            key_worker_busy.store(false);
            // The UI keeps animation_active() true while waiting; no SDL calls
            // from a detached thread, including after SDL shutdown.
        }).detach(); } catch(...) { key_worker_busy.store(false);discard_key_task();form_error="无法启动文件选择或校验线程。"; }
    }
    int poll_key_task() {
        if(!key_task)return 0;
        KeyFilePickerResult result;
        { std::lock_guard<std::mutex> lock(key_task->mutex);if(!key_task->done)return 0;result=std::move(key_task->result); }
        int action=key_action;discard_key_task();
        if(!result.error.empty()) {form_error=result.error;return 0;}
        if(result.path.empty())return 0;
        copy(key,result.path);form_error.clear();return action;
    }
    std::string form_error;
    std::uint64_t trust_id=0, trust_session=0;
    int last_cols=0,last_rows=0;
    std::uint64_t resized_session=0;

    // ---- Appearance and motion state ------------------------------------
    Tween entrance{0}, selection{0}, hover{0}, modal{1};
    Tween theme_fade{1}, notice_fade{1};
    Theme theme{}, theme_from{}, theme_to{};
    bool theme_ready=false;
    bool hovered=false;
    HoverAnimator hover_anim;
    ToggleAnimator toggle_anim;
    LoadingIndicator loading;
    MotionClock::time_point now{};
    bool reduced_motion=false;
    std::string notice_text;
    std::unordered_map<std::uint64_t,float> progress_shown;
    bool loading_visible=false;
    bool progress_visible=false;
    // Central page choreography: only one page is drawn at a time; the
    // transition engine fades out the old key, swaps, then settles the new one.
    PageTransition page;
    bool page_ready=false;
    std::uint64_t page_session=0;
    int page_view=0;
    float page_alpha=1.f;
    float page_offset=0.f;

    ~Impl() { clear_connection_secret();clear_paste(); }
    void animate(Tween& t, double to, const app::WorkspaceModel& m, double seconds) {
        t.retarget(to, now, std::chrono::duration<double>(seconds), Easing::Smoothstep, m.settings.reduced_motion);
    }
    void prepare(const store::Host& h) {
        discard_key_task();
        draft=h; copy(name,h.name); copy(host,h.hostname); copy(user,h.username);
        connection_type=h.connection_type=="serial"?1:0; serial_port.fill(0);
        const bool serial_path_too_long=h.serial_port.size()>=serial_port.size();
        if(!serial_path_too_long) copy(serial_port,h.serial_port);
        serial_baud=static_cast<int>(h.serial_baud); serial_data_bits=static_cast<int>(h.serial_data_bits); serial_stop_bits=static_cast<int>(h.serial_stop_bits);
        serial_parity=h.serial_parity=="even"?1:(h.serial_parity=="odd"?2:0);
        serial_flow=h.serial_flow=="software"?1:(h.serial_flow=="hardware"?2:0);
        copy(host_group,h.group); key.fill(0); if(h.key_path.size()<key.size())copy(key,h.key_path); copy(note,h.note); port=h.port;
        auth=h.auth=="keyboard-interactive"?-1:(h.auth=="agent"?2:(h.auth=="publickey"?1:0));
        clear_connection_secret(); form_error.clear();
        if(serial_path_too_long) form_error="已保存的串口设备路径过长，请重新填写。";
        if(connection_type==0&&auth==1&&key[0])start_key_task(false,0);
    }
    store::Host form_host() const {
        auto h=draft; h.name=name.data(); h.hostname=host.data(); h.username=user.data();
        h.group=host_group.data(); h.key_path=key.data(); h.note=note.data(); h.port=port;
        h.auth=auth==2?"agent":(auth==1?"publickey":"password");
        h.connection_type=connection_type==1?"serial":"ssh"; h.serial_port=serial_port.data();
        h.serial_baud=static_cast<unsigned>(serial_baud); h.serial_data_bits=static_cast<unsigned>(serial_data_bits); h.serial_stop_bits=static_cast<unsigned>(serial_stop_bits);
        h.serial_parity=serial_parity==1?"even":(serial_parity==2?"odd":"none");
        h.serial_flow=serial_flow==1?"software":(serial_flow==2?"hardware":"none");
        if(h.name.empty()) h.name=connection_type==1?h.serial_port:h.hostname;
        return h;
    }
    bool valid_connection() {
        if(!draft.id&&auth<2&&remember!=0&&!save_to_workspace) {
            form_error="记忆连接口令需要勾选保存到工作空间设备。";return false;
        }
        return true;
    }
    bool valid() {
        if(connection_type==1) {
            if(!serial_port[0]||serial_baud<=0||serial_baud>4000000) {form_error="请填写串口设备，波特率须在 1–4000000 之间。";return false;}
            if(serial_data_bits==5&&serial_stop_bits==2) {form_error="不支持 5 数据位与 2 停止位的组合。";return false;}
            return true;
        }
        if(auth<0) {form_error="原交互式认证已不支持，请重新选择认证方式。";return false;}
        if(!host[0] || !user[0]) {form_error="请填写服务器地址和用户名。";return false;}
        if(port<1||port>65535) {form_error="端口必须在 1–65535 之间。";return false;}
        if(auth==1&&!key[0]) {form_error="请选择私钥文件（或已保存的路径过长）。";return false;}
        return true;
    }

    // ---- Theme resolution -------------------------------------------------
    // The palette is interpolated, so switching theme or following a desktop
    // change animates instead of cutting.
    void resolve_theme(const app::WorkspaceModel& m) {
        const Theme target=theme_for(m.settings.theme,system_theme());
        if(!theme_ready) {
            theme=target; theme_from=target; theme_to=target; theme_ready=true;
            theme_fade.start(1,1,now,std::chrono::duration<double>(0),Easing::Smoothstep,reduced_motion);
            return;
        }
        if(target!=theme_to) {
            theme_from=theme; theme_to=target;
            theme_fade.start(0,1,now,std::chrono::duration<double>(motion.theme),Easing::EaseInOutQuint,reduced_motion);
        }
        const double t=theme_fade.update(now,reduced_motion);
        theme=lerp_theme(theme_from,theme_to,static_cast<float>(t));
    }

    // ---- Animated widgets -------------------------------------------------
    // Buttons and inputs are painted with an eased colour instead of ImGui's
    // instant hover switch, so every highlight grows and fades.
    void button_depth(float rounding,float hover,ButtonKind kind) {
        const ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        auto* dl=ImGui::GetWindowDrawList();
        if(kind==ButtonKind::Quiet) {
            if(hover>0.01f)dl->AddRectFilled(a,b,fade_color(theme.accent,0.10f*hover),rounding);
            return;
        }
        const bool white_sheen=kind==ButtonKind::Primary||kind==ButtonKind::Danger;
        const ImU32 sheen=fade_color(IM_COL32(255,255,255,255),
            white_sheen?0.16f+0.10f*hover:(theme.dark?0.035f+0.045f*hover:0.42f+0.22f*hover));
        dl->AddRectFilledMultiColor(ImVec2(a.x+1.f,a.y+1.f),ImVec2(b.x-1.f,a.y+(b.y-a.y)*0.55f),
            sheen,sheen,fade_color(sheen,0.f),fade_color(sheen,0.f));
        const ImU32 border=white_sheen?fade_color(IM_COL32(255,255,255,255),0.20f):fade_color(theme.border,0.95f);
        dl->AddRect(a,b,border,rounding,0,1.f);
        if(ImGui::IsItemFocused()&&ImGui::GetIO().NavActive)
            dl->AddRect(ImVec2(a.x-1.f,a.y-1.f),ImVec2(b.x+1.f,b.y+1.f),fade_color(theme.accent,0.65f),rounding+1.f,0,1.5f);
    }
    void panel_depth(float accent_amount) {
        const ImVec2 p=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
        auto* dl=ImGui::GetWindowDrawList();
        const ImU32 tint=fade_color(lerp_color(theme.card_bg,theme.accent,accent_amount),0.45f);
        const float depth=std::min(160.f,size.y*0.45f);
        dl->AddRectFilledMultiColor(p,ImVec2(p.x+size.x,p.y+depth),tint,tint,fade_color(tint,0.f),fade_color(tint,0.f));
        dl->AddLine(ImVec2(p.x+1.f,p.y+1.f),ImVec2(p.x+size.x-1.f,p.y+1.f),fade_color(theme.border,0.55f));
    }
    bool button(const char* label_text, const ImVec2& size, const char* id, ButtonKind kind=ButtonKind::Secondary) {
        const float f=hover_anim.factor(id,now,reduced_motion);
        ImU32 base=theme.button,hover=lerp_color(theme.button,theme.button_hovered,f),active=theme.button_active;
        ImU32 text_color=theme.text;
        if(kind==ButtonKind::Primary) {
            base=theme.accent;hover=lerp_color(theme.accent,theme.accent_hover,f);active=theme.accent_active;
            text_color=IM_COL32(255,255,255,255);
        } else if(kind==ButtonKind::Danger) {
            base=theme.danger;hover=lerp_color(theme.danger,IM_COL32(255,120,110,255),f);active=lerp_color(theme.danger,IM_COL32(180,30,24,255),0.55f);
            text_color=IM_COL32(255,255,255,255);
        } else if(kind==ButtonKind::Quiet) {
            base=IM_COL32(0,0,0,0);hover=fade_color(theme.accent,0.10f*f);active=fade_color(theme.accent,0.16f);
            text_color=theme.accent;
        }
        ImGui::PushStyleColor(ImGuiCol_Text,tone(text_color));
        ImGui::PushStyleColor(ImGuiCol_Button,tone(base));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,tone(hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,tone(active));
        const bool pressed=ImGui::Button(label_text,size);
        button_depth(layout.frame_rounding,f,kind);
        ImGui::PopStyleColor(4);
        hover_anim.observe(id,ImGui::IsItemHovered(),now,reduced_motion);
        return pressed;
    }
    bool small_button(const char* label_text, const char* id) {
        const float f=hover_anim.factor(id,now,reduced_motion);
        ImGui::PushStyleColor(ImGuiCol_Button,tone(theme.button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,tone(lerp_color(theme.button,theme.button_hovered,f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,tone(theme.button_active));
        const bool pressed=ImGui::SmallButton(label_text);
        button_depth(6.f,f,ButtonKind::Secondary);
        ImGui::PopStyleColor(3);
        hover_anim.observe(id,ImGui::IsItemHovered(),now,reduced_motion);
        return pressed;
    }
    bool selectable(const char* label_text, bool selected, int flags, const ImVec2& size, const char* id) {
        const float f=hover_anim.factor(id,now,reduced_motion);
        const bool visible=label_text[0]!='#';
        if(visible) {
            // Labelled rows are drawn by hand: transparent ImGui header, a
            // rounded pill and text with a real inset from the pill edge.
            ImGui::PushStyleColor(ImGuiCol_Header,IM_COL32(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,IM_COL32(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,IM_COL32(0,0,0,0));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Header,tone(theme.header));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,tone(lerp_color(theme.header,theme.header_hovered,f)));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,tone(lerp_color(theme.header_hovered,theme.header_active,f)));
        }
        const bool compact=visible&&size.x>0.f;
        if(compact) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,0));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,ImVec2(0.f,0.5f));
        }
        const std::string select_id=visible?(std::string("##")+id):std::string(label_text);
        const bool pressed=ImGui::Selectable(select_id.c_str(),selected,flags,size);
        if(compact) ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        if(visible) {
            const ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
            const float width=std::max(1.f,b.x-a.x);
            auto* dl=ImGui::GetWindowDrawList();
            constexpr float rounding=7.f;
            if(selected) {
                dl->AddRectFilled(a,b,theme.header,rounding);
                dl->AddRect(a,b,fade_color(theme.accent,0.32f),rounding,0,1.f);
            } else if(f>0.01f) {
                dl->AddRectFilled(a,b,fade_color(theme.ink,theme.dark?0.055f*f:0.045f*f),rounding);
            }
            const float inset=10.f;
            if(ImGui::GetFont()) {
                const std::string shown=elide(ImGui::GetFont(),ImGui::GetFontSize(),label_text,std::max(10.f,width-inset*2.f));
                const float item_height=std::max(ImGui::GetFontSize(),b.y-a.y);
                dl->AddText(ImVec2(a.x+inset,a.y+(item_height-ImGui::GetFontSize())*0.5f),
                            theme.ink,shown.c_str());
            }
        }
        hover_anim.observe(id,ImGui::IsItemHovered(),now,reduced_motion);
        return pressed;
    }
    void begin_frame_widget(const char* id) {
        const float f=hover_anim.factor(id,now,reduced_motion);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,tone(lerp_color(theme.frame_bg,theme.frame_hovered,f)));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,tone(lerp_color(theme.frame_bg,theme.frame_hovered,f)));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,tone(lerp_color(theme.frame_hovered,theme.frame_active,f)));
    }
    void end_frame_widget(const char* id) {
        hover_anim.observe(id,ImGui::IsItemHovered(),now,reduced_motion);
        ImGui::PopStyleColor(3);
    }
    bool checkbox(const char* label_text, bool* value, const char* id) {
        begin_frame_widget(id);
        const bool changed=ImGui::Checkbox(label_text,value);
        end_frame_widget(id);
        return changed;
    }
    // Rounded search field with a hand drawn magnifier, the way macOS renders
    // the toolbar search control.
    bool search_field(const char* hint,char* buffer,std::size_t capacity,const char* id) {
        const float f=hover_anim.factor(id,now,reduced_motion);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(30.f,layout.frame_padding_y));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,tone(lerp_color(theme.frame_bg,theme.frame_hovered,f)));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,tone(lerp_color(theme.frame_bg,theme.frame_hovered,f)));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,tone(theme.frame_active));
        const bool changed=ImGui::InputTextWithHint("##搜索框",hint,buffer,capacity);
        ImGui::PopStyleColor(3);ImGui::PopStyleVar();
        hover_anim.observe(id,ImGui::IsItemHovered(),now,reduced_motion);
        const ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        auto* dl=ImGui::GetWindowDrawList();
        const ImVec2 center(a.x+16.f,(a.y+b.y)*0.5f);
        const ImU32 tint=lerp_color(theme.muted,theme.accent,f*0.7f);
        dl->AddCircle(center,4.2f,tint,16,1.3f);
        dl->AddLine(ImVec2(center.x+3.1f,center.y+3.1f),ImVec2(center.x+6.6f,center.y+6.6f),tint,1.3f);
        return changed;
    }
    // macOS segmented control: one rounded track and a raised selected pill.
    float segmented_width(const char* const* labels,int count) const {
        float total=layout.segment_padding*2.f;
        for(int i=0;i<count;++i)total+=std::max(58.f,ImGui::CalcTextSize(labels[i]).x+30.f);
        return total;
    }
    int segmented(const char* id,const char* const* labels,int count,int selected,float* out_width=nullptr) {
        ImFont* font=ImGui::GetFont();const float fs=ImGui::GetFontSize();
        std::array<float,4> widths{};
        float total=layout.segment_padding*2.f;
        for(int i=0;i<count&&i<4;++i){widths[i]=std::max(58.f,ImGui::CalcTextSize(labels[i]).x+30.f);total+=widths[i];}
        const ImVec2 origin=ImGui::GetCursorScreenPos();
        const float height=layout.segment_height;
        auto* dl=ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin,ImVec2(origin.x+total,origin.y+height),fade_color(theme.frame_bg,0.90f),layout.segment_rounding);
        dl->AddRect(origin,ImVec2(origin.x+total,origin.y+height),fade_color(theme.border,0.80f),layout.segment_rounding,0,1.f);
        ImGui::PushID(id);
        int clicked=-1;float x=origin.x+layout.segment_padding;
        for(int i=0;i<count;++i) {
            const ImVec2 a(x,origin.y+layout.segment_padding),b(x+widths[i],origin.y+height-layout.segment_padding);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(a);
            const bool press=ImGui::InvisibleButton("##segment",ImVec2(b.x-a.x,b.y-a.y));
            const bool hovered=ImGui::IsItemHovered();
            const std::string key=std::string(id)+":"+std::to_string(i);
            const float f=hover_anim.factor(key,now,reduced_motion);
            if(press&&clicked<0)clicked=i;
            if(i==selected) {
                dl->AddRectFilled(a,b,theme.dark?theme.card_bg_alt:theme.surface,7.f);
                dl->AddRect(a,b,fade_color(theme.border,0.75f),7.f,0,1.f);
                dl->AddLine(ImVec2(a.x+5.f,b.y-1.f),ImVec2(b.x-5.f,b.y-1.f),fade_color(theme.shadow,0.9f));
            } else if(f>0.01f) {
                dl->AddRectFilled(a,b,fade_color(theme.ink,theme.dark?0.06f*f:0.045f*f),7.f);
            }
            if(font) {
                const ImVec2 ts=font->CalcTextSizeA(fs,1000.f,0.f,labels[i]);
                dl->AddText(font,fs,ImVec2(a.x+(b.x-a.x-ts.x)*0.5f,a.y+(b.y-a.y-ts.y)*0.5f),
                            i==selected?theme.ink:theme.muted,labels[i]);
            }
            hover_anim.observe(key,hovered,now,reduced_motion);
            ImGui::PopID();
            x=b.x;
        }
        ImGui::SetCursorScreenPos(origin);ImGui::Dummy(ImVec2(total,height));
        ImGui::PopID();
        if(out_width)*out_width=total;
        return clicked;
    }
    bool toggle(const char* label_text,bool* value,const char* id) {
        const float f=toggle_anim.factor(id,*value,now,reduced_motion);
        constexpr float width=38.f,height=22.f,knob=9.f;
        const ImVec2 pos=ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id,ImVec2(width,height));
        const bool pressed=ImGui::IsItemClicked();
        if(pressed)*value=!*value;
        const bool hovered=ImGui::IsItemHovered();
        auto* dl=ImGui::GetWindowDrawList();
        const ImU32 track=lerp_color(theme.frame_bg,theme.accent,f);
        dl->AddRectFilled(pos,ImVec2(pos.x+width,pos.y+height),hovered?lerp_color(track,theme.accent_hover,0.20f):track,height*0.5f);
        if(f<0.5f)dl->AddRect(pos,ImVec2(pos.x+width,pos.y+height),fade_color(theme.border,0.85f),height*0.5f,0,1.f);
        const float center_x=pos.x+2.f+knob+(width-4.f-knob*2.f)*f;
        dl->AddCircleFilled(ImVec2(center_x,pos.y+height*0.5f),knob,IM_COL32(255,255,255,255),24);
        dl->AddCircle(ImVec2(center_x,pos.y+height*0.5f),knob,fade_color(theme.shadow,0.28f),24,1.f);
        ImGui::SameLine(0,10.f);ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label_text);
        return pressed;
    }
    static std::string first_glyph(const std::string& text) {
        if(text.empty())return "?";
        const unsigned char c=static_cast<unsigned char>(text[0]);
        std::size_t n=(c&0xF8u)==0xF0u?4:((c&0xF0u)==0xE0u?3:((c&0xE0u)==0xC0u?2:1));
        if(n>text.size())n=text.size();
        return text.substr(0,n);
    }
    void draw_avatar(ImVec2 pos,float size,const std::string& name,std::uint64_t id) const {
        static const ImU32 colors[]={IM_COL32(10,132,255,255),IM_COL32(94,92,230,255),IM_COL32(48,176,199,255),IM_COL32(255,149,0,255),IM_COL32(191,90,242,255)};
        auto* dl=ImGui::GetWindowDrawList();
        const ImU32 color=colors[id%5];
        dl->AddRectFilled(pos,ImVec2(pos.x+size,pos.y+size),color,size*0.28f);
        dl->AddRectFilledMultiColor(pos,ImVec2(pos.x+size,pos.y+size),
            fade_color(IM_COL32(255,255,255,255),0.16f),fade_color(IM_COL32(255,255,255,255),0.06f),
            fade_color(IM_COL32(255,255,255,255),0.f),fade_color(IM_COL32(255,255,255,255),0.f));
        ImFont* font=ImGui::GetFont();if(!font)return;
        const std::string glyph=first_glyph(name);
        const float fs=size*0.42f;
        const ImVec2 ts=font->CalcTextSizeA(fs,1000.f,0.f,glyph.c_str());
        dl->AddText(font,fs,ImVec2(pos.x+(size-ts.x)*0.5f,pos.y+(size-ts.y)*0.5f),IM_COL32(255,255,255,245),glyph.c_str());
    }
    void draw_brand_icon(ImVec2 pos,float size) const {
        auto* dl=ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos,ImVec2(pos.x+size,pos.y+size),theme.accent,size*0.28f);
        dl->AddRectFilledMultiColor(pos,ImVec2(pos.x+size,pos.y+size*0.55f),
            fade_color(IM_COL32(255,255,255,255),0.20f),fade_color(IM_COL32(255,255,255,255),0.06f),
            fade_color(IM_COL32(255,255,255,255),0.f),fade_color(IM_COL32(255,255,255,255),0.f));
        const float stroke=std::max(1.5f,size*0.055f);
        const ImU32 white=IM_COL32(255,255,255,240);
        dl->AddLine(ImVec2(pos.x+size*0.26f,pos.y+size*0.38f),ImVec2(pos.x+size*0.42f,pos.y+size*0.50f),white,stroke);
        dl->AddLine(ImVec2(pos.x+size*0.42f,pos.y+size*0.50f),ImVec2(pos.x+size*0.26f,pos.y+size*0.62f),white,stroke);
        dl->AddLine(ImVec2(pos.x+size*0.50f,pos.y+size*0.62f),ImVec2(pos.x+size*0.72f,pos.y+size*0.62f),white,stroke);
    }

    // ---- Modal transition --------------------------------------------------
    void begin_modal() {
        modal.start(0,1,now,std::chrono::duration<double>(motion.modal),Easing::EaseOutQuint,reduced_motion);
    }
    float modal_alpha() { return static_cast<float>(modal.update(now,reduced_motion)); }
    void constrain_modal(float desired_width) {
        const ImVec2 available=ImGui::GetMainViewport()->WorkSize;
        const float max_width=std::max(1.f,available.x-layout.modal_viewport_margin*2.f);
        const float max_height=std::max(1.f,available.y-layout.modal_viewport_margin*2.f);
        const float width=std::min(desired_width,max_width);
        ImGui::SetNextWindowSize(ImVec2(width,0),ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(layout.modal_min_width,max_width),0),ImVec2(max_width,max_height));
    }
    // Height available to a modal body after reserving chrome and the footer.
    // The body always scrolls vertically, so long forms and 32 interactive
    // prompts can never enlarge the popup past the main window.
    // Final safety net: even during the slide-in offset or on a tiny viewport,
    // the popup rectangle is nudged back inside the main window.
    void clamp_modal_to_viewport() {
        const ImGuiViewport* viewport=ImGui::GetMainViewport();
        const ImVec2 pos=ImGui::GetWindowPos();
        const ImVec2 size=ImGui::GetWindowSize();
        const float min_x=viewport->WorkPos.x;
        const float min_y=viewport->WorkPos.y;
        const float max_x=std::max(min_x,viewport->WorkPos.x+viewport->WorkSize.x-size.x);
        const float max_y=std::max(min_y,viewport->WorkPos.y+viewport->WorkSize.y-size.y);
        const ImVec2 clamped(std::clamp(pos.x,min_x,max_x),std::clamp(pos.y,min_y,max_y));
        if(clamped.x!=pos.x||clamped.y!=pos.y)ImGui::SetWindowPos(clamped);
    }
    float modal_body_height(float desired) const {
        const ImVec2 available=ImGui::GetMainViewport()->WorkSize;
        const float usable=available.y-layout.modal_viewport_margin*2.f;
        // On tiny viewports the usual minimum must yield to title and footer.
        const float max_body=std::max(1.f,
                                      usable*layout.modal_body_max_ratio-layout.modal_chrome_height);
        return std::min(desired,max_body);
    }
    void begin_modal_body(float height) {
        ImGui::BeginChild("##弹窗正文",ImVec2(0,height),false);
    }
    void end_modal_body() { ImGui::EndChild(); }
    // Two footer buttons share the row, so a long label can never push the
    // second button outside a narrow popup.
    float footer_pair_width(float desired) const {
        const float usable=std::max(1.f,ImGui::GetContentRegionAvail().x-layout.item_spacing);
        return std::max(56.f,std::min(desired,usable*0.5f));
    }
    // Slide the popup up into place. The popup is always placed by the pivot,
    // so the resting position is the viewport centre and the animation cannot
    // leave it displaced: ImGui latches the first position it ever computed.
    void apply_modal_offset(float alpha) {
        const float offset=(1.f-alpha)*static_cast<float>(motion.modal_slide);
        const ImVec2 center=ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(ImVec2(center.x,center.y+offset),ImGuiCond_Always,ImVec2(0.5f,0.5f));
    }

    void label(const char* text) { ImGui::TextColored(tone(theme.muted),"%s",text); }
    void label(const std::string& text) { ImGui::TextColored(tone(theme.muted),"%s",text.c_str()); }

    // Keep serial diagnostics within their parent and reserve room for the form
    // or terminal. The non-interactive preview does not enter keyboard navigation.
    void serial_error(const char* id, const std::string& text) {
        const float width=std::max(1.f,ImGui::GetContentRegionAvail().x);
        const float line=ImGui::GetTextLineHeight();
        const float text_height=ImGui::CalcTextSize(text.c_str(),nullptr,false,width).y;
        const float height=std::min(text_height,line*3.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::BeginChild(id,ImVec2(width,std::max(1.f,height)),false,ImGuiWindowFlags_NoNav);
        ImGui::PushStyleColor(ImGuiCol_Text,tone(theme.danger));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+std::max(1.f,ImGui::GetContentRegionAvail().x));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // ---- Terminal colours --------------------------------------------------
    ImU32 term_color(const app::TerminalColor& c, bool foreground) const {
        if(c.kind==app::ColorKind::Default) return foreground?theme.terminal_fg:theme.terminal_bg;
        if(c.kind==app::ColorKind::TrueColor) return IM_COL32(c.red,c.green,c.blue,255);
        unsigned i=c.index;
        if(i<16) return theme.ansi[i];
        if(i>=232) { unsigned v=8+(i-232)*10; return IM_COL32(v,v,v,255); }
        i-=16; auto channel=[](unsigned n){return n ? 55+n*40 : 0;};
        return IM_COL32(channel(i/36),channel((i/6)%6),channel(i%6),255);
    }
    // Labels occupy a bounded left column so a field can never push its label
    // out of the modal, even at the minimum popup width.
    void field_row(const char* text) {
        ImGui::AlignTextToFramePadding();
        label(text);
        const float label_column=std::clamp(ImGui::GetContentRegionAvail().x*0.30f,88.f,126.f);
        if(ImGui::CalcTextSize(text).x+ImGui::GetStyle().ItemSpacing.x<label_column)
            ImGui::SameLine(label_column);
    }
    void host_fields(bool connecting) {
        field_row("设备名称");
        begin_frame_widget("设备名称");ImGui::SetNextItemWidth(-1);ImGui::InputText("##设备名称",name.data(),name.size());end_frame_widget("设备名称");
        field_row("服务器地址");
        begin_frame_widget("服务器地址");ImGui::SetNextItemWidth(-1);ImGui::InputText("##服务器地址",host.data(),host.size());end_frame_widget("服务器地址");
        field_row("SSH 端口");
        begin_frame_widget("SSH 端口");ImGui::SetNextItemWidth(std::min(layout.input_port_width,ImGui::GetContentRegionAvail().x));ImGui::InputInt("##SSH 端口",&port,0,0);end_frame_widget("SSH 端口");
        field_row("用户名");
        begin_frame_widget("用户名");ImGui::SetNextItemWidth(-1);ImGui::InputText("##用户名",user.data(),user.size());end_frame_widget("用户名");
        field_row("认证方式");
        begin_frame_widget("认证方式");ImGui::SetNextItemWidth(-1);
        if(ImGui::Combo("##认证方式",&auth,"密码\0私钥\0SSH 代理\0")) {discard_key_task();clear_connection_secret();form_error.clear();if(auth==1&&key[0])start_key_task(false,0);}
        end_frame_widget("认证方式");
        if(draft.auth=="keyboard-interactive") ImGui::TextWrapped("原交互式认证已停用，请先编辑设备并保存新的认证方式，再连接。");
        if(auth==1) {
            field_row("私钥路径");
            ImGui::TextWrapped("%s",key[0]?key.data():"尚未选择文件");
            ImGui::BeginDisabled(static_cast<bool>(key_task));
            if(button("选择私钥文件",ImVec2(0,layout.button_height_small),"选择私钥文件"))start_key_task(true,0);
            ImGui::EndDisabled();
            if(key_task)ImGui::TextUnformatted("正在选择或校验私钥文件…");
        }
        if(connecting&&!draft.id) {
            checkbox("保存到工作空间设备",&save_to_workspace,"保存到工作空间设备");
            ImGui::TextWrapped("记忆口令需保存设备；否则仅连接。");
        }
        if(connecting&&auth>=0&&auth<2) {
            field_row("连接口令");
            begin_frame_widget("连接口令");ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##连接口令",password.data(),password.size(),show_password?ImGuiInputTextFlags_None:ImGuiInputTextFlags_Password);
            end_frame_widget("连接口令");
            checkbox("显示口令明文",&show_password,"显示口令明文");
            if(draft.id&&has_saved_password)
                ImGui::TextWrapped("此设备已有保存口令：选择记忆策略后，留空使用保存口令，输入新口令替换；保存口令不会回填显示。");
            field_row("记忆策略");
            begin_frame_widget("记忆策略");ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##此设备记忆策略",&remember,"从不记忆\0仅本次运行\0加密储存连接口令\0"))form_error.clear();
            end_frame_widget("记忆策略");
            ImGui::TextWrapped("口令不写入设备资料；仅本次运行在退出时清除。隐藏仅为掩码，并非加密。");
            if(!draft.id&&!save_to_workspace&&remember!=0)
                ImGui::TextColored(tone(theme.warning),"请勾选保存到工作空间设备，或改为从不记忆。");
        }
        if(!connecting) {
            field_row("分组");
            begin_frame_widget("分组");ImGui::SetNextItemWidth(-1);ImGui::InputText("##分组",host_group.data(),host_group.size());end_frame_widget("分组");
            field_row("备注");
            begin_frame_widget("备注");ImGui::SetNextItemWidth(-1);ImGui::InputTextMultiline("##备注",note.data(),note.size(),ImVec2(-1,layout.note_field_height));end_frame_widget("备注");
            label("设备记录不包含密码；连接时再输入。");
        }
        if(!form_error.empty()) ImGui::TextColored(tone(theme.danger),"%s",form_error.c_str());
    }
    void serial_fields(const app::WorkspaceModel& m,const app::WorkspaceCallbacks& cb, bool connecting) {
        if(!connecting) {
            field_row("设备名称");
            ImGui::SetNextItemWidth(-1); ImGui::InputText("##设备名称",name.data(),name.size());
        }
        field_row("串口设备");
        ImGui::SetNextItemWidth(-1); ImGui::InputText("##串口路径",serial_port.data(),serial_port.size());
        if(ImGui::BeginCombo("可用设备", "选择设备")) {
            for(const auto& device:m.serial_ports) {
                ImGui::PushID(device.c_str());
                if(ImGui::Selectable(device.c_str())) {
                    if(device.size()>=serial_port.size()) {serial_port.fill(0);form_error="串口设备路径过长，请手动填写。";}
                    else {copy(serial_port,device);form_error.clear();}
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(m.serial_ports_loading||!cb.serial_refresh);
        if(button("刷新设备列表",ImVec2(0,layout.button_height_small),"刷新串口")) cb.serial_refresh();
        ImGui::EndDisabled();
        if(m.serial_ports_loading) label("正在枚举设备…");
        if(!m.serial_ports_error.empty()) serial_error("##串口枚举错误",m.serial_ports_error);
        // Narrow windows fall back to one column and vertical scrolling.
        const int columns=ImGui::GetContentRegionAvail().x>=360.f?2:1;
        if(ImGui::BeginTable("##串口参数",columns,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); label("波特率"); ImGui::SetNextItemWidth(-1); ImGui::InputInt("##波特率",&serial_baud,0,0);
            ImGui::TableNextColumn(); label("数据位"); int data_index=serial_data_bits-5;
            ImGui::SetNextItemWidth(-1); if(ImGui::Combo("##数据位",&data_index,"5\0 6\0 7\0 8\0")) serial_data_bits=data_index+5;
            ImGui::TableNextColumn(); label("停止位"); int stop_index=serial_stop_bits-1;
            ImGui::SetNextItemWidth(-1); if(ImGui::Combo("##停止位",&stop_index,"1\0 2\0")) serial_stop_bits=stop_index+1;
            ImGui::TableNextColumn(); label("校验"); ImGui::SetNextItemWidth(-1); ImGui::Combo("##校验",&serial_parity,"无\0偶校验\0奇校验\0");
            ImGui::TableNextColumn(); label("流控"); ImGui::SetNextItemWidth(-1); ImGui::Combo("##流控",&serial_flow,"无\0软件 XON/XOFF\0硬件 RTS/CTS\0");
            ImGui::EndTable();
        }
        ImGui::PushStyleColor(ImGuiCol_Text,tone(theme.warning));
        ImGui::TextWrapped("打开串口可能改变 DTR/RTS 并复位连接设备。");
        ImGui::PopStyleColor();
        if(!connecting) {
            field_row("分组");
            ImGui::SetNextItemWidth(-1); ImGui::InputText("##分组",host_group.data(),host_group.size());
            field_row("备注");
            ImGui::InputTextMultiline("##备注",note.data(),note.size(),ImVec2(-1,layout.note_field_height));
        }
        ImGui::TextWrapped("设备不支持的串口配置组合将由后台报告。");
        if(!form_error.empty()) serial_error("##串口表单错误",form_error);
    }
    void dialogs(const app::WorkspaceModel& m,const app::WorkspaceCallbacks& cb) {
        if(edit_pending) {ImGui::OpenPopup("设备资料");edit_pending=false;begin_modal();}
        if(connect_pending) {ImGui::OpenPopup("连接到服务器");connect_pending=false;begin_modal();}
        if(delete_pending) {ImGui::OpenPopup("删除设备？");delete_pending=false;begin_modal();}
        if(settings_pending) {ImGui::OpenPopup("偏好设置");settings_pending=false;begin_modal();}
        if(close_pending) {ImGui::OpenPopup("关闭会话？");close_pending=false;begin_modal();}
        if(paste_pending) {ImGui::OpenPopup("确认粘贴到远端");paste_pending=false;begin_modal();}
        if(m.trust && (trust_id!=m.trust->request_id||trust_session!=m.trust->session_id)) {
            trust_id=m.trust->request_id;trust_session=m.trust->session_id;
            ImGui::OpenPopup("验证服务器身份");begin_modal();
        }
        const int key_ready=poll_key_task();
        const float a=modal_alpha();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,a);
        constrain_modal(layout.modal_width_device);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("设备资料",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10.f,6.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10.f,6.f));
            begin_modal_body(modal_body_height(380));
            ImGui::BeginDisabled(static_cast<bool>(key_task)||key_ready!=0);
            field_row("连接方式");
            ImGui::SetNextItemWidth(std::min(180.f,ImGui::GetContentRegionAvail().x));
            if(ImGui::Combo("##设备连接方式",&connection_type,"SSH\0串口\0")) {discard_key_task();clear_connection_secret();form_error.clear();if(connection_type==0&&auth==1&&key[0])start_key_task(false,0);}
            if(connection_type==0) host_fields(false); else serial_fields(m,cb,false);
            ImGui::EndDisabled();
            end_modal_body();
            ImGui::PopStyleVar(2);
            const float footer=footer_pair_width(130);
            const bool save_clicked=button("保存设备",ImVec2(footer,layout.button_height),"保存设备",ButtonKind::Primary);
            if((save_clicked||key_ready==1)&&valid()&&cb.save_host&&!key_task) {
                if(connection_type==0&&auth==1&&key_ready!=1) start_key_task(false,1);
                else {cb.save_host(form_host());ImGui::CloseCurrentPopup();}
            }
            ImGui::SameLine();if(button("取消",ImVec2(footer,layout.button_height),"取消设备")) {discard_key_task();ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        constrain_modal(layout.modal_width_device);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("连接到服务器",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10.f,6.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10.f,6.f));
            begin_modal_body(modal_body_height(500));
            ImGui::BeginDisabled(static_cast<bool>(key_task)||key_ready!=0);
            ImGui::SetNextItemWidth(std::min(180.f,ImGui::GetContentRegionAvail().x));
            if(ImGui::Combo("连接方式",&connection_type,"SSH\0串口\0")) {discard_key_task();clear_connection_secret();form_error.clear();if(connection_type==0&&auth==1&&key[0])start_key_task(false,0);}
            if(connection_type==0) host_fields(true); else serial_fields(m,cb,true);
            ImGui::EndDisabled();
            end_modal_body();
            ImGui::PopStyleVar(2);
            const float footer=footer_pair_width(150);
            const bool connect_clicked=button(connection_type==0?"发起安全连接":"连接串口",ImVec2(footer,layout.button_height),"发起安全连接",ButtonKind::Primary);
            if(connect_clicked&&connection_type==1) {
                if(!valid()) {}
                else if(!cb.serial_connect) form_error="串口连接功能尚不可用。";
                else {
                    net::SerialOptions options; options.port=serial_port.data(); options.baud=static_cast<unsigned>(serial_baud);
                    options.data_bits=static_cast<unsigned>(serial_data_bits); options.stop_bits=static_cast<unsigned>(serial_stop_bits);
                    options.parity=static_cast<net::SerialParity>(serial_parity); options.flow=static_cast<net::SerialFlow>(serial_flow);
                    cb.serial_connect(std::move(options)); ImGui::CloseCurrentPopup();
                }
            }
            if(connection_type==0&&(connect_clicked||key_ready==2)&&valid()&&valid_connection()&&(cb.connect_saved||cb.connect)&&!key_task) {
                if(auth==1&&key_ready!=2) start_key_task(false,2);
                else {
                auto h=form_host();std::string secret(password.data());clear_connection_secret();
                const auto method=static_cast<app::AuthMethod>(auth);
                if(cb.connect_saved) {
                    app::ConnectRequest r; r.host=h;r.password=std::move(secret);r.key_path=key.data();r.method=method;
                    r.save_to_workspace=!h.id&&save_to_workspace;
                    r.policy=auth<2?static_cast<app::RememberPolicy>(remember):app::RememberPolicy::Never;
                    r.save_password=auth<2&&remember!=0&&!r.password.empty();
                    r.use_saved_password=auth<2&&h.id&&has_saved_password&&remember!=0&&r.password.empty();
                    cb.connect_saved(std::move(r));
                } else cb.connect(h,std::move(secret),key.data(),method);
                ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();if(button("取消",ImVec2(footer,layout.button_height),"取消连接")) {discard_key_task();clear_connection_secret();ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        constrain_modal(layout.modal_width_delete);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("关闭会话？",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            ImGui::TextWrapped("确定关闭「%s」的会话？",close_title.c_str());
            ImGui::TextWrapped("关闭后连接和正在进行的传输将被取消。");
            const float footer=footer_pair_width(ImGui::GetContentRegionAvail().x*0.5f);
            if(button("确认关闭",ImVec2(footer,layout.button_height),"确认关闭会话",ButtonKind::Danger)) {
                if(cb.disconnect) cb.disconnect(close_session);
                close_session=0; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if(button("取消",ImVec2(footer,layout.button_height),"取消关闭会话")) {close_session=0; ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        constrain_modal(layout.modal_width_delete);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("删除设备？",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            ImGui::TextWrapped("从设备库删除「%s」？",draft.name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text,tone(theme.muted));
            ImGui::TextWrapped("此操作不可撤销；不会自动关闭已有会话。");
            ImGui::PopStyleColor();
            const float footer=footer_pair_width(ImGui::GetContentRegionAvail().x*0.5f);
            if(button("确认删除",ImVec2(footer,layout.button_height),"确认删除",ButtonKind::Danger)&&cb.delete_host) {cb.delete_host(draft.id);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(button("保留设备",ImVec2(footer,layout.button_height),"保留设备"))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        constrain_modal(layout.modal_width_settings);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("偏好设置",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            begin_modal_body(modal_body_height(300));
            auto s=m.settings;
            bool changed=false;
            label("交互");
            changed=toggle("减少动态效果",&s.reduced_motion,"减少动态效果")||changed;
            changed=toggle("省电模式（空闲时按需绘制）",&s.power_saving,"省电模式")||changed;
            label("动画均为短时过渡，不使用持续粒子、模糊或旋转动画。");
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            label("外观");
            static const char* theme_labels[]={"跟随系统","深色","浅色"};
            const int theme_choice=segmented("主题选择",theme_labels,3,static_cast<int>(s.theme));
            if(theme_choice>=0){s.theme=static_cast<app::ThemeMode>(theme_choice);changed=true;}
            if(changed&&cb.setting)cb.setting(s);
            {
                const std::string effective=std::string("当前外观：")+theme_mode_label(s.theme)+
                    (s.theme==app::ThemeMode::System ? (theme.dark?"（系统 · 深色）":"（系统 · 浅色）"):"");
                label(effective);
            }
            label("界面采用 macOS 风格的中性灰材质、系统蓝强调色与圆角控件。");
            end_modal_body();
            if(button("完成",ImVec2(100,layout.button_height_small),"完成设置",ButtonKind::Primary))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        constrain_modal(layout.modal_width_paste);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("确认粘贴到远端",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            auto target=paste_terminal.lock();
            const app::SessionView* target_session=nullptr;
            for(const auto& s:m.sessions)if(s.id==paste_session&&s.connected&&s.terminal==target)target_session=&s;
            begin_modal_body(modal_body_height(400));
            if(!paste_error.empty())ImGui::TextWrapped("%s",paste_error.c_str());
            else {
                ImGui::TextWrapped("目标：%s",target_session?target_session->title.c_str():"原会话已断开或关闭");
                const auto lines=1+std::count(paste_text.begin(),paste_text.end(),'\n');
                ImGui::Text("%zu 字节 / %zu 行 · 上限 64 千字节",paste_text.size(),static_cast<std::size_t>(lines));
                ImGui::TextColored(tone(theme.warning),"换行可能立即执行命令。请检查来源及全部内容。");
                const float preview_width=std::max(60.f,std::min(layout.paste_preview_width,ImGui::GetContentRegionAvail().x));
                const float preview_height=std::max(60.f,std::min(layout.paste_preview_height,ImGui::GetContentRegionAvail().y-58.f));
                ImGui::BeginChild("粘贴预览",ImVec2(preview_width,preview_height),true,ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::TextUnformatted(paste_text.data(),paste_text.data()+paste_text.size());
                ImGui::EndChild();
                label("仅预览，不会自动发送；确认后向原目标提交一次。");
            }
            end_modal_body();
            const float footer=footer_pair_width(170);
            if(paste_error.empty()) {
                ImGui::BeginDisabled(!target_session||!target);
                if(button("确认，发送一次",ImVec2(footer,layout.button_height),"确认发送",ButtonKind::Danger)) {
                    // Clear retained payload before sending: no retry or double-submit.
                    auto payload=std::move(paste_text); clear_paste();
                    try { target->write_utf8(payload); }
                    catch(const std::exception& e) { paste_error=e.what(); }
                    wipe(payload.data(),payload.size());
                    if(paste_error.empty()){restore_terminal_focus=true;ImGui::CloseCurrentPopup();}
                }
                ImGui::EndDisabled();ImGui::SameLine();
            }
            if(button("取消，不发送",ImVec2(footer,layout.button_height),"取消粘贴")) {clear_paste();restore_terminal_focus=true;ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        constrain_modal(layout.modal_width_trust);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("验证服务器身份",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            begin_modal_body(modal_body_height(260));
            if(m.trust) {
                const auto& t=*m.trust;
                ImGui::TextColored(tone(theme.warning),"%s",t.mismatch?"警告：服务器密钥与已保存记录不一致":"首次连接 · 请独立核对服务器指纹");
                ImGui::TextWrapped("%s:%d  /  %s",t.host.c_str(),t.port,t.algorithm.c_str());
                ImGui::Separator();ImGui::TextWrapped("%s",t.fingerprint.c_str());ImGui::Separator();
                ImGui::TextWrapped("首次信任无法自动证明服务器身份。请通过管理员或其他可信渠道核对完整 SHA-256 指纹，陌生指纹可能代表中间人攻击。");
                if(t.mismatch)ImGui::TextWrapped("为保护连接，本界面不允许绕过密钥不匹配。请取消后由可信渠道调查密钥变更。");
            } else label("验证请求已结束。");
            end_modal_body();
            const float footer=footer_pair_width(210);
            if(m.trust) {
                const auto& t=*m.trust;
                if(!t.mismatch) {
                    if(button("已核对，信任并继续",ImVec2(footer,layout.button_height_large),"信任并继续",ButtonKind::Primary)&&cb.trust) {cb.trust(t.session_id,t.request_id,true);ImGui::CloseCurrentPopup();}
                    ImGui::SameLine();
                }
                if(button("拒绝并取消连接",ImVec2(footer,layout.button_height_large),"拒绝连接")&&cb.trust){cb.trust(t.session_id,t.request_id,false);ImGui::CloseCurrentPopup();}
            } else if(button("关闭",ImVec2(0,layout.button_height),"关闭验证"))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if(!ImGui::IsPopupOpen("连接到服务器"))clear_connection_secret();
        if(!ImGui::IsPopupOpen("连接到服务器")&&!ImGui::IsPopupOpen("设备资料"))discard_key_task();
    }
    static std::string child_path(const std::string& base,const std::string& name) { return base.empty()?name:base+((base.back()=='/'||base.back()=='\\')?"":"/")+name; }
    static std::string parent_path(const std::string& base) {
        if(base.empty()||base=="."||base=="/")return ".";
        std::string s=base;
        while(s.size()>1&&(s.back()=='/'||s.back()=='\\'))s.pop_back();
        const auto pos=s.find_last_of("/\\");
        if(pos==std::string::npos)return ".";
        if(pos==0)return "/";
        if(pos==2&&s.size()>2&&s[1]==':')return s.substr(0,3);
        return s.substr(0,pos);
    }
    struct DraggedFile {
        std::uint64_t session{};
        bool remote=false, directory=false;
        char name[256]{};
        char dir[1200]{};
    };
    void file_drop_target(std::uint64_t session,bool target_remote,const std::string& target_dir,const app::WorkspaceCallbacks& cb) {
        if(!ImGui::BeginDragDropTarget())return;
        if(const ImGuiPayload* payload=ImGui::AcceptDragDropPayload("JIANSSH_FILE")) {
            if(payload->DataSize==sizeof(DraggedFile)) {
                const auto* dragged=static_cast<const DraggedFile*>(payload->Data);
                if(dragged->session==session&&dragged->remote!=target_remote&&!dragged->directory&&cb.file_transfer&&
                   dragged->name[0]&&std::memchr(dragged->name,0,sizeof(dragged->name))&&std::memchr(dragged->dir,0,sizeof(dragged->dir))) {
                    cb.file_transfer(session,!dragged->remote,child_path(dragged->dir,dragged->name),child_path(target_dir,dragged->name));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    void feature_dialogs(const app::WorkspaceModel&,const app::WorkspaceCallbacks& cb) {
        if(operation_pending){operation_pending=false;ImGui::OpenPopup("确认文件操作");begin_modal();}
        const float a=modal_alpha();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,a);
        constrain_modal(layout.modal_width_operation);
        apply_modal_offset(a);
        if(ImGui::BeginPopupModal("确认文件操作",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove)) {
            clamp_modal_to_viewport();
            begin_modal_body(modal_body_height(150));
            ImGui::TextWrapped("%s: %s",operation.remote?"远端":"本地",operation.path.c_str());
            const bool target=operation.operation==app::FileOperation::Mkdir||operation.operation==app::FileOperation::Rename;
            if(target) {begin_frame_widget("新名称");ImGui::InputText("新名称（单个路径分量）",operation_target.data(),operation_target.size());end_frame_widget("新名称");}
            if(operation.operation==app::FileOperation::Chmod) {begin_frame_widget("八进制权限");ImGui::InputText("八进制权限",permissions.data(),permissions.size());end_frame_widget("八进制权限");}
            if(operation.operation==app::FileOperation::Delete)label("永久删除文件；递归删除已禁用。此操作不能撤销。");
            end_modal_body();
            bool valid_target=!target||(operation_target[0]&&std::string(operation_target.data()).find('/')==std::string::npos&&std::string(operation_target.data())!="."&&std::string(operation_target.data())!="..");
            unsigned mode=0;bool valid_mode=true;
            if(operation.operation==app::FileOperation::Chmod){valid_mode=permissions[0]!=0;for(const char* p=permissions.data();*p;++p){if(*p<'0'||*p>'7'){valid_mode=false;break;}mode=mode*8+static_cast<unsigned>(*p-'0');if(mode>07777){valid_mode=false;break;}}}
            const float footer=footer_pair_width(180);
            ImGui::BeginDisabled(!valid_target||!valid_mode||!cb.file_operation);
            if(button("确认执行",ImVec2(footer,layout.button_height),"确认执行",operation.operation==app::FileOperation::Delete?ButtonKind::Danger:ButtonKind::Primary)) {
                operation.permissions=mode;
                if(target){auto base=operation.path;if(operation.operation==app::FileOperation::Rename){const auto pos=base.rfind('/');base=pos==std::string::npos?".":base.substr(0,pos==0?1:pos);}operation.target=child_path(base,operation_target.data());}
                if(operation.operation==app::FileOperation::Mkdir)operation.path=operation.target;
                cb.file_operation(operation);ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();ImGui::SameLine();if(button("取消",ImVec2(footer,layout.button_height),"取消操作"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    void files(const app::SessionView& s,const app::WorkspaceModel& m,const app::WorkspaceCallbacks& cb) {
        if(files_session!=s.id) {
            files_session=s.id;copy(local_path,".");copy(remote_path,".");local_selection.clear();remote_selection.clear();
            if(cb.file_list){cb.file_list(s.id,false,".");cb.file_list(s.id,true,".");}
        }
        ImGui::PushStyleColor(ImGuiCol_Text,tone(theme.muted));
        ImGui::TextWrapped("双栏文件传输 · 点选「.. 上级目录」返回；文件可从本地/远端栏拖到另一栏传输。目标已存在时不覆盖，目录递归传输与递归删除暂不可用。");
        ImGui::PopStyleColor();
        const float width=std::max(100.f,(ImGui::GetContentRegionAvail().x-layout.column_gap)/2);
        // Let the two file columns consume the available height; only the
        // operation buttons and a compact transfer queue stay below.
        const float pane_height=std::max(150.f,ImGui::GetContentRegionAvail().y-172.f);
        for(int side=0;side<2;++side) {
            const bool remote=side==1;auto& path=remote?remote_path:local_path;
            auto& selection=remote?remote_selection:local_selection;auto& directory=remote?remote_directory:local_directory;
            const app::FilePane* pane=nullptr;
            for(const auto& p:m.file_panes)if(p.session_id==s.id&&p.remote==remote)pane=&p;
            if(pane&&!remote&&std::strcmp(path.data(),".")==0&&pane->path!="."&&!pane->path.empty())copy(path,pane->path);
            ImGui::PushID(side);ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_ChildBg,tone(theme.card_bg));
            ImGui::BeginChild("文件栏",ImVec2(width,pane_height),true);
            label(remote?"远端":"本地");
            ImGui::PushID(remote?"远端路径":"本地路径");
            begin_frame_widget("路径");
            ImGui::SetNextItemWidth(-1);
            bool go=ImGui::InputText("##路径",path.data(),path.size(),ImGuiInputTextFlags_EnterReturnsTrue);
            end_frame_widget("路径");
            ImGui::PopID();
            if(button("刷新 / 前往",ImVec2(0,layout.button_height_small),"刷新目录"))go=true;
            ImGui::SameLine();
            auto navigate=[&](const std::string& target){
                if(!cb.file_list)return;
                copy(path,target);selection.clear();cb.file_list(s.id,remote,path.data());
            };
            ImGui::BeginDisabled(!cb.file_list);
            if(button(".. 上级",ImVec2(0,layout.button_height_small),"上级目录"))navigate(parent_path(pane?pane->path:std::string(path.data())));
            ImGui::EndDisabled();
            if(go&&cb.file_list){selection.clear();cb.file_list(s.id,remote,path.data());}
            if(pane) {
                if(pane->loading) {
                    // Stepped loading feedback: the active dot advances on a fixed
                    // cadence, each step is a short transition, never a rotation.
                    const int step=loading.step(now,reduced_motion);
                    ImGui::TextUnformatted("正在读取目录");
                    ImGui::SameLine();
                    for(int i=0;i<3;++i) {
                        const bool on=step==i;
                        ImGui::TextColored(tone(on?theme.accent:theme.muted),"%s",on?"●":"○");
                    }
                    loading_visible=!reduced_motion;
                }
                if(!pane->error.empty())ImGui::TextWrapped("%s",pane->error.c_str());
                ImGui::TextWrapped("当前: %s",pane->path.c_str());
                const std::string up=parent_path(pane->path);
                if(up!=pane->path&&selectable(".. 上级目录",false,0,ImVec2(0,0),"上级目录"))navigate(up);
                for(const auto& e:pane->entries) {
                    ImGui::PushID(e.name.c_str());
                    const std::string title=(e.directory?"[目录] ":(e.symlink?"[链接] ":""))+e.name;
                    const std::string entry_id=std::string(remote?"远端文件:":"本地文件:")+e.name;
                    if(selectable(title.c_str(),selection==e.name,ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0,0),entry_id.c_str())) {
                        selection=e.name;directory=e.directory;
                        if(e.directory&&ImGui::IsMouseDoubleClicked(0)&&cb.file_list){copy(path,child_path(pane->path,e.name));selection.clear();cb.file_list(s.id,remote,path.data());}
                    }
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%llu 字节 · 权限 %04o",static_cast<unsigned long long>(e.size),e.permissions&07777);
                    file_drop_target(s.id,remote,pane->path,cb);
                    if(!e.directory&&e.name.size()<sizeof(DraggedFile::name)&&pane->path.size()<sizeof(DraggedFile::dir)) {
                        if(ImGui::BeginDragDropSource()) {
                            DraggedFile dragged;dragged.session=s.id;dragged.remote=remote;
                            std::snprintf(dragged.name,sizeof(dragged.name),"%s",e.name.c_str());
                            std::snprintf(dragged.dir,sizeof(dragged.dir),"%s",pane->path.c_str());
                            ImGui::SetDragDropPayload("JIANSSH_FILE",&dragged,sizeof(dragged));
                            ImGui::TextUnformatted(title.c_str());
                            ImGui::EndDragDropSource();
                        }
                    }
                    ImGui::PopID();
                }
                const float remaining=ImGui::GetContentRegionAvail().y;
                if(remaining>1.f) {
                    ImGui::Dummy(ImVec2(std::max(1.f,ImGui::GetContentRegionAvail().x),remaining));
                    file_drop_target(s.id,remote,pane->path,cb);
                }
            } else label("尚无目录结果。请刷新。");
            ImGui::EndChild();
            ImGui::PopStyleColor();
            const std::string base=pane?pane->path:std::string(path.data());
            const char* ops[]={"新建目录","重命名","删除","权限"};
            for(int op=0;op<4;++op) {
                ImGui::BeginDisabled(!cb.file_operation||(op>0&&(selection.empty()||selection=="."||selection==".."))||(op==2&&directory));
                if(small_button(ops[op],ops[op])) {
                    operation={s.id,remote,static_cast<app::FileOperation>(op),op==0?base:child_path(base,selection),{},0,false};
                    operation_target.fill(0);copy(permissions,"0644");operation_pending=true;
                }
                ImGui::EndDisabled();if(op!=3)ImGui::SameLine();
            }
            ImGui::EndGroup();ImGui::PopID();if(!remote)ImGui::SameLine();
        }
        const app::FilePane *lp=nullptr,*rp=nullptr;
        for(const auto& p:m.file_panes)if(p.session_id==s.id){if(p.remote)rp=&p;else lp=&p;}
        ImGui::BeginDisabled(!s.connected||!cb.file_transfer||!lp||!rp||local_selection.empty()||local_directory);
        if(button("上传选中文件",ImVec2(0,layout.button_height),"上传文件")&&lp&&rp)cb.file_transfer(s.id,true,child_path(lp->path,local_selection),child_path(rp->path,local_selection));
        ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(!s.connected||!cb.file_transfer||!lp||!rp||remote_selection.empty()||remote_directory);
        if(button("下载选中文件",ImVec2(0,layout.button_height),"下载文件")&&lp&&rp)cb.file_transfer(s.id,false,child_path(rp->path,remote_selection),child_path(lp->path,remote_selection));
        ImGui::EndDisabled();
        ImGui::Separator();label("任务队列（真实字节进度）");
        ImGui::BeginChild("传输队列",ImVec2(0,0),false);
        for(const auto& t:m.transfers)if(t.session_id==s.id) {
            ImGui::PushID(static_cast<int>(t.id));ImGui::TextWrapped("%s %s → %s",t.upload?"上传":"下载",t.source.c_str(),t.destination.c_str());
            const char* states[]={"排队","传输中","完成","失败","已取消"};
            ImGui::Text("%s · %llu / %llu 字节",states[static_cast<int>(t.state)],static_cast<unsigned long long>(t.completed_bytes),static_cast<unsigned long long>(t.total_bytes));
            if(t.total_bytes) {
                // The bar eases towards the reported fraction instead of jumping.
                const float target=static_cast<float>(std::min(1.0,static_cast<double>(t.completed_bytes)/t.total_bytes));
                float& shown=progress_shown[t.id];
                shown=reduced_motion?target:smooth_towards(shown,target,ImGui::GetIO().DeltaTime,motion.progress);
                progress_visible|=std::abs(target-shown)>0.001f;
                ImGui::ProgressBar(shown,ImVec2(-1,layout.progress_bar_height),"");
            }
            if(!t.error.empty())ImGui::TextWrapped("%s",t.error.c_str());
            if((t.state==app::TransferState::Queued||t.state==app::TransferState::Running)&&cb.file_cancel&&small_button("取消任务","取消任务"))cb.file_cancel(t.id);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    static std::string human_rate(double value) {
        char text[40];
        if(value>=1024.0*1024.0)std::snprintf(text,sizeof(text),"%.1f 兆字节/秒",value/(1024.0*1024.0));
        else if(value>=1024.0)std::snprintf(text,sizeof(text),"%.1f 千字节/秒",value/1024.0);
        else std::snprintf(text,sizeof(text),"%.0f 字节/秒",value);
        return text;
    }
    void metrics_bar(const app::SessionView& s,const app::WorkspaceModel& m,const app::WorkspaceCallbacks& cb) {
        (void)cb;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,5));
        ImGui::BeginChild("资源概况",ImVec2(0,layout.metrics_bar_height),false);
        {
            const ImVec2 p=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x+10.f,p.y+1.f),ImVec2(p.x+size.x-10.f,p.y+1.f),fade_color(theme.accent,0.16f));
        }
        const app::MetricsView* data=nullptr;for(const auto& v:m.metrics)if(v.session_id==s.id)data=&v;
        const bool enabled=data&&data->enabled;
        label("资源");
        auto latest=[&](std::optional<double> app::MetricSample::*field) {
            std::optional<double> value;
            if(data)for(const auto& p:data->samples)if(p.timestamp>=data->now-300&&p.timestamp<=data->now&&(p.*field))value=p.*field;
            return value;
        };
        const struct {const char* name;std::optional<double> app::MetricSample::*field;} rows[]={
            {"处理器",&app::MetricSample::cpu_percent},{"内存",&app::MetricSample::memory_percent},{"磁盘",&app::MetricSample::disk_percent},
            {"接收",&app::MetricSample::net_rx_bytes_sec},{"发送",&app::MetricSample::net_tx_bytes_sec}};
        for(int i=0;i<5;++i) {
            ImGui::SameLine();
            const auto value=latest(rows[i].field);
            if(!value)ImGui::TextColored(tone(theme.muted),"%s —",rows[i].name);
            else if(i<3)ImGui::Text("%s %.0f%%",rows[i].name,*value);
            else ImGui::Text("%s %s",rows[i].name,human_rate(*value).c_str());
        }
        if(!enabled) {
            ImGui::SameLine();label(s.connected?"监控启动中":"未连接");
        } else if(data&&!data->error.empty()&&ImGui::GetContentRegionAvail().x>40.f) {
            ImGui::SameLine();
            ImGui::TextColored(tone(theme.muted),"%s",elide(ImGui::GetFont(),ImGui::GetFontSize(),data->error,ImGui::GetContentRegionAvail().x).c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    void terminal(const app::SessionView& s,const app::WorkspaceCallbacks& cb,WorkspaceFonts fonts) {
        if(!s.terminal) {label("终端尚未就绪。连接状态由后台会话提供。");return;}
        ImFont* font=fonts.mono?fonts.mono:ImGui::GetFont();
        ImGui::PushFont(font);
        const float size=ImGui::GetFontSize();
        ImGui::PopFont();
        const float cw=std::max(1.f,font->CalcTextSizeA(size,1000,0,"M").x), ch=size+3;
        const auto avail=ImGui::GetContentRegionAvail();
        const int cols=std::max(2,static_cast<int>((avail.x-layout.terminal_grid_gap)/cw)),rows=std::max(1,static_cast<int>((avail.y-layout.terminal_grid_gap)/ch));
        if((last_cols!=cols||last_rows!=rows||resized_session!=s.id)&&cb.resize) {
            selection_valid=false;selecting=false;
            last_cols=cols;last_rows=rows;resized_session=s.id;cb.resize(s.id,cols,rows);
        }
        auto origin=ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("终端输入区域",ImVec2(std::max(1.f,avail.x),std::max(1.f,avail.y)));
        if(restore_terminal_focus&&!paste_pending&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) {
            ImGui::SetWindowFocus();terminal_focus=true;restore_terminal_focus=false;
        }
        if(ImGui::IsItemClicked()) {terminal_focus=true;ImGui::SetWindowFocus();}
        if(ImGui::IsMouseClicked(0)&&!ImGui::IsItemHovered())terminal_focus=false;
        const bool context_requested=ImGui::IsItemHovered()&&ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if(context_requested){terminal_focus=true;ImGui::OpenPopup("终端操作");}
        auto* dl=ImGui::GetWindowDrawList();
        const float fade=page_alpha;
        dl->AddRectFilled(origin,ImVec2(origin.x+avail.x,origin.y+avail.y),fade_color(theme.terminal_bg,fade),layout.terminal_rounding);
        const ImVec2 p(origin.x+layout.terminal_padding,origin.y+layout.terminal_padding);
        dl->PushClipRect(origin,ImVec2(origin.x+avail.x,origin.y+avail.y),true);
        // Bounded screen-only snapshot: never copies/scans terminal history.
        auto snap=s.terminal->snapshot();
        if(selection_session!=s.id) {selection_session=s.id;selection_valid=false;selecting=false;}
        const int grid_rows=std::min(rows,snap.rows),grid_cols=std::min(cols,snap.cols);
        auto cell_at_mouse=[&] {
            const auto mouse=ImGui::GetIO().MousePos;
            const int r=std::clamp(static_cast<int>((mouse.y-p.y)/ch),0,std::max(0,grid_rows-1));
            const int c=std::clamp(static_cast<int>((mouse.x-p.x)/cw),0,std::max(0,grid_cols-1));
            return r*grid_cols+c;
        };
        if(grid_rows>0&&grid_cols>0) {
            if(ImGui::IsItemClicked()) {selection_anchor=selection_end=cell_at_mouse();selecting=true;selection_valid=false;}
            if(selecting&&ImGui::IsMouseDown(0)) {selection_end=cell_at_mouse();selection_valid=selection_end!=selection_anchor;}
            if(selecting&&ImGui::IsMouseReleased(0))selecting=false;
        }
        const int selection_first=std::min(selection_anchor,selection_end),selection_last=std::max(selection_anchor,selection_end);
        auto copy_selection=[&] {
            if(!selection_valid||grid_cols<=0)return;
            std::string text;
            for(int r=selection_first/grid_cols;r<=selection_last/grid_cols&&r<grid_rows;++r) {
                std::string line;
                const int first=r==selection_first/grid_cols?selection_first%grid_cols:0;
                const int last=r==selection_last/grid_cols?selection_last%grid_cols:grid_cols-1;
                for(int c=first;c<=last&&c<static_cast<int>(snap.lines[r].size());++c) {
                    const auto& cell=snap.lines[r][c];
                    if(cell.width==0) {if(c==first&&c>0)line+=snap.lines[r][c-1].text;continue;}
                    line+=cell.text.empty()?" ":cell.text;
                }
                while(!line.empty()&&line.back()==' ')line.pop_back();
                if(r!=selection_first/grid_cols)text+='\n';
                text+=line;
            }
            ImGui::SetClipboardText(text.c_str());
        };
        for(int r=0;r<std::min(rows,snap.rows);++r) {
            if(r>=static_cast<int>(snap.lines.size()))break;
            const auto& line=snap.lines[r];
            for(int c=0;c<std::min(cols,static_cast<int>(line.size()));++c) {
                const auto& cell=line[c];if(cell.width==0)continue;
                auto fg=term_color(cell.fg,true),bg=term_color(cell.bg,false);if(cell.style.reverse)std::swap(fg,bg);
                fg=fade_color(fg,fade);bg=fade_color(bg,fade);
                const ImVec2 q(p.x+c*cw,p.y+r*ch),end(q.x+cw*std::max(1,cell.width),q.y+ch);
                dl->AddRectFilled(q,end,bg);
                if(selection_valid&&r*grid_cols+c<=selection_last&&r*grid_cols+c+std::max(1,cell.width)-1>=selection_first)
                    dl->AddRectFilled(q,end,IM_COL32(100,145,210,110));
                if(!cell.style.conceal&&!cell.text.empty()) {
                    dl->AddText(font,size,q,fg,cell.text.c_str());
                    if(cell.style.bold)dl->AddText(font,size,ImVec2(q.x+.5f,q.y),fg,cell.text.c_str());
                    if(cell.style.underline)dl->AddLine(ImVec2(q.x,end.y-2),ImVec2(end.x,end.y-2),fg);
                    if(cell.style.strike)dl->AddLine(ImVec2(q.x,q.y+ch*.55f),ImVec2(end.x,q.y+ch*.55f),fg);
                }
            }
        }
        const auto& cur=snap.cursor;
        if(cur.visible&&cur.row>=0&&cur.row<rows&&cur.col>=0&&cur.col<cols) {
            const ImVec2 q(p.x+cur.col*cw,p.y+cur.row*ch);
            const ImU32 cursor_color=fade_color(theme.terminal_cursor,fade);
            // Match the glyph height instead of the full cell box; the block and
            // outline cursors read as text markers, not oversized frames.
            constexpr float inset=3.f;
            if(cur.shape==2)dl->AddRectFilled(ImVec2(q.x,q.y+ch-2),ImVec2(q.x+cw,q.y+ch),cursor_color);
            else if(cur.shape==3)dl->AddRectFilled(ImVec2(q.x,q.y+inset),ImVec2(q.x+2,q.y+ch-inset),cursor_color);
            else dl->AddRect(ImVec2(q.x,q.y+inset),ImVec2(q.x+cw,q.y+ch-inset),fade_color(terminal_focus?theme.terminal_cursor:theme.muted,fade));
        }
        dl->PopClipRect();
        if(ImGui::BeginPopup("终端操作")) {
            terminal_menu_open=true;
            if(ImGui::MenuItem("复制选中内容", "Ctrl+Shift+C", false, selection_valid)) {copy_selection();restore_terminal_focus=true;}
            if(ImGui::MenuItem("粘贴", "Ctrl+Shift+V", false, s.connected)) {request_paste(s);}
            ImGui::EndPopup();
        } else if(terminal_menu_open) {terminal_menu_open=false;if(!paste_pending&&!ImGui::IsMouseClicked(0))restore_terminal_focus=true;}
        if(!terminal_focus||!s.connected||ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)||!ImGui::IsWindowFocused())return;
        if(page.phase()!=PageTransition::Phase::Idle)return;
        terminal_input_active=true;
        auto& io=ImGui::GetIO();
        app::Modifier mod=app::Modifier::None;
        if(io.KeyCtrl) mod=mod|app::Modifier::Ctrl;
        if(io.KeyAlt) mod=mod|app::Modifier::Alt;
        if(io.KeyShift) mod=mod|app::Modifier::Shift;
        struct Map {ImGuiKey imgui;app::Key terminal;};
        static constexpr Map keys[]={{ImGuiKey_Enter,app::Key::Enter},{ImGuiKey_KeypadEnter,app::Key::Enter},{ImGuiKey_Tab,app::Key::Tab},{ImGuiKey_Backspace,app::Key::Backspace},{ImGuiKey_Escape,app::Key::Escape},{ImGuiKey_UpArrow,app::Key::Up},{ImGuiKey_DownArrow,app::Key::Down},{ImGuiKey_LeftArrow,app::Key::Left},{ImGuiKey_RightArrow,app::Key::Right},{ImGuiKey_Insert,app::Key::Insert},{ImGuiKey_Delete,app::Key::Delete},{ImGuiKey_Home,app::Key::Home},{ImGuiKey_End,app::Key::End},{ImGuiKey_PageUp,app::Key::PageUp},{ImGuiKey_PageDown,app::Key::PageDown}};
        if(io.KeyCtrl&&io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_C,false)) {
            copy_selection();
        }
        else if((io.KeyCtrl&&io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_V,false)) || (io.KeyShift&&!io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Insert,false))) request_paste(s);
        else {
            for(const auto& k:keys)if(ImGui::IsKeyPressed(k.imgui))s.terminal->key(k.terminal,mod);
            for(int i=0;i<12;++i)if(ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1+i)))s.terminal->function_key(i+1,mod);
            if(io.KeyCtrl) {
                for(int i=0;i<26;++i)if(ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A+i)))s.terminal->unichar(U'a'+i,mod);
                if(ImGui::IsKeyPressed(ImGuiKey_LeftBracket))s.terminal->unichar(U'[',mod);
                if(ImGui::IsKeyPressed(ImGuiKey_RightBracket))s.terminal->unichar(U']',mod);
                if(ImGui::IsKeyPressed(ImGuiKey_Backslash))s.terminal->unichar(U'\\',mod);
                if(ImGui::IsKeyPressed(ImGuiKey_Space))s.terminal->unichar(U' ',mod);
            } else for(int i=0;i<io.InputQueueCharacters.Size;++i) {
                // SDL text events already incorporate Shift and keyboard layout.
                const auto c=io.InputQueueCharacters[i];if(c>=32&&c!=127)s.terminal->unichar(c,io.KeyAlt?app::Modifier::Alt:app::Modifier::None);
            }
        }
        io.WantCaptureKeyboard=true;io.WantTextInput=true;
    }
};
Workspace::Workspace():impl_(std::make_unique<Impl>()) {}
Workspace::~Workspace()=default;
bool Workspace::terminal_input_active()const noexcept { return impl_->terminal_input_active; }
bool Workspace::animation_active()const noexcept {
    const auto& v=*impl_;
    return v.entrance.active()||v.selection.active()||v.hover.active()||v.modal.active()||
           v.theme_fade.active()||v.notice_fade.active()||v.hover_anim.active()||v.toggle_anim.active()||
           v.page.active()||v.loading_visible||v.progress_visible||static_cast<bool>(v.key_task);
}
Rgba Workspace::background_color()const noexcept {
    const auto c=ImGui::ColorConvertU32ToFloat4(impl_->theme.window_bg);
    return Rgba{c.x,c.y,c.z,c.w};
}
bool Workspace::theme_is_dark()const noexcept { return impl_->theme.dark; }

void Workspace::draw(const app::WorkspaceModel& m,const app::WorkspaceCallbacks& cb,WorkspaceFonts fonts) {
    auto& v=*impl_;const auto now=MotionClock::now();
    v.terminal_input_active=false;
    v.now=now;
    v.reduced_motion=m.settings.reduced_motion;
    v.resolve_theme(m);
    if(v.first){v.first=false;v.entrance.start(0,1,now,std::chrono::duration<double>(motion.entrance),Easing::EaseOutQuint,v.reduced_motion);}
    const float entry=static_cast<float>(v.entrance.update(now,v.reduced_motion));
    // The sidebar trails the content slightly during the entrance: a subtle
    // stagger reads as depth without lengthening the overall animation.
    const float entry_side=std::clamp((entry-0.12f)/0.88f,0.f,1.f);
    v.selection.update(now,v.reduced_motion);v.hover.update(now,v.reduced_motion);
    // A new notice fades in; the status bar never blinks.
    if(m.notice!=v.notice_text){v.notice_text=m.notice;v.notice_fade.start(0,1,now,std::chrono::duration<double>(motion.notice),Easing::EaseOutCubic,v.reduced_motion);}
    const float notice_alpha=static_cast<float>(v.notice_fade.update(now,v.reduced_motion));
    // Resolve the active session before drawing; the central page transition
    // needs to know its target key before any content is emitted.
    const app::SessionView* active=nullptr;
    for(const auto& s:m.sessions)if(s.id==v.selected){active=&s;break;}
    if(!active&&!m.sessions.empty()){active=&m.sessions.back();v.selected=active->id;}
    if(active) {
        if(v.view>1)v.view=1;
        if(!(active->type==app::SessionType::Ssh&&active->capabilities.file_transfer))v.view=0;
    }
    const std::string desired_page=active?("s"+std::to_string(active->id)+":v"+std::to_string(v.view)):"welcome";
    if(!v.page_ready){v.page.reset(desired_page);v.page_ready=true;}
    v.page.update(desired_page,now,motion.page_out,motion.page_in,v.reduced_motion);
    if(v.page.displayed()=="welcome"){v.page_session=0;v.page_view=0;}
    else {unsigned long long id=0;int view=0;if(std::sscanf(v.page.displayed().c_str(),"s%llu:v%d",&id,&view)==2){v.page_session=static_cast<std::uint64_t>(id);v.page_view=view;}}
    v.page_alpha=v.page.alpha();
    v.page_offset=v.page.phase()==PageTransition::Phase::In
        ?v.page.incoming_offset(static_cast<float>(motion.page_slide))
        :-v.page.outgoing_offset(static_cast<float>(motion.page_out_slide));
    v.loading_visible=false;
    v.progress_visible=false;
    if(fonts.regular)ImGui::PushFont(fonts.regular);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(layout.window_padding_x,layout.window_padding_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(layout.frame_padding_x,layout.frame_padding_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(layout.item_spacing,layout.item_spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,layout.frame_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,layout.child_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,layout.popup_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,layout.window_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign,ImVec2(0.5f,0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,layout.scrollbar_size);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding,layout.scrollbar_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,layout.grab_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,layout.grab_min_size);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding,layout.tab_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,layout.disabled_alpha);
    push_theme(v.theme);
    auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("JianSSH 工作空间",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    {
        auto* dl=ImGui::GetWindowDrawList();
        const ImVec2 p=viewport->Pos,q(viewport->Pos.x+viewport->Size.x,viewport->Pos.y+viewport->Size.y);
        const ImU32 top=v.theme.dark?lerp_color(v.theme.window_bg,v.theme.accent,0.045f)
                                        :lerp_color(v.theme.window_bg,IM_COL32(255,255,255,255),0.62f);
        const ImU32 bottom=v.theme.dark?lerp_color(v.theme.window_bg,IM_COL32(0,0,0,255),0.10f)
                                      :v.theme.window_bg;
        dl->AddRectFilledMultiColor(p,q,top,top,bottom,bottom);
    }
    const auto full=ImGui::GetContentRegionAvail();
    // Column widths always add up to the space the workspace was given.
    const Regions regions=compute_regions(full.x,layout);
    const float height=region_height(full.y,layout);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,tone(v.theme.sidebar_bg));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,.30f+.70f*entry_side);
    ImGui::BeginChild("设备导航",ImVec2(regions.sidebar,height),true);
    v.panel_depth(0.045f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+8.f*(1.f-entry_side));
    const ImVec2 brand=ImGui::GetCursorScreenPos();
    v.draw_brand_icon(brand,30.f);
    ImGui::Dummy(ImVec2(30,30));ImGui::SameLine(0,11.f);
    ImGui::BeginGroup();
    if(fonts.heading)ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted("JianSSH");
    if(fonts.heading)ImGui::PopFont();
    ImGui::TextColored(tone(v.theme.muted),"安全连接，从容抵达");
    ImGui::EndGroup();
    ImGui::Spacing();ImGui::Spacing();
    v.search_field("搜索名称、地址或用户",v.search.data(),v.search.size(),"搜索设备");
    ImGui::Spacing();
    if(v.button("＋ 新增设备",ImVec2(-1,layout.button_height),"新增设备")) {v.prepare_connection({},m);v.edit_pending=true;}
    ImGui::Spacing();ImGui::Spacing();
    v.label("工作空间");
    const float item_width=std::max(80.f,ImGui::GetContentRegionAvail().x-4.f);
    if(v.selectable("全部设备",v.group.empty(),0,ImVec2(item_width,layout.select_height),"分组全部"))v.group.clear();
    std::set<std::string> groups;for(const auto& h:m.hosts)if(!h.group.empty())groups.insert(h.group);
    for(const auto& g:groups)if(v.selectable(g.c_str(),v.group==g,0,ImVec2(item_width,layout.select_height),("分组"+g).c_str()))v.group=g;
    ImGui::Spacing();v.label("设备列表");
    ImGui::BeginChild("设备滚动列表",ImVec2(0,std::max(layout.device_list_min_height,ImGui::GetContentRegionAvail().y-layout.device_list_reserve)),false);
    if(m.hosts.empty()) {
        ImGui::Dummy(ImVec2(0,16.f));
        ImGui::TextColored(tone(v.theme.muted),"还没有设备");
        ImGui::TextWrapped("新增一个设备，或使用快速连接开始。");
    }
    bool any_hover=false;
    ImGuiListClipper clipper;
    std::vector<const store::Host*> filtered;
    for(const auto& h:m.hosts) {
        if(!v.group.empty()&&h.group!=v.group)continue;
        bool match=!v.search[0]||h.name.find(v.search.data())!=std::string::npos||h.hostname.find(v.search.data())!=std::string::npos||h.username.find(v.search.data())!=std::string::npos||h.serial_port.find(v.search.data())!=std::string::npos;
        if(!match)for(const auto& tag:h.tags)if(tag.find(v.search.data())!=std::string::npos){match=true;break;}
        if(match)filtered.push_back(&h);
    }
    // The clipper step must equal the drawn row height, otherwise the visible
    // row estimate drifts and the list scrolls unevenly.
    clipper.Begin(static_cast<int>(filtered.size()),static_cast<int>(layout.device_row_height));
    while(clipper.Step())for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) {
        const auto& h=*filtered[i];ImGui::PushID(static_cast<int>(h.id));
        auto pos=ImGui::GetCursorScreenPos();
        const float row_width=ImGui::GetContentRegionAvail().x;
        const std::string row_id="设备行"+std::to_string(h.id);
        const float hover_value=v.hover_anim.factor(row_id,now,v.reduced_motion);
        if(v.selectable("##设备",h.id==v.selected_host,ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0,layout.device_row_height),row_id.c_str())) {
            v.selected_host=h.id;
            v.selection.start(0,1,now,std::chrono::duration<double>(motion.selection),Easing::EaseOutCubic,v.reduced_motion);
            v.prepare_connection(h,m);
            if(ImGui::IsMouseDoubleClicked(0))v.connect_pending=true;
        }
        auto* dl=ImGui::GetWindowDrawList();
        const bool selected=h.id==v.selected_host;
        if(selected) {
            dl->AddRectFilled(pos,ImVec2(pos.x+row_width,pos.y+layout.device_row_height),v.theme.header,layout.device_outline_rounding);
            dl->AddRect(pos,ImVec2(pos.x+row_width,pos.y+layout.device_row_height),fade_color(v.theme.accent,0.30f),layout.device_outline_rounding,0,1.f);
        } else if(hover_value>0.001f) {
            dl->AddRectFilled(pos,ImVec2(pos.x+row_width,pos.y+layout.device_row_height),
                fade_color(v.theme.ink,v.theme.dark?0.055f*hover_value:0.045f*hover_value),layout.device_outline_rounding);
        }
        const bool over=ImGui::IsItemHovered();any_hover|=over;
        if(over&&!selected)
            dl->AddRect(pos,ImVec2(pos.x+row_width,pos.y+layout.device_row_height),fade_color(v.theme.border,0.7f*hover_value),layout.device_outline_rounding,0,1.f);
        const float avatar=layout.device_avatar_size;
        v.draw_avatar(ImVec2(pos.x+10.f,pos.y+(layout.device_row_height-avatar)*0.5f),avatar,h.name,static_cast<std::uint64_t>(h.id));
        const float text_width=std::max(20.f,row_width-layout.device_row_text_x-12.f);
        const float font_size=ImGui::GetFontSize();
        ImFont* row_font=ImGui::GetFont();
        dl->AddText(ImVec2(pos.x+layout.device_row_text_x,pos.y+layout.device_row_title_dy),v.theme.ink,elide(row_font,font_size,h.name,text_width).c_str());
        const std::string address=h.connection_type=="serial"?"串口 · "+h.serial_port:h.username+"@"+h.hostname;
        dl->AddText(ImVec2(pos.x+layout.device_row_text_x,pos.y+layout.device_row_address_dy),v.theme.muted,elide(row_font,font_size,address,text_width).c_str());
        if(ImGui::BeginPopupContextItem("设备操作")) {
            if(ImGui::MenuItem("连接")){v.prepare_connection(h,m);v.connect_pending=true;}
            if(ImGui::MenuItem("编辑设备")){v.prepare_connection(h,m);v.edit_pending=true;}
            if(ImGui::MenuItem("删除设备")){v.prepare_connection(h,m);v.delete_pending=true;}
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if(any_hover!=v.hovered){v.hovered=any_hover;v.animate(v.hover,any_hover?1:0,m,motion.hover_in);}
    ImGui::EndChild();
    ImGui::Spacing();
    if(v.button("偏好设置",ImVec2(-1,layout.button_height_small),"偏好设置",ButtonKind::Quiet))v.settings_pending=true;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine(0,layout.column_gap);
    ImGui::BeginChild("连接工作区",ImVec2(regions.central,height),true);
    v.panel_depth(0.030f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+6.f*(1.f-entry));
    const float header_width=ImGui::GetContentRegionAvail().x;
    const float content_right=ImGui::GetWindowContentRegionMax().x;
    const bool show_connect=header_width>=430.f;
    const bool full_actions=header_width>=640.f;
    // Toolbar: large title left, commands aligned to the right edge.
    if(fonts.heading)ImGui::PushFont(fonts.heading);
    ImGui::TextUnformatted("连接工作台");
    if(fonts.heading)ImGui::PopFont();
    const float title_end=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
    const store::Host* chosen=nullptr;for(const auto& h:m.hosts)if(h.id==v.selected_host){chosen=&h;break;}
    constexpr float quick_w=110.f, connect_w=96.f, small_w=52.f;
    const float actions_width=quick_w+(show_connect?8.f+connect_w:0.f)+(full_actions?16.f+small_w*2.f:0.f);
    ImGui::SameLine(std::max(title_end+22.f,content_right-actions_width));
    if(v.button("＋ 快速连接",ImVec2(quick_w,layout.button_height_small),"快速连接",ButtonKind::Primary)){v.prepare_connection({},m);v.connect_pending=true;}
    ImGui::BeginDisabled(!chosen);
    if(show_connect) {
        ImGui::SameLine(0,8.f);
        if(v.button("连接设备",ImVec2(connect_w,layout.button_height_small),"连接设备")&&chosen){v.prepare_connection(*chosen,m);v.connect_pending=true;}
    }
    if(full_actions) {
        ImGui::SameLine(0,8.f);
        if(v.button("编辑",ImVec2(small_w,layout.button_height_small),"编辑设备")&&chosen){v.prepare_connection(*chosen,m);v.edit_pending=true;}
        ImGui::SameLine(0,8.f);
        if(v.button("删除",ImVec2(small_w,layout.button_height_small),"删除设备")&&chosen){v.prepare_connection(*chosen,m);v.delete_pending=true;}
    }
    ImGui::EndDisabled();
    {
        const ImVec2 line_start=ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(line_start.x,line_start.y+3.f),
            ImVec2(line_start.x+ImGui::GetContentRegionAvail().x,line_start.y+3.f),fade_color(v.theme.separator,0.55f));
        ImGui::Dummy(ImVec2(0,8.f));
    }
    auto session_tabs=[&]{
        ImGui::BeginChild("会话标签",ImVec2(0,layout.tab_bar_height),false,ImGuiWindowFlags_HorizontalScrollbar);
        for(const auto& s:m.sessions) {
            ImGui::PushID(static_cast<int>(s.id));
            const std::string title=std::string(s.type==app::SessionType::Serial?"串口 · ":"")+s.title;
            constexpr float text_padding=12.f, dot_gap=7.f, close_width=20.f;
            const float tab_width=std::clamp(6.f+dot_gap+ImGui::CalcTextSize(title.c_str()).x+text_padding*2.f+6.f+close_width,layout.tab_min_width,layout.tab_max_width);
            const ImVec2 tab_pos=ImGui::GetCursorScreenPos();
            auto* dl=ImGui::GetWindowDrawList();
            const std::string tab_id="会话标签"+std::to_string(s.id);
            const float hover=v.hover_anim.factor(tab_id,now,v.reduced_motion);
            if(v.selected==s.id) {
                dl->AddRectFilled(ImVec2(tab_pos.x+1.f,tab_pos.y+2.f),ImVec2(tab_pos.x+tab_width+1.f,tab_pos.y+layout.tab_height+2.f),fade_color(v.theme.shadow,0.55f),layout.tab_rounding);
                dl->AddRectFilled(tab_pos,ImVec2(tab_pos.x+tab_width,tab_pos.y+layout.tab_height),v.theme.tab_selected,layout.tab_rounding);
                dl->AddRect(tab_pos,ImVec2(tab_pos.x+tab_width,tab_pos.y+layout.tab_height),fade_color(v.theme.border,0.9f),layout.tab_rounding,0,1.f);
            } else if(hover>0.01f) {
                dl->AddRectFilled(tab_pos,ImVec2(tab_pos.x+tab_width,tab_pos.y+layout.tab_height),
                    fade_color(v.theme.ink,v.theme.dark?0.06f*hover:0.045f*hover),layout.tab_rounding);
            }
            const std::string visible_title=elide(ImGui::GetFont(),ImGui::GetFontSize(),title,tab_width-text_padding*2.f-6.f-dot_gap-close_width);
            const float dot_x=tab_pos.x+text_padding+3.f;
            const float center_y=tab_pos.y+layout.tab_height*0.5f;
            dl->AddCircleFilled(ImVec2(dot_x,center_y),3.f,s.connected?v.theme.success:v.theme.muted);
            if(s.connected)dl->AddCircle(ImVec2(dot_x,center_y),5.5f,fade_color(v.theme.success,0.18f),16,1.f);
            dl->AddText(ImVec2(dot_x+3.f+dot_gap,tab_pos.y+(layout.tab_height-ImGui::GetFontSize())*0.5f),
                v.selected==s.id?v.theme.ink:v.theme.muted,visible_title.c_str());
            const float close_left=tab_pos.x+tab_width-text_padding-close_width+2.f;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,0));
            ImGui::SetCursorScreenPos(tab_pos);
            if(ImGui::InvisibleButton("##选择会话",ImVec2(close_left-tab_pos.x,layout.tab_height))) {
                v.selected=s.id;v.selection.start(0,1,now,std::chrono::duration<double>(motion.selection),Easing::EaseOutCubic,v.reduced_motion);v.terminal_focus=false;
            }
            const bool tab_hovered=ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(ImVec2(close_left,tab_pos.y));
            if(ImGui::InvisibleButton("##关闭会话",ImVec2(close_width,layout.tab_height))) {v.close_session=s.id;v.close_title=s.title;v.close_pending=true;}
            const bool close_hovered=ImGui::IsItemHovered();
            const ImVec2 close_center(close_left+close_width*0.5f,center_y);
            if(close_hovered)dl->AddCircleFilled(close_center,9.f,fade_color(v.theme.ink,0.10f),20);
            const ImU32 cross=close_hovered?v.theme.ink:fade_color(v.theme.muted,0.85f);
            dl->AddLine(ImVec2(close_center.x-3.f,close_center.y-3.f),ImVec2(close_center.x+3.f,close_center.y+3.f),cross,1.25f);
            dl->AddLine(ImVec2(close_center.x+3.f,close_center.y-3.f),ImVec2(close_center.x-3.f,close_center.y+3.f),cross,1.25f);
            ImGui::PopStyleVar();
            if(close_hovered)ImGui::SetTooltip("断开或关闭此会话");
            v.hover_anim.observe(tab_id,tab_hovered,now,v.reduced_motion);
            ImGui::SameLine(0,layout.tab_gap);ImGui::PopID();
        }
        ImGui::EndChild();
    };
    if(!m.sessions.empty())session_tabs();
    auto welcome_page=[&]{
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+std::max(0.f,v.page_offset));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,v.page_alpha);
        ImGui::BeginChild("欢迎卡片",ImVec2(0,0),false);
        const ImVec2 region=ImGui::GetContentRegionAvail();
        const float block=std::clamp(region.x-48.f,300.f,580.f);
        const float left_base=ImGui::GetCursorPosX();
        const float left=(region.x-block)*0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+std::max(layout.welcome_top_min,region.y*layout.welcome_top_ratio));
        const ImVec2 icon_pos(ImGui::GetCursorScreenPos().x+left+(block-58.f)*0.5f,ImGui::GetCursorScreenPos().y);
        auto* dl=ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(icon_pos.x+29.f,icon_pos.y+29.f),39.f,fade_color(v.theme.accent,0.10f),48);
        v.draw_brand_icon(icon_pos,58.f);
        ImGui::Dummy(ImVec2(block,64.f));
        const char* welcome_title="下一段旅程，从安全连接开始";
        float title_width=0.f;
        if(fonts.heading) {
            ImGui::PushFont(fonts.heading);
            title_width=fonts.heading->CalcTextSizeA(fonts.heading->FontSize,1000.f,0.f,welcome_title).x;
        } else title_width=ImGui::CalcTextSize(welcome_title).x;
        ImGui::SetCursorPosX(left_base+left+(block-title_width)*0.5f);
        ImGui::TextUnformatted(welcome_title);
        if(fonts.heading)ImGui::PopFont();
        ImGui::Spacing();
        ImGui::SetCursorPosX(left_base+left);
        ImGui::PushTextWrapPos(left_base+left+block);
        ImGui::TextColored(tone(v.theme.muted),"欢迎使用 JianSSH。设备库目前%s。这里不展示模拟服务器或虚构的监控数据。",m.hosts.empty()?"为空":"已就绪");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();ImGui::Spacing();
        const char* steps[]={"新增设备或使用快速连接","独立核对首次连接的服务器指纹","在真实终端中开始工作"};
        const float steps_block=std::min(block,390.f);
        const ImVec2 steps_origin=ImGui::GetCursorScreenPos();
        const float steps_x=steps_origin.x+(block-steps_block)*0.5f;
        for(int i=0;i<3;++i) {
            const float y=steps_origin.y+i*32.f;
            dl->AddCircleFilled(ImVec2(steps_x+11.f,y+11.f),11.f,fade_color(v.theme.accent,0.13f),24);
            dl->AddCircle(ImVec2(steps_x+11.f,y+11.f),11.f,fade_color(v.theme.accent,0.35f),24,1.f);
            char number[4];std::snprintf(number,sizeof(number),"%02d",i+1);
            const float nw=ImGui::CalcTextSize(number).x;
            dl->AddText(ImVec2(steps_x+11.f-nw*0.5f,y+11.f-ImGui::GetFontSize()*0.5f),v.theme.accent,number);
            dl->AddText(ImVec2(steps_x+34.f,y+2.f),v.theme.ink,steps[i]);
        }
        ImGui::Dummy(ImVec2(block,3*32.f));
        ImGui::SetCursorPosX(left_base+left+(block-layout.welcome_button_width)*0.5f);
        if(v.button("开始第一次连接",ImVec2(layout.welcome_button_width,layout.button_height_xl),"开始第一次连接",ButtonKind::Primary)){v.prepare_connection(chosen?*chosen:store::Host{},m);v.connect_pending=true;}
        ImGui::Spacing();ImGui::Spacing();
        ImGui::SetCursorPosX(left_base+left);
        ImGui::PushTextWrapPos(left_base+left+block);
        ImGui::TextColored(tone(v.theme.muted),"设备配置保存在本机；密码不写入设备库。连接与认证在后台执行，界面不会阻塞等待网络。");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    };
    if(active) {
        const bool can_files=active->type==app::SessionType::Ssh&&active->capabilities.file_transfer;
        const char* disconnect_label=active->connected?"断开连接":"结束会话";
        static const char* view_labels[]={"终端","文件传输"};
        const float seg_width=can_files?v.segmented_width(view_labels,2):0.f;
        const float disconnect_width=ImGui::CalcTextSize(disconnect_label).x+24.f;
        const float right_width=seg_width+(can_files?10.f:0.f)+disconnect_width;
        const float status_max=std::max(40.f,ImGui::GetContentRegionAvail().x-right_width-28.f);
        const ImVec2 status_pos=ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(14,18));ImGui::SameLine(0,0);
        {
            auto* dl=ImGui::GetWindowDrawList();
            const ImU32 dot=active->connected?v.theme.success:v.theme.muted;
            dl->AddCircleFilled(ImVec2(status_pos.x+5,status_pos.y+9),3.2f,dot);
            if(active->connected)dl->AddCircle(ImVec2(status_pos.x+5,status_pos.y+9),6.f,fade_color(dot,0.20f),18,1.f);
        }
        v.label(elide(ImGui::GetFont(),ImGui::GetFontSize(),active->status,status_max));
        const float label_end=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
        ImGui::SameLine(std::max(label_end+16.f,content_right-right_width));
        if(can_files) {
            const int clicked=v.segmented("视图切换",view_labels,2,v.view);
            if(clicked>=0)v.view=clicked;
            ImGui::SameLine(0,10.f);
        }
        if(v.button(disconnect_label,ImVec2(disconnect_width,layout.button_height_small),"断开会话")) {
            v.close_session=active->id;v.close_title=active->title;v.close_pending=true;
        }
        if(!active->error.empty()) {
            if(active->type==app::SessionType::Serial) v.serial_error("##串口会话错误",active->error);
            else ImGui::TextColored(tone(v.theme.danger),"%s",active->error.c_str());
        }
        ImGui::Spacing();
        const app::SessionView* shown=nullptr;
        if(v.page_session)for(const auto& s:m.sessions)if(s.id==v.page_session)shown=&s;
        if(!shown) welcome_page();
        else {
            const bool shown_monitor=shown->type==app::SessionType::Ssh&&shown->capabilities.monitor;
            const float bar_space=shown_monitor?layout.metrics_bar_height+layout.metrics_bar_gap:0.f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+std::clamp(v.page_offset,-18.f,18.f));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,v.page_alpha);
            const float view_height=std::max(40.f,ImGui::GetContentRegionAvail().y-bar_space);
            ImGui::BeginChild("会话视图",ImVec2(0,view_height),false);
            if(v.page_view==0)v.terminal(*shown,cb,fonts); else v.files(*shown,m,cb);
            ImGui::EndChild();
            if(shown_monitor) {ImGui::Dummy(ImVec2(0,layout.metrics_bar_gap));v.metrics_bar(*shown,m,cb);}
            ImGui::PopStyleVar();
        }
    } else {
        welcome_page();
    }
    ImGui::EndChild();
    // Status bar: a hairline and a quiet summary, matching a macOS footer.
    {
        const ImVec2 status_origin=ImGui::GetCursorScreenPos();
        const float status_width=ImGui::GetContentRegionAvail().x;
        auto* dl=ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(status_origin.x,status_origin.y+1.f),ImVec2(status_origin.x+status_width,status_origin.y+1.f),fade_color(v.theme.separator,0.65f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+6.f);
        const ImVec2 row=ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(row.x+4.f,row.y+9.f),3.f,v.theme.accent);
        ImGui::Dummy(ImVec2(12,18));ImGui::SameLine();
        ImGui::TextColored(tone(v.theme.muted),"%zu 台设备  ·  %zu 个会话",m.hosts.size(),m.sessions.size());
        const float left_end=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
        std::string theme_status=std::string("外观：")+theme_mode_label(m.settings.theme);
        if(m.settings.theme==app::ThemeMode::System)theme_status+=v.theme.dark?" · 深色":" · 浅色";
        const std::string mode_status=m.settings.power_saving?"按需绘制 · 省电":"标准模式";
        const float right_width=ImGui::CalcTextSize(mode_status.c_str()).x+18.f+ImGui::CalcTextSize(theme_status.c_str()).x;
        const float right_start=std::max(left_end+20.f,ImGui::GetWindowContentRegionMax().x-right_width);
        if(!m.notice.empty()) {
            const float notice_room=right_start-left_end-30.f;
            if(notice_room>40.f) {
                const std::string notice=elide(ImGui::GetFont(),ImGui::GetFontSize(),m.notice,notice_room);
                dl->AddText(ImGui::GetFont(),ImGui::GetFontSize(),ImVec2(ImGui::GetWindowPos().x+left_end+18.f,row.y+1.f),
                    fade_color(v.theme.muted,notice_alpha),notice.c_str());
            }
        }
        ImGui::SameLine(right_start);
        v.label(mode_status);
        ImGui::SameLine(0,18.f);
        v.label(theme_status);
    }
    v.dialogs(m,cb);
    v.feature_dialogs(m,cb);
    ImGui::End();pop_theme();ImGui::PopStyleVar(14);
    if(fonts.regular)ImGui::PopFont();
}
} // namespace jianssh::ui
