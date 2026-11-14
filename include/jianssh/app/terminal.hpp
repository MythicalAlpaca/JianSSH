#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jianssh::app {
enum class ColorKind { Default, Indexed, TrueColor };
struct TerminalColor {
    ColorKind kind = ColorKind::Default;
    std::uint8_t index = 0, red = 0, green = 0, blue = 0;
};
struct CellStyle {
    bool bold = false, italic = false, blink = false, reverse = false;
    bool conceal = false, strike = false;
    unsigned underline = 0, font = 0;
};
struct TerminalCell {
    // UTF-8 base character and combining characters; empty for blank/continuation cells.
    std::string text;
    int width = 1; // 0 for the trailing cell of a wide glyph
    CellStyle style;
    TerminalColor fg, bg;
};
using TerminalLine = std::vector<TerminalCell>;
struct TerminalCursor {
    int row = 0, col = 0;
    bool visible = true, blink = false;
    int shape = 1; // 1 block, 2 underline, 3 bar
};
struct TerminalSnapshot {
    int rows = 0, cols = 0;
    TerminalCursor cursor;
    std::vector<TerminalLine> lines;
};
enum class Key { Enter, Tab, Backspace, Escape, Up, Down, Left, Right,
                 Insert, Delete, Home, End, PageUp, PageDown };
enum class Modifier : unsigned { None = 0, Shift = 1, Alt = 2, Ctrl = 4 };
constexpr Modifier operator|(Modifier a, Modifier b) noexcept {
    return static_cast<Modifier>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

// Single-thread confined RAII terminal core. No rendering or network ownership.
class Terminal {
public:
    using WriteCallback = std::function<void(std::string_view)>;
    static constexpr std::size_t feed_chunk_size = 4096;
    explicit Terminal(int rows, int cols, std::size_t scrollback_limit = 10000);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) noexcept;
    Terminal& operator=(Terminal&&) noexcept;

    // Consumes all bytes in bounded parser calls; split UTF-8/escape sequences are retained.
    std::size_t feed(std::string_view bytes);
    void resize(int rows, int cols);
    [[nodiscard]] int rows() const;
    [[nodiscard]] int cols() const;
    [[nodiscard]] TerminalCell cell(int row, int col) const;
    [[nodiscard]] TerminalCursor cursor() const;
    [[nodiscard]] TerminalSnapshot snapshot() const;
    // Screen only, trailing blank cells trimmed, rows separated by '\n'.
    [[nodiscard]] std::string text() const;
    // Owned copies, oldest first; historical lines retain their original column count.
    [[nodiscard]] std::vector<TerminalLine> scrollback() const;
    [[nodiscard]] std::size_t scrollback_size() const;
    [[nodiscard]] std::size_t scrollback_limit() const;
    void set_scrollback_limit(std::size_t limit);
    void clear_scrollback();

    // Callback views are valid only during the call. Do not re-enter this Terminal.
    // Terminal protocol replies also use this callback. With no callback output is dropped.
    void set_write_callback(WriteCallback callback);
    void write_utf8(std::string_view text); // paste with mode-dependent wrappers, one callback
    void unichar(char32_t codepoint, Modifier modifiers = Modifier::None);
    void key(Key key, Modifier modifiers = Modifier::None);
    void function_key(unsigned number, Modifier modifiers = Modifier::None); // F1..F255
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Impl& impl();
    const Impl& impl() const;
};
} // namespace jianssh::app
