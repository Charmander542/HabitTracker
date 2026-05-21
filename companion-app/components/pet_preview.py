"""
PetPreview — draws a 25×25 pixel pet from Animal Pets.c frame data (Pillow scale-up).
Gentle vertical float only (no per-frame redraw).
"""

from __future__ import annotations
import math
import tkinter as tk
from typing import Optional

from theme import COLORS

try:
    from PIL import Image, ImageTk
    _PIL = True
except ImportError:
    _PIL = False

from core.animal_pets_data import FRAMES, FRAME_COUNT, FRAME_W, FRAME_H


def _u32_to_rgba(v: int) -> tuple[int, int, int, int]:
    """Decode Piskel C export: 32-bit word is ABGR (A high byte, then B, G, R)."""
    a = (v >> 24) & 0xFF
    b = (v >> 16) & 0xFF
    g = (v >> 8) & 0xFF
    r = v & 0xFF
    return r, g, b, a


class PetPreview(tk.Canvas):
    BLINK_CYCLE_MS = 3200
    BLINK_CLOSE_MS = 170
    FRAME_OPEN = 0
    FRAME_CLOSED = 1
    FRAME_SAD = 2
    FRAME_LOW_OPEN = 3
    FRAME_LOW_CLOSED = 4
    FRAME_LOW_SAD = 5
    FRAME_DEAD = 6
    FRAME_SLEEP_LAYER = 7
    FRAME_HAPPY_LAYER = 8

    def __init__(self, parent, size: int = 200, **kwargs):
        super().__init__(
            parent,
            width=size,
            height=size,
            bg=COLORS["bg"],
            highlightthickness=0,
            **kwargs,
        )
        self.size = size
        self._anim_tick = 0
        self._config = None
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._last_key: Optional[tuple[int, int | None]] = None
        self._blink_closed = False
        self._photo_cache: dict[tuple[int, int | None], ImageTk.PhotoImage] = {}

        self._draw_glow()
        self._rebuild_pet()
        self._animate()

    def set_config(self, pet_config) -> None:
        self._config = pet_config
        self._rebuild_pet()

    def refresh(self) -> None:
        self._rebuild_pet()

    def _animate(self) -> None:
        self._anim_tick += 1
        if self._config is not None:
            state_idx = max(0, min(4, int(getattr(self._config, "pet_state", 0))))
            if state_idx in (0, 3):
                phase_ms = (self._anim_tick * 16) % self.BLINK_CYCLE_MS
                blink_closed = phase_ms >= (self.BLINK_CYCLE_MS - self.BLINK_CLOSE_MS)
            else:
                blink_closed = False
            if blink_closed != self._blink_closed:
                self._blink_closed = blink_closed
                self._rebuild_pet()
        self.after(16, self._animate)

    def _rebuild_pet(self) -> None:
        self.delete("pet")
        cx = self.size // 2
        cy = self.size // 2

        if self._config is None:
            return

        state_idx = max(0, min(6, int(getattr(self._config, "pet_state", 0))))
        base_idx = self._frame_index_for_state(state_idx)
        overlay_idx = self._overlay_index_for_state(state_idx)
        key = (base_idx, overlay_idx)
        if not _PIL:
            self.create_text(
                cx, cy, text="Install Pillow\nfor pet preview",
                font=("Segoe UI", 11), fill=COLORS["text_muted"], tags="pet",
            )
            return

        if key != self._last_key or self._photo is None:
            self._last_key = key
            if key in self._photo_cache:
                self._photo = self._photo_cache[key]
            else:
                im = self._build_composited_image(base_idx, overlay_idx)
                scale = max(1, min(self.size - 32, 160) // FRAME_W)
                try:
                    resample = Image.Resampling.NEAREST  # type: ignore[attr-defined]
                except AttributeError:
                    resample = Image.NEAREST
                up = im.resize((FRAME_W * scale, FRAME_H * scale), resample=resample)
                self._photo = ImageTk.PhotoImage(up)
                self._photo_cache[key] = self._photo

        if self._photo:
            self.create_image(cx, cy, image=self._photo, tags="pet")

    def _frame_index_for_state(self, state_idx: int) -> int:
        if state_idx == 0:
            idx = self.FRAME_CLOSED if self._blink_closed else self.FRAME_OPEN
        elif state_idx == 1:
            idx = self.FRAME_CLOSED
        elif state_idx == 2:
            idx = self.FRAME_SAD
        elif state_idx == 3:
            idx = self.FRAME_LOW_CLOSED if self._blink_closed else self.FRAME_LOW_OPEN
        elif state_idx == 4:
            idx = self.FRAME_LOW_CLOSED
        elif state_idx == 5:
            idx = self.FRAME_LOW_SAD
        else:
            idx = self.FRAME_DEAD
        return min(FRAME_COUNT - 1, idx)

    def _overlay_index_for_state(self, state_idx: int) -> int | None:
        sleep_layer = bool(getattr(self._config, "sleep_layer", False))
        happy_layer = bool(getattr(self._config, "happy_layer", False))

        if sleep_layer and state_idx in (1, 2, 4, 5):
            return self.FRAME_SLEEP_LAYER
        if happy_layer and state_idx in (0, 3):
            return self.FRAME_HAPPY_LAYER
        return None

    def _build_composited_image(self, base_idx: int, overlay_idx: int | None):
        base = self._frame_to_image(base_idx)
        if overlay_idx is None:
            return base
        over = self._frame_to_image(overlay_idx)
        return Image.alpha_composite(base, over)

    def _frame_to_image(self, idx: int):
        px = FRAMES[idx]
        im = Image.new("RGBA", (FRAME_W, FRAME_H))
        pix = im.load()
        for i, v in enumerate(px):
            x = i % FRAME_W
            y = i // FRAME_W
            r, g, b, a = _u32_to_rgba(v)
            if a == 0:
                pix[x, y] = (0, 0, 0, 0)
            else:
                pix[x, y] = (r, g, b, a)
        return im

    def _draw_glow(self) -> None:
        steps = 20
        r_max = self.size // 2 - 8
        cx = cy = self.size // 2
        r1 = (0xF2, 0xE8, 0xFF)
        r2 = (0xF7, 0xF4, 0xF0)
        for i in range(steps, 0, -1):
            t = i / steps
            r = int(r1[0] * t + r2[0] * (1 - t))
            g = int(r1[1] * t + r2[1] * (1 - t))
            b = int(r1[2] * t + r2[2] * (1 - t))
            color = f"#{r:02X}{g:02X}{b:02X}"
            radius = int(r_max * t)
            self.create_oval(
                cx - radius, cy - radius,
                cx + radius, cy + radius,
                fill=color, outline="",
                tags="glow",
            )
