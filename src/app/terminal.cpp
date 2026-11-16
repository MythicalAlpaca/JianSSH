#include "jianssh/app/terminal.hpp"

#include <vterm.h>
#include <algorithm>
#include <deque>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace jianssh::app {
namespace {
void validate_size(int rows, int cols) {
    // Prevent integer overflow and unbounded accidental allocations in the C library.
    if (rows <= 0 || cols <= 0 || rows > 4096 || cols > 4096 ||
        static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols) > 4'000'000)
        throw std::invalid_argument("终端尺寸超出范围");
}
void append_utf8(std::string& out, std::uint32_t c) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) {
        out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 63));
        out += static_cast<char>(0x80 | (c & 63));
    } else {
        out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63));
        out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63));
    }
}
TerminalColor convert_color(const VTermColor& c) {
    TerminalColor result;
    if (c.type & VTERM_COLOR_DEFAULT_MASK) return result;
    if (VTERM_COLOR_IS_INDEXED(&c)) { result.kind = ColorKind::Indexed; result.index = c.indexed.idx; }
    else { result.kind = ColorKind::TrueColor; result.red = c.rgb.red; result.green = c.rgb.green; result.blue = c.rgb.blue; }
    return result;
}
TerminalCell convert_cell(const VTermScreenCell& c) {
    TerminalCell result;
    result.width = c.chars[0] == UINT32_MAX ? 0 : c.width;
    for (auto ch : c.chars) {
        if (!ch || ch == UINT32_MAX) break;
        append_utf8(result.text, ch);
    }
    result.style = {bool(c.attrs.bold), bool(c.attrs.italic), bool(c.attrs.blink),
                    bool(c.attrs.reverse), bool(c.attrs.conceal), bool(c.attrs.strike),
                    c.attrs.underline, c.attrs.font};
    result.fg = convert_color(c.fg); result.bg = convert_color(c.bg);
    return result;
}
VTermModifier mods(Modifier m) {
    const auto bits = static_cast<unsigned>(m);
    if (bits & ~7u) throw std::invalid_argument("终端修饰键无效");
    return static_cast<VTermModifier>(bits);
}
}
struct Terminal::Impl {
    struct Deleter { void operator()(VTerm* p) const noexcept { vterm_free(p); } };
    std::unique_ptr<VTerm, Deleter> vt;
    VTermScreen* screen = nullptr;
    TerminalCursor cursor;
    bool mouse_tracking = false;
    struct HistoryLine {
        int cols;
        VTermScreenCell tail;
        std::vector<VTermScreenCell> cells;
        HistoryLine(int count, const VTermScreenCell* data) : cols(count), tail(data[count - 1]) {
            // Preserve the exact final cell (including colour/style/width), but
            // store repeated trailing cells only once. Typical shell output has
            // long blank tails. A byte comparison is conservative and lossless.
            while (count && std::memcmp(&data[count - 1], &tail, sizeof(tail)) == 0) --count;
            cells.assign(data, data + count);
        }
    };
    std::deque<HistoryLine> history;
    std::size_t limit;
    WriteCallback writer;
    std::exception_ptr callback_error;
    // A UTF-8 prefix at a parser-call boundary (at most three bytes).
    std::string pending_utf8;

    void check_error() { if (callback_error) std::rethrow_exception(std::exchange(callback_error, {})); }
    template<class F> int guarded(F&& fn) noexcept {
        try { return fn(); } catch (...) { if (!callback_error) callback_error = std::current_exception(); return 0; }
    }
    static void output(const char* data, std::size_t size, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        self.guarded([&] { if (self.writer) self.writer(std::string_view(data, size)); return 1; });
    }
    static int move_cursor(VTermPos p, VTermPos, int visible, void* user) noexcept {
        auto& c = static_cast<Impl*>(user)->cursor;
        c.row = p.row; c.col = p.col; c.visible = visible; return 1;
    }
    static int property(VTermProp prop, VTermValue* val, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        auto& c = self.cursor;
        if (prop == VTERM_PROP_MOUSE) self.mouse_tracking = val->number != VTERM_PROP_MOUSE_NONE;
        if (prop == VTERM_PROP_CURSORVISIBLE) c.visible = val->boolean;
        if (prop == VTERM_PROP_CURSORBLINK) c.blink = val->boolean;
        if (prop == VTERM_PROP_CURSORSHAPE) c.shape = val->number;
        return 1;
    }
    static int push(int cols, const VTermScreenCell* cells, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        return self.guarded([&] {
            if (self.limit) {
                // Evict first to never temporarily exceed the configured line limit.
                if (self.history.size() >= self.limit) self.history.pop_front();
                if (cols > 0) self.history.emplace_back(cols, cells);
            }
            return 1;
        });
    }
    static int pop(int cols, VTermScreenCell* cells, void* user) noexcept {
        auto& self = *static_cast<Impl*>(user);
        if (self.history.empty()) return 0;
        const auto& line = self.history.back();
        // Pad grown lines using default-colour blank cells, not zero RGB black.
        VTermScreenCell blank{}; blank.width = 1;
        blank.fg.type = VTERM_COLOR_DEFAULT_FG; blank.bg.type = VTERM_COLOR_DEFAULT_BG;
        std::fill(cells, cells + cols, blank);
        const auto restored = std::min(line.cols, cols);
        std::fill_n(cells, restored, line.tail);
        std::copy_n(line.cells.begin(), std::min(line.cells.size(), static_cast<std::size_t>(cols)), cells);
        self.history.pop_back(); return 1;
    }
    static int clear(void* user) noexcept { static_cast<Impl*>(user)->history.clear(); return 1; }
    Impl(int rows, int cols, std::size_t max_lines) : limit(max_lines) {
        validate_size(rows, cols);
        vt.reset(vterm_new(rows, cols));
        if (!vt) throw std::bad_alloc();
        vterm_set_utf8(vt.get(), 1);
        screen = vterm_obtain_screen(vt.get());
        if (!screen) throw std::bad_alloc();
        static const VTermScreenCallbacks callbacks = [] {
            VTermScreenCallbacks cb{};
            cb.movecursor = move_cursor; cb.settermprop = property;
            cb.sb_pushline = push; cb.sb_popline = pop; cb.sb_clear = clear;
            return cb;
        }();
        vterm_screen_set_callbacks(screen, &callbacks, this);
        vterm_output_set_callback(vt.get(), output, this);
        vterm_screen_enable_altscreen(screen, 1);
        vterm_screen_reset(screen, 1);
        check_error();
    }
};
Terminal::Terminal(int rows, int cols, std::size_t limit) : impl_(std::make_unique<Impl>(rows, cols, limit)) {}
Terminal::~Terminal() = default;
Terminal::Terminal(Terminal&&) noexcept = default;
Terminal& Terminal::operator=(Terminal&&) noexcept = default;
Terminal::Impl& Terminal::impl() { if (!impl_) throw std::logic_error("终端对象已被移动"); return *impl_; }
const Terminal::Impl& Terminal::impl() const { if (!impl_) throw std::logic_error("终端对象已被移动"); return *impl_; }
std::size_t Terminal::feed(std::string_view bytes) {
    auto& s = impl();
    const auto input_size = bytes.size();
    while (!bytes.empty()) {
        std::string chunk = std::move(s.pending_utf8);
        s.pending_utf8.clear();
        const auto count = std::min(feed_chunk_size - chunk.size(), bytes.size());
        chunk.append(bytes.data(), count);
        bytes.remove_prefix(count);

        // libvterm can lose an incomplete codepoint after earlier text in the
        // same write. Withhold only a potentially valid suffix; ASCII controls
        // and escape sequences are passed through unchanged and immediately.
        auto lead = chunk.size();
        while (lead && chunk.size() - lead < 3 &&
               (static_cast<unsigned char>(chunk[lead - 1]) & 0xc0) == 0x80) --lead;
        if (lead) {
            --lead;
            const auto first = static_cast<unsigned char>(chunk[lead]);
            const auto len = first >= 0xc2 && first <= 0xdf ? 2u :
                             first >= 0xe0 && first <= 0xef ? 3u :
                             first >= 0xf0 && first <= 0xf4 ? 4u : 0u;
            const auto have = chunk.size() - lead;
            bool prefix = len && have < len;
            if (prefix && have > 1) {
                const auto second = static_cast<unsigned char>(chunk[lead + 1]);
                prefix = (second & 0xc0) == 0x80 &&
                    !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
            }
            if (prefix) { s.pending_utf8.assign(chunk, lead, have); chunk.resize(lead); }
        }
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            const auto n = vterm_input_write(s.vt.get(), chunk.data() + offset, chunk.size() - offset);
            s.check_error();
            if (!n) throw std::runtime_error("终端解析未能继续");
            offset += n;
        }
    }
    vterm_screen_flush_damage(s.screen); s.check_error();
    return input_size;
}
void Terminal::resize(int rows, int cols) {
    validate_size(rows, cols);
    auto& s = impl(); vterm_set_size(s.vt.get(), rows, cols);
    vterm_screen_flush_damage(s.screen); s.check_error();
}
int Terminal::rows() const { int r; vterm_get_size(impl().vt.get(), &r, nullptr); return r; }
int Terminal::cols() const { int c; vterm_get_size(impl().vt.get(), nullptr, &c); return c; }
TerminalCell Terminal::cell(int row, int col) const {
    if (row < 0 || col < 0 || row >= rows() || col >= cols()) throw std::out_of_range("终端单元");
    VTermScreenCell c{};
    if (!vterm_screen_get_cell(impl().screen, {row, col}, &c)) throw std::runtime_error("无法读取终端单元");
    return convert_cell(c);
}
TerminalCursor Terminal::cursor() const {
    auto c = impl().cursor; VTermPos pos{};
    vterm_state_get_cursorpos(vterm_obtain_state(impl().vt.get()), &pos);
    c.row = pos.row; c.col = pos.col; return c;
}
TerminalSnapshot Terminal::snapshot() const {
    TerminalSnapshot result{rows(), cols(), cursor(), {}};
    result.lines.reserve(result.rows);
    for (int r = 0; r < result.rows; ++r) {
        TerminalLine line; line.reserve(result.cols);
        for (int c = 0; c < result.cols; ++c) line.push_back(cell(r, c));
        result.lines.push_back(std::move(line));
    }
    return result;
}
std::string Terminal::text() const {
    std::string result;
    for (const auto& line : snapshot().lines) {
        std::string row;
        for (const auto& c : line) {
            if (c.width == 0) continue;
            row += c.text.empty() ? " " : c.text;
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        result += row; result += '\n';
    }
    if (!result.empty()) result.pop_back();
    return result;
}
std::vector<TerminalLine> Terminal::scrollback() const {
    std::vector<TerminalLine> result;
    result.reserve(impl().history.size());
    for (const auto& raw : impl().history) {
        TerminalLine line; line.reserve(raw.cols);
        for (const auto& c : raw.cells) line.push_back(convert_cell(c));
        while (line.size() < static_cast<std::size_t>(raw.cols)) line.push_back(convert_cell(raw.tail));
        result.push_back(std::move(line));
    }
    return result;
}
std::size_t Terminal::scrollback_size() const { return impl().history.size(); }
std::size_t Terminal::scrollback_limit() const { return impl().limit; }
void Terminal::set_scrollback_limit(std::size_t limit) {
    auto& s = impl(); s.limit = limit;
    while (s.history.size() > limit) s.history.pop_front();
}
void Terminal::clear_scrollback() { impl().history.clear(); }
void Terminal::set_write_callback(WriteCallback cb) { impl().writer = std::move(cb); }
void Terminal::write_utf8(std::string_view text) {
    auto& s = impl();
    if (!s.writer || text.empty()) return;
    // Collect the mode-dependent wrappers and payload before one admission decision.
    std::string payload;
    auto writer = std::move(s.writer);
    s.writer = [&](std::string_view part) { payload.append(part); };
    try {
        vterm_keyboard_start_paste(s.vt.get()); s.check_error();
        payload.append(text);
        vterm_keyboard_end_paste(s.vt.get()); s.check_error();
    } catch (...) { s.writer = std::move(writer); throw; }
    s.writer = std::move(writer);
    s.writer(payload);
}
void Terminal::unichar(char32_t c, Modifier m) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) throw std::invalid_argument("Unicode 码点无效");
    auto& s = impl();
    const auto bits = static_cast<unsigned>(mods(m));
    if ((bits & 4u) && ((c >= U'a' && c <= U'z') || (c >= U'[' && c <= U'_') || c == U' ')) {
        char bytes[2] = {'\x1b', static_cast<char>(c == U' ' ? 0 : c & 0x1f)};
        if (s.writer) s.writer(std::string_view(bytes + ((bits & 2u) ? 0 : 1), (bits & 2u) ? 2 : 1));
        return;
    }
    vterm_keyboard_unichar(s.vt.get(), c, mods(m)); s.check_error();
}
void Terminal::key(Key k, Modifier m) {
    static constexpr VTermKey keys[] = {VTERM_KEY_ENTER, VTERM_KEY_TAB, VTERM_KEY_BACKSPACE,
        VTERM_KEY_ESCAPE, VTERM_KEY_UP, VTERM_KEY_DOWN, VTERM_KEY_LEFT, VTERM_KEY_RIGHT,
        VTERM_KEY_INS, VTERM_KEY_DEL, VTERM_KEY_HOME, VTERM_KEY_END, VTERM_KEY_PAGEUP, VTERM_KEY_PAGEDOWN};
    const auto index = static_cast<unsigned>(k);
    if (index >= sizeof(keys) / sizeof(keys[0])) throw std::invalid_argument("终端按键无效");
    auto& s = impl(); vterm_keyboard_key(s.vt.get(), keys[index], mods(m)); s.check_error();
}
void Terminal::function_key(unsigned n, Modifier m) {
    if (!n || n > 255) throw std::invalid_argument("功能键必须在 F1..F255 之间");
    auto& s = impl(); vterm_keyboard_key(s.vt.get(), static_cast<VTermKey>(VTERM_KEY_FUNCTION(n)), mods(m)); s.check_error();
}
bool Terminal::mouse_tracking() const { return impl().mouse_tracking; }
void Terminal::mouse_move(int row, int col, Modifier m) {
    if (row < 0 || col < 0 || row >= rows() || col >= cols()) throw std::out_of_range("终端鼠标坐标");
    auto& s = impl(); vterm_mouse_move(s.vt.get(), row, col, mods(m)); s.check_error();
}
void Terminal::mouse_button(int button, bool pressed, Modifier m) {
    if (button < 1 || button > 7) throw std::invalid_argument("终端鼠标按键无效");
    auto& s = impl(); vterm_mouse_button(s.vt.get(), button, pressed, mods(m)); s.check_error();
}
} // namespace jianssh::app
