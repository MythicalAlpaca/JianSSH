#!/usr/bin/env python3
"""Generate embedded assets; never reads fonts at application runtime."""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'build/generated'
out.mkdir(parents=True, exist_ok=True)
assets = [('ui_font', root/'assets/fonts/DroidSansFallbackFull.ttf'), ('mono_font', root/'assets/fonts/DejaVuSansMono.ttf'), ('ui_sans', root/'assets/fonts/DejaVuSans.ttf')]
for name, path in assets:
    data = path.read_bytes()
    text = '#pragma once\ninline const unsigned char '+name+'[] = {\n'
    text += '\n'.join(','.join(str(b) for b in data[i:i+24])+',' for i in range(0,len(data),24))
    text += '\n};\n'
    target = out / (name+'.hpp')
    if not target.exists() or target.read_text() != text:
        target.write_text(text)
# Include every literal UI glyph, not only the font library's common-CJK subset.
# Headers are scanned as well: presentation layers keep their labels in
# include/jianssh/ui, and a literal that is not covered here becomes tofu.
sources = list((root/'src').rglob('*.cpp')) + list((root/'include').rglob('*.hpp'))
chars = sorted({ord(c) for p in sources for c in p.read_text() if 0x80 <= ord(c) <= 0xffff})
ranges = ','.join(f'{c},{c}' for c in chars)+',0'
glyphs = '#pragma once\ninline const unsigned short ui_extra_glyphs[] = {'+ranges+'};\n'
target = out/'ui_glyphs.hpp'
if not target.exists() or target.read_text() != glyphs: target.write_text(glyphs)
icon = (root/'assets/icon.rgba').read_bytes()
icon_text = ('#pragma once\ninline constexpr int app_icon_width = 256;\n'
             'inline constexpr int app_icon_height = 256;\n'
             'inline const unsigned char app_icon_rgba[] = {\n')
icon_text += '\n'.join(','.join(str(b) for b in icon[i:i+24])+',' for i in range(0,len(icon),24))
icon_text += '\n};\n'
target = out/'app_icon.hpp'
if not target.exists() or target.read_text() != icon_text: target.write_text(icon_text)
print('Embedded fonts, literal glyph ranges and application icon generated')
