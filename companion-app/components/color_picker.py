"""
ColorPicker — palette-based swatch selector.

Renders a row of circular color swatches (28px diameter).
Selected swatch gets a 2px outline border in accent_pink.
"""

from __future__ import annotations
import tkinter as tk
from typing import Callable, List, Optional

from theme import COLORS


class ColorPicker(tk.Frame):
    """A horizontal row of circular color swatches."""

    SWATCH_D = 28       # diameter of each swatch circle
    SWATCH_GAP = 8      # gap between swatches
    BORDER_W = 2        # selection border width

    def __init__(
        self,
        parent,
        colors: List[str],
        selected: Optional[str] = None,
        on_select: Optional[Callable[[str], None]] = None,
        allow_none: bool = False,   # include an explicit "none" option
        **kwargs,
    ):
        super().__init__(parent, bg=COLORS["bg"], **kwargs)
        self.colors = colors
        self.selected = selected or (colors[0] if colors else None)
        self.on_select = on_select
        self.allow_none = allow_none

        self._build()

    # ── Public ───────────────────────────────────────────────────────────────

    def set_selected(self, color: Optional[str]) -> None:
        self.selected = color
        self._refresh_borders()

    def get_selected(self) -> Optional[str]:
        return self.selected

    # ── Build ────────────────────────────────────────────────────────────────

    def _build(self) -> None:
        for widget in self.winfo_children():
            widget.destroy()

        swatch_list = list(self.colors)
        if self.allow_none:
            swatch_list.append(None)

        self._swatches: List[tk.Canvas] = []
        for color in swatch_list:
            c = self._make_swatch(color)
            c.pack(side=tk.LEFT, padx=(0, self.SWATCH_GAP))
            self._swatches.append(c)

        self._refresh_borders()

    def _make_swatch(self, color: Optional[str]) -> tk.Canvas:
        d = self.SWATCH_D + self.BORDER_W * 2 + 2
        canvas = tk.Canvas(
            self, width=d, height=d,
            bg=COLORS["bg"], highlightthickness=0,
        )
        canvas.color = color  # type: ignore[attr-defined]

        if color is None:
            # "none" swatch: white circle with diagonal line
            r = self.SWATCH_D // 2
            off = self.BORDER_W + 1
            canvas.create_oval(
                off, off, off + self.SWATCH_D, off + self.SWATCH_D,
                fill="white", outline=COLORS["border"], width=1,
                tags="circle",
            )
            canvas.create_line(off + 4, off + self.SWATCH_D - 4,
                               off + self.SWATCH_D - 4, off + 4,
                               fill=COLORS["text_muted"], width=1.5, tags="cross")
        else:
            off = self.BORDER_W + 1
            canvas.create_oval(
                off, off, off + self.SWATCH_D, off + self.SWATCH_D,
                fill=color, outline="", tags="circle",
            )

        canvas.bind("<Button-1>", lambda e, col=color: self._on_click(col))
        canvas.bind("<Enter>", lambda e, cv=canvas: self._on_hover(cv, True))
        canvas.bind("<Leave>", lambda e, cv=canvas: self._on_hover(cv, False))
        return canvas

    def _refresh_borders(self) -> None:
        for c in self._swatches:
            col = c.color  # type: ignore[attr-defined]
            off = self.BORDER_W + 1
            d = self.SWATCH_D

            # Remove old border oval
            c.delete("border")

            if col == self.selected:
                c.create_oval(
                    off - self.BORDER_W, off - self.BORDER_W,
                    off + d + self.BORDER_W, off + d + self.BORDER_W,
                    outline=COLORS["accent_pink"],
                    width=self.BORDER_W,
                    tags="border",
                )
                # Keep border below circle fill
                c.tag_lower("border", "circle")
            else:
                c.create_oval(
                    off, off, off + d, off + d,
                    outline=COLORS["border"], width=1,
                    tags="border",
                )
                c.tag_lower("border", "circle")

    # ── Interaction ──────────────────────────────────────────────────────────

    def _on_click(self, color: Optional[str]) -> None:
        self.selected = color
        self._refresh_borders()
        if self.on_select:
            self.on_select(color)

    def _on_hover(self, canvas: tk.Canvas, entering: bool) -> None:
        col = canvas.color  # type: ignore[attr-defined]
        if col != self.selected:
            canvas.delete("border")
            off = self.BORDER_W + 1
            d = self.SWATCH_D
            outline = COLORS["text_muted"] if entering else COLORS["border"]
            canvas.create_oval(
                off, off, off + d, off + d,
                outline=outline, width=1, tags="border",
            )
            canvas.tag_lower("border", "circle")
