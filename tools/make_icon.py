#!/usr/bin/env python3
"""Generate the JianSSH application icon.

Outputs are committed so the build only embeds them:
  assets/icon.ico   Windows PE resource (16..256 px)
  assets/icon.png   256 px preview
  assets/icon.rgba  raw RGBA pixels used for SDL_SetWindowIcon

Requires Pillow when the icon needs to be regenerated; normal builds do not
run this script.
"""
import pathlib
import struct
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SIZE = 256
SS = 4  # supersampling factor for smooth edges

BG = (18, 30, 40, 255)
BORDER = (63, 182, 168, 255)
INK = (236, 245, 245, 255)
TEAL = (86, 210, 195, 255)


def build() -> Image.Image:
    s = SIZE * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    margin = 8 * SS
    radius = 46 * SS
    d.rounded_rectangle((margin, margin, s - margin, s - margin), radius=radius,
                        fill=BG, outline=BORDER, width=7 * SS)
    # Terminal prompt ">" and underscore, drawn as thick strokes so they stay
    # legible down to 16 px.
    d.line([(66 * SS, 78 * SS), (126 * SS, 128 * SS), (66 * SS, 178 * SS)],
           fill=INK, width=17 * SS, joint="curve")
    d.rounded_rectangle((146 * SS, 158 * SS, 198 * SS, 176 * SS), radius=5 * SS, fill=TEAL)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main() -> None:
    ASSETS.mkdir(exist_ok=True)
    icon = build()
    icon.save(ASSETS / "icon.png")
    icon.save(ASSETS / "icon.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    raw = icon.tobytes()  # RGBA, row-major
    assert len(raw) == SIZE * SIZE * 4, len(raw)
    (ASSETS / "icon.rgba").write_bytes(raw)
    print(f"icon: {SIZE}x{SIZE}, {len(raw)} bytes RGBA")


if __name__ == "__main__":
    main()
