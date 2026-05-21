from __future__ import annotations

import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
APP_DIR = ROOT / "companion-app"
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

from core.animal_pets_data import FRAMES, FRAME_W, FRAME_H

SIZE = 412
OUT_DIR = ROOT / "companion-app" / "assets" / "screen-concepts"


def u32_to_rgba(v: int) -> tuple[int, int, int, int]:
    a = (v >> 24) & 0xFF
    b = (v >> 16) & 0xFF
    g = (v >> 8) & 0xFF
    r = v & 0xFF
    return r, g, b, a


def frame_image(idx: int, scale: int = 7) -> Image.Image:
    src = Image.new("RGBA", (FRAME_W, FRAME_H))
    pix = src.load()
    for i, v in enumerate(FRAMES[idx]):
        x = i % FRAME_W
        y = i // FRAME_W
        pix[x, y] = u32_to_rgba(v)
    return src.resize((FRAME_W * scale, FRAME_H * scale), Image.Resampling.NEAREST)


def base_canvas() -> Image.Image:
    im = Image.new("RGBA", (SIZE, SIZE), (8, 10, 18, 255))
    d = ImageDraw.Draw(im)
    d.ellipse((8, 8, SIZE - 8, SIZE - 8), outline=(100, 255, 190, 180), width=7)
    d.ellipse((16, 16, SIZE - 16, SIZE - 16), outline=(80, 200, 180, 80), width=2)
    return im


def circular_crop(im: Image.Image) -> Image.Image:
    mask = Image.new("L", (SIZE, SIZE), 0)
    md = ImageDraw.Draw(mask)
    md.ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.paste(im, (0, 0), mask)
    return out


def add_bars(d: ImageDraw.ImageDraw, values: list[float]) -> None:
    colors = [(114, 255, 176), (255, 165, 87), (219, 93, 255), (110, 165, 255)]
    y = 84
    for i, v in enumerate(values):
        d.rounded_rectangle((214, y, 360, y + 34), radius=16, fill=(55, 62, 85, 255))
        d.rounded_rectangle((214, y, int(214 + 146 * v), y + 34), radius=16, fill=colors[i], outline=None)
        y += 50


def draw_screen(filename: str, frame_idx: int, bars: list[float], title: str, subtitle: str) -> None:
    im = base_canvas()
    d = ImageDraw.Draw(im)
    duck = frame_image(frame_idx, scale=7)
    im.alpha_composite(duck, dest=(54, 112))
    add_bars(d, bars)
    font_big = ImageFont.load_default()
    d.text((56, 322), title, fill=(139, 248, 147, 255), font=font_big)
    d.text((56, 346), subtitle, fill=(239, 242, 255, 255), font=font_big)
    circular_crop(im).save(OUT_DIR / filename)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    draw_screen("home.png", 0, [0.75, 0.55, 0.62, 0.50], "DAY 5: Feeling great!", "3 / 4 habits complete")
    draw_screen("habit_prompt.png", 0, [0.40, 0.30, 0.66, 0.25], "Hydrate now", "Tap to log progress")
    draw_screen("sleep_mode.png", 1, [0.10, 0.15, 0.22, 0.08], "Sleep mode", "Resting for tomorrow")
    draw_screen("celebration.png", 0, [1.00, 1.00, 1.00, 1.00], "All habits done!", "Great work today")
    print(f"Wrote mockups to {OUT_DIR}")


if __name__ == "__main__":
    main()
