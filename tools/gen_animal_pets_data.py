"""
Regenerate companion-app/core/animal_pets_data.py from repo-root Duck.c

Run from repo root:
  python tools/gen_animal_pets_data.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
C_PATH = REPO / "Duck.c"
OUT_PATH = REPO / "companion-app" / "core" / "animal_pets_data.py"
APP_FRAME_LIMIT = 9


def parse_c(src: str) -> tuple[int, int, int, list[tuple[int, ...]]]:
    m_fc = re.search(r"#define\s+\w+_FRAME_COUNT\s+(\d+)", src)
    m_fw = re.search(r"#define\s+\w+_FRAME_WIDTH\s+(\d+)", src)
    m_fh = re.search(r"#define\s+\w+_FRAME_HEIGHT\s+(\d+)", src)
    if not (m_fc and m_fw and m_fh):
        raise ValueError("Missing ANIMAL_PETS_FRAME_COUNT / WIDTH / HEIGHT defines")
    fc, fw, fh = int(m_fc.group(1)), int(m_fw.group(1)), int(m_fh.group(1))
    ppx = fw * fh

    m_arr = re.search(r"static\s+const\s+uint32_t\s+(\w+)\s*\[\d+\]\[\d+\]\s*=\s*\{", src)
    if not m_arr:
        raise ValueError("uint32 frame array initializer not found")
    i = src.find("{", m_arr.end() - 1)

    depth = 0
    end = -1
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                end = j
                break
    if end < 0:
        raise ValueError("unbalanced braces in array")

    blob = src[i + 1 : end]
    hexes = re.findall(r"0x[0-9a-fA-F]+", blob, flags=re.I)
    vals = [int(h, 16) & 0xFFFFFFFF for h in hexes]
    expect = fc * ppx
    if len(vals) != expect:
        raise ValueError(f"Expected {expect} uint32 values, found {len(vals)}")

    frames = [tuple(vals[k : k + ppx]) for k in range(0, len(vals), ppx)]
    return fc, fw, fh, frames


def emit(fc: int, fw: int, fh: int, frames: list[tuple[int, ...]]) -> str:
    lines: list[str] = [
        "# Auto-generated from Duck.c — run: python tools/gen_animal_pets_data.py",
        f"# {fc} frames x {fw}x{fh} (row-major), uint32 per pixel (Piskel ABGR).",
        f"FRAME_W = {fw}",
        f"FRAME_H = {fh}",
        f"FRAME_COUNT = {fc}",
        "",
        "FRAMES = (",
    ]
    for fi, frame in enumerate(frames):
        lines.append("    (")
        for row_start in range(0, len(frame), fw):
            row = frame[row_start : row_start + fw]
            row_s = ", ".join(hex(p) for p in row)
            lines.append(f"        {row_s},")
        lines.append("    ),")
    lines.append(")")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    if not C_PATH.is_file():
        print(f"Missing C source: {C_PATH}", file=sys.stderr)
        return 1
    text = C_PATH.read_text(encoding="utf-8")
    fc, fw, fh, frames = parse_c(text)
    if fc < APP_FRAME_LIMIT:
        print(
            f"Animal Pets.c has {fc} frames, but app expects at least {APP_FRAME_LIMIT}.",
            file=sys.stderr,
        )
        return 1
    frames = frames[:APP_FRAME_LIMIT]
    fc = APP_FRAME_LIMIT
    out = emit(fc, fw, fh, frames)
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(out, encoding="utf-8", newline="\n")
    print(f"Wrote {OUT_PATH} ({fc} frames, {fw}x{fh})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
