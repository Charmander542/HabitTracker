"""
StatSlider — custom pill-shaped slider for personality stats.

Visual spec:
  - Pill track: 6px tall, surface_alt background, full width
  - Filled portion: accent_blue
  - Thumb: 18px circle, white with 1.5px accent_blue border
  - Value label: DM Sans 13px muted, shown to the right
  - Subtitle: optional description text below the label

All drawn on a tk.Canvas — no default OS widget styling.
"""

from __future__ import annotations
import tkinter as tk
from typing import Callable, Optional

from theme import COLORS, body, muted


class StatSlider(tk.Frame):
    """A labeled personality stat slider (0–100)."""

    TRACK_H = 6
    THUMB_R = 9    # radius of thumb circle
    LABEL_W = 32   # width reserved for value text
    PAD_X = 12     # horizontal padding inside canvas

    def __init__(
        self,
        parent,
        label: str,
        subtitle: str = "",
        value: int = 50,
        color: str = COLORS["accent_blue"],
        on_change: Optional[Callable[[int], None]] = None,
        **kwargs,
    ):
        super().__init__(parent, bg=COLORS["bg"], **kwargs)
        self.value = value
        self.color = color
        self.on_change = on_change
        self._dragging = False

        # Top row: label + value
        top = tk.Frame(self, bg=COLORS["bg"])
        top.pack(fill=tk.X, padx=0, pady=(0, 2))

        self._label_text = tk.Label(
            top, text=label,
            font=body(13, "normal"), fg=COLORS["text_primary"], bg=COLORS["bg"],
        )
        self._label_text.pack(side=tk.LEFT)

        self._value_label = tk.Label(
            top, text=str(value),
            font=muted(13), fg=COLORS["text_muted"], bg=COLORS["bg"],
            width=3, anchor="e",
        )
        self._value_label.pack(side=tk.RIGHT)

        # Canvas for the slider track + thumb
        self._canvas = tk.Canvas(
            self, height=self.THUMB_R * 2 + 4,
            bg=COLORS["bg"], highlightthickness=0,
        )
        self._canvas.pack(fill=tk.X, pady=(0, 2))

        # Subtitle
        if subtitle:
            tk.Label(
                self, text=subtitle,
                font=muted(11), fg=COLORS["text_muted"], bg=COLORS["bg"],
                anchor="w",
            ).pack(fill=tk.X)

        self._canvas.bind("<Configure>", self._on_resize)
        self._canvas.bind("<ButtonPress-1>", self._on_press)
        self._canvas.bind("<B1-Motion>", self._on_drag)
        self._canvas.bind("<ButtonRelease-1>", self._on_release)

        self.after(10, self._draw)

    # ── Public ───────────────────────────────────────────────────────────────

    def set_value(self, value: int) -> None:
        self.value = max(0, min(100, value))
        self._value_label.configure(text=str(self.value))
        self._draw()

    def get_value(self) -> int:
        return self.value

    # ── Drawing ──────────────────────────────────────────────────────────────

    def _draw(self) -> None:
        c = self._canvas
        c.delete("all")

        w = c.winfo_width()
        if w < 20:
            return

        cy = c.winfo_height() // 2
        track_x1 = self.PAD_X
        track_x2 = w - self.PAD_X - self.LABEL_W
        track_y1 = cy - self.TRACK_H // 2
        track_y2 = cy + self.TRACK_H // 2
        r = self.TRACK_H // 2

        # Track background (pill)
        self._pill(c, track_x1, track_y1, track_x2, track_y2,
                   r, COLORS["surface_alt"])

        # Filled portion
        fill_x = track_x1 + int((track_x2 - track_x1) * self.value / 100)
        if fill_x > track_x1 + r * 2:
            self._pill(c, track_x1, track_y1, fill_x, track_y2, r, self.color)

        # Thumb
        thumb_x = fill_x
        thumb_x = max(track_x1 + self.THUMB_R, min(track_x2 - self.THUMB_R, thumb_x))
        c.create_oval(
            thumb_x - self.THUMB_R, cy - self.THUMB_R,
            thumb_x + self.THUMB_R, cy + self.THUMB_R,
            fill="white", outline=self.color, width=1.5,
        )

    @staticmethod
    def _pill(canvas, x1, y1, x2, y2, r, color) -> None:
        """Draw a pill-shaped filled rectangle."""
        canvas.create_rectangle(x1 + r, y1, x2 - r, y2, fill=color, outline="")
        canvas.create_oval(x1, y1, x1 + r*2, y2, fill=color, outline="")
        canvas.create_oval(x2 - r*2, y1, x2, y2, fill=color, outline="")

    # ── Interaction ──────────────────────────────────────────────────────────

    def _on_resize(self, event) -> None:
        self._draw()

    def _on_press(self, event) -> None:
        self._dragging = True
        self._update_value_from_x(event.x)

    def _on_drag(self, event) -> None:
        if self._dragging:
            self._update_value_from_x(event.x)

    def _on_release(self, event) -> None:
        self._dragging = False

    def _update_value_from_x(self, x: int) -> None:
        w = self._canvas.winfo_width()
        track_x1 = self.PAD_X
        track_x2 = w - self.PAD_X - self.LABEL_W
        track_w = track_x2 - track_x1
        if track_w <= 0:
            return
        ratio = (x - track_x1) / track_w
        new_val = int(max(0.0, min(1.0, ratio)) * 100)
        if new_val != self.value:
            self.value = new_val
            self._value_label.configure(text=str(self.value))
            self._draw()
            if self.on_change:
                self.on_change(self.value)
