"""
NavRail — collapsible left navigation sidebar.

Collapsed: 72px wide, icons only.
Expanded: 200px wide, icons + labels.
Active item has a filled pill background in accent_pink.
Toggle button at the bottom expands/collapses with animation.
"""

from __future__ import annotations
import tkinter as tk
from typing import Callable, Optional

from theme import COLORS, heading, body

# Navigation destinations
NAV_ITEMS = [
    ("dashboard", "Dashboard"),
    ("builder",   "Build"),
    ("habits",    "Habits"),
    ("gallery",   "Gallery"),
]

EXPANDED_W  = 200


class NavRail(tk.Frame):

    def __init__(self, parent, on_navigate: Callable[[str], None], **kwargs):
        super().__init__(
            parent,
            bg=COLORS["surface"],
            width=EXPANDED_W,
            **kwargs,
        )
        self.pack_propagate(False)
        self.on_navigate = on_navigate
        self._active: Optional[str] = None
        self._expanded = True

        self._build()
        self._add_border()

    # ── Public ───────────────────────────────────────────────────────────────

    def set_active(self, view_name: str) -> None:
        self._active = view_name
        self._refresh_items()

    # ── Build ────────────────────────────────────────────────────────────────

    def _build(self) -> None:
        # App logo / title area
        self._header = tk.Frame(self, bg=COLORS["surface"], height=64)
        self._header.pack(fill=tk.X)
        self._header.pack_propagate(False)

        self._logo_label = tk.Label(
            self._header, text="🐾",
            font=("Segoe UI Emoji", 24), bg=COLORS["surface"],
        )
        self._logo_label.place(relx=0.5, rely=0.5, anchor="center")

        # Divider
        tk.Frame(self, bg=COLORS["border"], height=1).pack(fill=tk.X)

        # Nav items
        self._items_frame = tk.Frame(self, bg=COLORS["surface"])
        self._items_frame.pack(fill=tk.X, pady=8)

        self._item_widgets: dict[str, _NavItem] = {}
        for key, label in NAV_ITEMS:
            item = _NavItem(
                self._items_frame,
                key=key,
                label=label,
                on_click=self._on_item_click,
                expanded=self._expanded,
            )
            item.pack(fill=tk.X, padx=8, pady=2)
            self._item_widgets[key] = item

        # Spacer
        tk.Frame(self, bg=COLORS["surface"]).pack(fill=tk.BOTH, expand=True)

        # Divider
        tk.Frame(self, bg=COLORS["border"], height=1).pack(fill=tk.X)

    def _add_border(self) -> None:
        """Right-edge 1px border."""
        border = tk.Frame(self, bg=COLORS["border"], width=1)
        border.place(relx=1.0, rely=0, relheight=1.0, anchor="ne")

    def _refresh_items(self) -> None:
        for key, item in self._item_widgets.items():
            item.set_active(key == self._active)

    def _on_item_click(self, key: str) -> None:
        self._active = key
        self._refresh_items()
        self.on_navigate(key)

# ── _NavItem ──────────────────────────────────────────────────────────────────

def draw_icon_dashboard(canvas, cx, cy, color):
    """2×2 grid squares icon."""
    g, sq = 2, 6
    for dx in [-(sq+g//2), g//2]:
        for dy in [-(sq+g//2), g//2]:
            canvas.create_rectangle(
                cx+dx, cy+dy, cx+dx+sq, cy+dy+sq,
                outline=color, width=1.5, tags="icon",
            )

def draw_icon_builder(canvas, cx, cy, color):
    """Magic wand + sparkle."""
    canvas.create_line(cx-8, cy+8, cx+5, cy-5, fill=color, width=2, tags="icon")
    canvas.create_line(cx+5, cy-5, cx+8, cy-8, fill=color, width=1.5, tags="icon")
    for dx, dy in [(8, -12), (12, -6), (10, -14)]:
        r = 1.5
        canvas.create_oval(cx+dx-r, cy+dy-r, cx+dx+r, cy+dy+r,
                           fill=color, outline="", tags="icon")

def draw_icon_habits(canvas, cx, cy, color):
    """Checklist lines."""
    for i, (checked, y_off) in enumerate([(True, -6), (False, 0), (True, 6)]):
        y = cy + y_off
        if checked:
            canvas.create_line(cx-9, y, cx-6, y+3, cx-3, y-2,
                               fill=color, width=1.5, tags="icon")
        else:
            canvas.create_oval(cx-10, y-2, cx-6, y+2,
                               outline=color, width=1.5, tags="icon")
        canvas.create_line(cx-2, y, cx+9, y, fill=color, width=1.5, tags="icon")

def draw_icon_gallery(canvas, cx, cy, color):
    """Camera body."""
    canvas.create_rectangle(cx-9, cy-5, cx+9, cy+7,
                            outline=color, width=1.5, tags="icon")
    canvas.create_arc(cx-5, cy-2, cx+5, cy+5,
                      start=0, extent=360, outline=color, width=1.5, tags="icon")
    canvas.create_rectangle(cx-4, cy-9, cx, cy-5,
                            outline=color, width=1.5, tags="icon")

ICON_DRAWERS = {
    "dashboard": draw_icon_dashboard,
    "builder":   draw_icon_builder,
    "habits":    draw_icon_habits,
    "gallery":   draw_icon_gallery,
}


class _NavItem(tk.Frame):
    """A single navigation rail item (icon + optional label)."""

    ITEM_H = 44
    ICON_SIZE = 18
    ICON_X = 22

    def __init__(
        self,
        parent,
        key: str,
        label: str,
        on_click: Callable[[str], None],
        expanded: bool,
        icon_draw=None,
    ):
        super().__init__(parent, bg=COLORS["surface"], cursor="hand2")
        self.key = key
        self._label_text = label
        self.on_click = on_click
        self._is_active = False
        self._expanded = expanded
        self._icon_draw = icon_draw or ICON_DRAWERS.get(key, lambda *a: None)
        self._hover = False

        self._build()
        self.bind("<Button-1>", self._on_click)
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)

    def set_active(self, active: bool) -> None:
        self._is_active = active
        self._draw()

    def set_expanded(self, expanded: bool) -> None:
        self._expanded = expanded
        self._draw()

    def set_label(self, text: str) -> None:
        self._label_text = text
        self._draw()

    def _build(self) -> None:
        self.configure(height=self.ITEM_H)
        self.pack_propagate(False)

        # Single row canvas avoids mixed widget highlight artifacts.
        self._canvas = tk.Canvas(
            self,
            height=self.ITEM_H,
            bg=COLORS["surface"], highlightthickness=0,
        )
        self._canvas.pack(fill=tk.BOTH, expand=True)
        self._canvas.bind("<Button-1>", self._on_click)
        self._canvas.bind("<Enter>", self._on_enter)
        self._canvas.bind("<Leave>", self._on_leave)
        self._canvas.bind("<Configure>", lambda _e: self._draw())

        self._draw()

    def _draw(self) -> None:
        c = self._canvas
        c.delete("all")
        w = c.winfo_width() or (self.ICON_SIZE + self.PILL_PAD_X * 2)
        h = self.ITEM_H
        cx = self.ICON_X if self._expanded else w // 2
        cy = h // 2

        if self._is_active:
            # Full-row filled pill for clean active state.
            r = 16
            x1, y1 = 4, 6
            x2, y2 = w - 4, h - 6
            c.create_rectangle(x1+r, y1, x2-r, y2, fill=COLORS["accent_pink"], outline="")
            c.create_oval(x1, y1, x1+r*2, y2, fill=COLORS["accent_pink"], outline="")
            c.create_oval(x2-r*2, y1, x2, y2, fill=COLORS["accent_pink"], outline="")
            icon_color = COLORS["text_primary"]
        elif self._hover:
            r = 16
            x1, y1 = 4, 6
            x2, y2 = w - 4, h - 6
            c.create_rectangle(x1+r, y1, x2-r, y2, fill=COLORS["surface_alt"], outline="")
            c.create_oval(x1, y1, x1+r*2, y2, fill=COLORS["surface_alt"], outline="")
            c.create_oval(x2-r*2, y1, x2, y2, fill=COLORS["surface_alt"], outline="")
            icon_color = COLORS["text_primary"]
        else:
            icon_color = COLORS["text_muted"]

        self._icon_draw(c, cx, cy, icon_color)

        if self._expanded:
            text_x = 44
            text_color = COLORS["text_primary"] if (self._is_active or self._hover) else COLORS["text_muted"]
            c.create_text(
                text_x, cy, text=self._label_text, anchor="w",
                font=body(13, "normal"), fill=text_color
            )

    def _on_click(self, _event=None) -> None:
        self.on_click(self.key)

    def _on_enter(self, _event=None) -> None:
        self._hover = True
        self._draw()

    def _on_leave(self, _event=None) -> None:
        self._hover = False
        self._draw()
