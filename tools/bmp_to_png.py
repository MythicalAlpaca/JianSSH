#!/usr/bin/env python3
"""Convert this application's uncompressed SDL BMP screenshots using stdlib."""
import pathlib, struct, sys, zlib
src, dst = map(pathlib.Path, sys.argv[1:3])
d = src.read_bytes()
assert d[:2] == b'BM'
o = struct.unpack_from('<I', d, 10)[0]
w, h = struct.unpack_from('<ii', d, 18)
b = struct.unpack_from('<H', d, 28)[0]
assert b in (24, 32) and w > 0 and h != 0
stride = ((w*b+31)//32)*4
rows = []
for y in range(abs(h)):
    row = d[o+(abs(h)-1-y if h>0 else y)*stride:][:stride]
    rgb = bytearray()
    for x in range(w):
        blue, green, red = row[x*(b//8):][:3]
        rgb.extend((red, green, blue))
    rows.append(b'\0'+rgb)
def chunk(k, v):
    return struct.pack('>I', len(v))+k+v+struct.pack('>I', zlib.crc32(k+v)&0xffffffff)
dst.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB',w,abs(h),8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b''))
