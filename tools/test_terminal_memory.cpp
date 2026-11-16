#include "jianssh/app/terminal.hpp"
#include <cassert>
#include <iostream>

using jianssh::app::Terminal;
int main() {
    Terminal terminal(2, 80, 3);
    terminal.feed("first\r\nsecond\r\nthird\r\nfourth\r\nfifth");
    auto history = terminal.scrollback();
    assert(history.size() == 3);
    assert(history.front().size() == 80);
    assert(history.front()[0].text == "f");
    assert(history.front()[5].text.empty());
    terminal.resize(5, 80);
    assert(terminal.scrollback_size() == 0);
    assert(terminal.text() == "first\nsecond\nthird\nfourth\nfifth");

    // A coloured blank tail must survive compression and resize restoration.
    Terminal coloured(2, 80, 10);
    coloured.feed("\x1b[41m\x1b[2Jhello\r\nworld\r\n");
    auto before = coloured.scrollback();
    assert(before.size() == 1);
    assert(before[0][79].bg.kind == jianssh::app::ColorKind::Indexed);
    assert(before[0][79].bg.index == 1);
    coloured.resize(3, 80);
    assert(coloured.cell(0, 79).bg.index == 1);
    assert(coloured.cell(0, 0).text == "h");

    Terminal wide(2, 80, 10);
    wide.feed("中文e\xcc\x81\r\nnext\r\n");
    auto glyphs = wide.scrollback();
    assert(glyphs[0][0].text == "中");
    assert(glyphs[0][1].width == 0);
    assert(glyphs[0][4].text == "e\xcc\x81");
    wide.resize(3, 100);
    assert(wide.cell(0, 0).text == "中");
    assert(wide.cell(0, 99).text.empty());
    wide.set_scrollback_limit(0);
    wide.feed("\r\na\r\nb\r\nc\r\n");
    assert(wide.scrollback_size() == 0);

    // Run with /usr/bin/time -v to compare populated multi-session history.
    std::vector<Terminal> sessions;
    for (int i = 0; i < 10; ++i) {
        sessions.emplace_back(24, 400, 2000);
        for (int j = 0; j < 2100; ++j) sessions.back().feed("short shell output\r\n");
        assert(sessions.back().scrollback_size() == 2000);
    }
    std::cout << "terminal history regression and memory workload passed\n";
}
