#include "jianssh/app/workspace_controller.hpp"
#include "jianssh/ui/motion.hpp"
#include "jianssh/ui/system_theme.hpp"
#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "ui_font.hpp"
#include "mono_font.hpp"
#include "ui_sans.hpp"
#include "ui_glyphs.hpp"
#include "app_icon.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
std::filesystem::path config_path(){
 if(auto* p=std::getenv("XDG_CONFIG_HOME");p&&*p)return std::filesystem::path(p)/"JianSSH/jianssh.db";
#ifdef _WIN32
 if(auto* a=std::getenv("APPDATA");a&&*a)return std::filesystem::path(a)/"JianSSH/jianssh.db";
 if(auto* u=std::getenv("USERPROFILE");u&&*u)return std::filesystem::path(u)/"AppData/Roaming/JianSSH/jianssh.db";
 throw std::runtime_error("需要 APPDATA 或 USERPROFILE 环境变量");
#else
 auto* h=std::getenv("HOME");if(!h||!*h)throw std::runtime_error("需要 HOME 或 XDG_CONFIG_HOME 环境变量");
 return std::filesystem::path(h)/".config/JianSSH/jianssh.db";
#endif
}
bool screenshot(const std::string& path,int w,int h){
 using Read=void(*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
 auto read=reinterpret_cast<Read>(SDL_GL_GetProcAddress("glReadPixels"));
 if(!read)return false;
 std::vector<unsigned char> pixels(static_cast<std::size_t>(w)*h*4);
 read(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
 auto* surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
 if(!surface)return false;
 for(int y=0;y<h;++y)std::copy_n(pixels.data()+static_cast<std::size_t>(h-y-1)*w*4,w*4,static_cast<unsigned char*>(surface->pixels)+y*surface->pitch);
 bool ok=SDL_SaveBMP(surface,path.c_str())==0;SDL_FreeSurface(surface);return ok;
}
}
int run_application(int argc,char**argv){
    int smoke_ms=0;bool reduced=false;std::string capture,metrics;std::filesystem::path database;
    int win_w=1280,win_h=800;
    jianssh::app::ThemeMode theme=jianssh::app::ThemeMode::System;
    for(int i=1;i<argc;++i){std::string a=argv[i];
    if(a=="--version"){std::puts(
#ifdef _WIN32
     "JianSSH 1.0.0（Windows 桌面版）"
#else
     "JianSSH 1.0.0（Linux 桌面版）"
#endif
    );return 0;}
    if(a=="--help"){std::puts("JianSSH [--smoke-ms 毫秒] [--screenshot 文件.bmp] [--metrics 文件] [--data-dir 目录] [--reduced-motion] [--theme system|dark|light] [--window 宽x高]");return 0;}
    if(a=="--reduced-motion")reduced=true;
    else if(a=="--window"&&i+1<argc){std::string v=argv[++i];int w=0,h=0;
     if(std::sscanf(v.c_str(),"%dx%d",&w,&h)!=2||w<300||h<200||w>8192||h>8192){std::fprintf(stderr,"窗口尺寸必须为 宽x高，且范围在 300x200..8192x8192 之间\n");return 1;}
     win_w=w;win_h=h;}
    else if(a=="--theme"&&i+1<argc){std::string v=argv[++i];
     if(v=="dark")theme=jianssh::app::ThemeMode::Dark;
     else if(v=="light")theme=jianssh::app::ThemeMode::Light;
     else if(v=="system")theme=jianssh::app::ThemeMode::System;
     else {std::fprintf(stderr,"主题必须是 system、dark 或 light\n");return 1;}}
    else if((a=="--smoke-ms"||a=="--screenshot"||a=="--metrics"||a=="--data-dir")&&i+1<argc){std::string v=argv[++i];if(a=="--smoke-ms")smoke_ms=std::stoi(v);else if(a=="--screenshot")capture=v;else if(a=="--metrics")metrics=v;else database=std::filesystem::path(v)/"jianssh.db";}
    }
 if(database.empty())database=config_path();
 if(smoke_ms<0||smoke_ms>3600000)throw std::invalid_argument("冒烟运行时长必须在 0..3600000 毫秒之间");
 const auto boot=Clock::now();
 const bool timing=std::getenv("JIANSSH_TIMING")!=nullptr;
 auto mark=[&](const char* phase){if(timing)std::fprintf(stderr,"启动耗时 %-12s %.1f ms\n",phase,std::chrono::duration<double,std::milli>(Clock::now()-boot).count());};
 // Build the CJK font atlas in parallel with window/GL creation.
 IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
 ImFontConfig cfg;cfg.FontDataOwnedByAtlas=false;
 jianssh::ui::WorkspaceFonts fonts;
 // CJK-only fallback ranges: Basic Latin must stay with the proportional
 // DejaVu Sans base, otherwise the monospaced fallback would flatten it.
 static const ImWchar cjk_ranges[]={0x2000,0x206F,0x3000,0x30FF,0x31F0,0x31FF,0xFF00,0xFFEF,0xFFFD,0xFFFD,0x4E00,0x9FAF,0};
 ImFontGlyphRangesBuilder body_builder;body_builder.AddRanges(cjk_ranges);body_builder.AddRanges(ui_extra_glyphs);ImVector<ImWchar> body_ranges;body_builder.BuildRanges(&body_ranges);
 ImFontGlyphRangesBuilder heading_builder;heading_builder.AddRanges(ui_extra_glyphs);ImVector<ImWchar> heading_ranges;heading_builder.BuildRanges(&heading_ranges);
 auto add_font=[&](const unsigned char* data,std::size_t bytes,float size,const ImWchar* ranges){
  return io.Fonts->AddFontFromMemoryTTF((void*)data,static_cast<int>(bytes),size,&cfg,ranges);
 };
 auto make_font=[&](const unsigned char* base,std::size_t base_bytes,float size,const ImWchar* fallback_ranges){
  auto* font=add_font(base,base_bytes,size,io.Fonts->GetGlyphRangesDefault());
  ImFontConfig merge=cfg;merge.MergeMode=true;
  io.Fonts->AddFontFromMemoryTTF((void*)ui_font,sizeof(ui_font),size,&merge,fallback_ranges);
  return font;
 };
 // macOS-like UI typography: proportional sans for labels, true monospace for terminals.
 fonts.regular=make_font(ui_sans,sizeof(ui_sans),15.5f,body_ranges.Data);
 fonts.heading=make_font(ui_sans,sizeof(ui_sans),24.0f,heading_ranges.Data);
 fonts.mono=make_font(mono_font,sizeof(mono_font),15.5f,body_ranges.Data);
 mark("字体注册");
 std::thread font_builder([&io]{unsigned char* pixels=nullptr;int w=0,h=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);});
 auto join_fonts=[&]{if(font_builder.joinable())font_builder.join();};
 // SDL consults this when the main window is created, before ImGui's backend
 // gets a chance to set it. Native IME UI supplies composition/candidate text.
 SDL_SetHint(SDL_HINT_IME_SHOW_UI,"1");
#ifdef _WIN32
 // Avoid Windows bitmap-scaling the entire OpenGL window on high-DPI displays.
 SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS,"system");
#endif
 if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0){join_fonts();std::fputs("SDL 初始化失败\n",stderr);return 1;}
 mark("SDL");
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
 SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,0);SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,0);
 SDL_Window* window=SDL_CreateWindow("JianSSH · 从容连接",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,win_w,win_h,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
 if(!window){join_fonts();std::fputs("窗口创建失败\n",stderr);SDL_Quit();return 1;}
 SDL_SetWindowMinimumSize(window,300,200);auto gl=SDL_GL_CreateContext(window);
 if(!gl){join_fonts();std::fputs("无法创建 OpenGL 3.3 上下文\n",stderr);SDL_DestroyWindow(window);SDL_Quit();return 1;}
 if(auto* icon=SDL_CreateRGBSurfaceWithFormatFrom((void*)app_icon_rgba,app_icon_width,app_icon_height,32,app_icon_width*4,SDL_PIXELFORMAT_RGBA32)){SDL_SetWindowIcon(window,icon);SDL_FreeSurface(icon);}
 mark("窗口与GL");
 SDL_GL_MakeCurrent(window,gl);SDL_GL_SetSwapInterval(0);
 join_fonts();
 mark("字体图集");
 if(!ImGui_ImplSDL2_InitForOpenGL(window,gl)||!ImGui_ImplOpenGL3_Init("#version 330")){std::fprintf(stderr,"ImGui 后端初始化失败\n");return 1;}
 mark("ImGui后端");
 using Viewport=void(*)(GLint,GLint,GLsizei,GLsizei);using ClearColor=void(*)(GLfloat,GLfloat,GLfloat,GLfloat);using Clear=void(*)(GLbitfield);
 auto viewport=reinterpret_cast<Viewport>(SDL_GL_GetProcAddress("glViewport"));auto clearcolor=reinterpret_cast<ClearColor>(SDL_GL_GetProcAddress("glClearColor"));auto clear=reinterpret_cast<Clear>(SDL_GL_GetProcAddress("glClear"));
 bool running=true,dirty=true;unsigned frames=0,waits=0;auto start=Clock::now();int result=0;bool font_pixels_released=false;
 try{
  jianssh::app::WorkspaceController controller(database);auto cb=controller.callbacks();auto setting=controller.model().settings;setting.reduced_motion=reduced;setting.theme=theme;cb.setting(setting);
  mark("控制器");
   jianssh::ui::Workspace workspace;
  // Follow the display refresh rate so animations are paced at the panel's
  // native cadence instead of a fixed 60 Hz timer; clamp runaway drivers.
  int refresh_hz=60;
  if(SDL_DisplayMode mode{};SDL_GetCurrentDisplayMode(0,&mode)==0&&mode.refresh_rate>=50)refresh_hz=std::clamp(mode.refresh_rate,50,144);
  const auto refresh_interval=std::chrono::nanoseconds(1'000'000'000LL/refresh_hz);
  jianssh::ui::FrameScheduler scheduler(std::chrono::milliseconds{250},refresh_interval);
  // Input events keep the loop at frame rate for a short while. A keyboard
  // focus move needs a couple of frames to settle; drawing those frames back
  // to back would let Dear ImGui drop the first character typed after a Tab.
  auto followup_until=Clock::now()-std::chrono::milliseconds(1);
  bool force_now=false;
  // The desktop preference is probed off the render thread; when it changes the
  // window asks for a repaint even while it is otherwise idle.
  std::uint64_t theme_revision=jianssh::ui::system_theme_revision();
  while(running){
   const auto now=Clock::now();if(smoke_ms>0&&now-start>=std::chrono::milliseconds(smoke_ms))break;
   dirty=controller.tick()||dirty;
   const auto revision=jianssh::ui::system_theme_revision();
   if(revision!=theme_revision){theme_revision=revision;dirty=true;}
   const auto flags=SDL_GetWindowFlags(window);bool visible=!(flags&(SDL_WINDOW_MINIMIZED|SDL_WINDOW_HIDDEN));
   // Hidden windows still drain bounded network queues at <=4 Hz; never render.
   bool active=workspace.animation_active()||!controller.model().settings.power_saving||now<followup_until;
   // The frame that carries a keyboard event is drawn at once; the following
   // ones keep the regular cadence so a focus move can settle before the next
   // character arrives. Dear ImGui discards characters that reach a text field
   // in the very frame it becomes active.
   auto plan=scheduler.plan(now,visible,active&&!force_now,dirty);
   force_now=false;
   if(plan.render){
    if(frames==0)mark("首帧前");
     // ImGui handles navigation in NewFrame, before the terminal is drawn.
     // Let the terminal own Esc/arrows while it has focus; retain navigation
     // for forms and dialogs once terminal input is inactive.
     if(workspace.terminal_input_active())io.ConfigFlags&=~ImGuiConfigFlags_NavEnableKeyboard;
     else io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
     ImGui_ImplOpenGL3_NewFrame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();workspace.draw(controller.model(),controller.callbacks(),fonts);ImGui::Render();
    // Clear with the active theme so no colour seam can appear at the edge.
    const auto background=workspace.background_color();
    int w,h;SDL_GL_GetDrawableSize(window,&w,&h);viewport(0,0,w,h);clearcolor(background.r,background.g,background.b,background.a);clear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());++frames;
    if(!font_pixels_released){io.Fonts->ClearTexData();font_pixels_released=true;}
    if(!capture.empty()&&smoke_ms>0&&now-start>=std::chrono::milliseconds(smoke_ms-100)){if(!screenshot(capture,w,h))result=1;capture.clear();}
    SDL_GL_SwapWindow(window);scheduler.did_render(now);dirty=false;
    if(frames==1)mark("首帧完成");
   }
   int timeout=250;if(plan.wait)timeout=std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(*plan.wait).count()),1,250);
    // Drawing can start an animation (including the very first entrance frame).
    // Re-evaluate afterwards instead of sleeping on the old idle state.
    if(plan.render){
     const bool animate=workspace.animation_active()||!controller.model().settings.power_saving||Clock::now()<followup_until;
     if(animate){
      const auto remaining=scheduler.refresh_interval()-(Clock::now()-now);
      timeout=std::clamp(static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()),1,17);
     }else timeout=controller.busy()?16:50;
    }
   if(controller.has_pending_output())timeout=1;
   if(smoke_ms>0)timeout=std::min(timeout,50);
   SDL_Event e;++waits;
   if(SDL_WaitEventTimeout(&e,timeout)){
    do{ImGui_ImplSDL2_ProcessEvent(&e);if(e.type==SDL_QUIT||(e.type==SDL_WINDOWEVENT&&e.window.event==SDL_WINDOWEVENT_CLOSE))running=false;dirty=true;force_now=true;followup_until=Clock::now()+std::chrono::milliseconds(150);}while(SDL_PollEvent(&e));
   }
   if(smoke_ms>0&&!capture.empty()&&now-start>std::chrono::milliseconds(smoke_ms-200))dirty=true;
  }
  controller.begin_shutdown();
  const auto shutdown_deadline=Clock::now()+std::chrono::seconds(10);
  while(!controller.shutdown_complete()&&Clock::now()<shutdown_deadline){controller.tick();SDL_Delay(5);}
  if(!controller.shutdown_complete()){std::fprintf(stderr,"存储在 10 秒内未完成关闭，请检查数据库状态\n");result=1;}
  const auto elapsed=std::chrono::duration<double>(Clock::now()-start).count();
  std::fprintf(stdout,"JianSSH 桌面驱动=%s 帧数=%u 等待=%u 用时=%.3f 秒\n",SDL_GetCurrentVideoDriver(),frames,waits,elapsed);
  if(!metrics.empty()){std::ofstream out(metrics);out<<"{\"driver\":\""<<SDL_GetCurrentVideoDriver()<<"\",\"frames\":"<<frames<<",\"waits\":"<<waits<<",\"elapsed_seconds\":"<<elapsed<<",\"reduced_motion\":"<<(controller.model().settings.reduced_motion?"true":"false")<<",\"theme\":\""<<(theme==jianssh::app::ThemeMode::Dark?"dark":theme==jianssh::app::ThemeMode::Light?"light":"system")<<"\"}\n";}
 }catch(const std::exception& e){std::fprintf(stderr,"JianSSH 错误：%s\n",e.what());result=1;}
 ImGui_ImplOpenGL3_Shutdown();ImGui_ImplSDL2_Shutdown();ImGui::DestroyContext();SDL_GL_DeleteContext(gl);SDL_DestroyWindow(window);SDL_Quit();return result;
}

int main(int argc,char**argv){try{return run_application(argc,argv);}catch(const std::exception& e){
 std::fprintf(stderr,"JianSSH 错误：%s\n",e.what());
#if defined(_WIN32) && !defined(SDL_MAIN_HANDLED)
 SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"JianSSH",e.what(),nullptr);
#endif
 return 1;}}
