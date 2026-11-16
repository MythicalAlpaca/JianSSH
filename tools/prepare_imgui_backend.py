#!/usr/bin/env python3
"""Adapt the pinned desktop GL backend to a single-channel font texture."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
old_read = 'io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);'
old_upload = 'GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));'
new_upload = '''// The backend's minimal GL loader omits these core 3.3 constants.
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
    // Desktop OpenGL 3.3: sample coverage as alpha, with white RGB.
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED));
    GLint last_unpack_alignment;
    GL_CALL(glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_unpack_alignment));
    GL_CALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels));
    GL_CALL(glPixelStorei(GL_UNPACK_ALIGNMENT, last_unpack_alignment));'''
for old in (old_read, old_upload):
    if source.count(old) != 1:
        raise SystemExit('Unsupported ImGui backend: review the font texture adaptation')
source = source.replace(old_read, 'io.Fonts->GetTexDataAsAlpha8(&pixels, &width, &height);')
source = source.replace(old_upload, new_upload)
target = pathlib.Path(sys.argv[2])
target.parent.mkdir(parents=True, exist_ok=True)
if not target.exists() or target.read_text() != source:
    target.write_text(source)
