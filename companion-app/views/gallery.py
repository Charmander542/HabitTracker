"""
Gallery view — photos captured on the device, pulled over USB serial.

Layout:
  Header: title + date filter dropdown + "Pull from device" button
  3-column grid of photo tiles with hover overlay
  Empty state with SVG-style illustration
"""

from __future__ import annotations
import tkinter as tk
from tkinter import filedialog
from typing import TYPE_CHECKING, Optional

import customtkinter as ctk

try:
    from PIL import Image, ImageTk, ImageDraw
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False

from theme import COLORS, SPACE, heading, body, muted

if TYPE_CHECKING:
    from main import HabitCompanionApp


class GalleryView(ctk.CTkFrame):

    def __init__(self, parent, app, config, serial_bridge, **kwargs):
        super().__init__(parent, fg_color=COLORS["bg"], corner_radius=0, **kwargs)
        self.app = app
        self.config = config
        self.bridge = serial_bridge
        self._photos: list[dict] = []
        self._filter = "All"
        self._pull_progress = 0.0

        self._build()
        self._register_events()

    # ── Build ────────────────────────────────────────────────────────────────

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=1)

        self._build_topbar()
        self._build_grid_area()

    def _build_topbar(self) -> None:
        bar = ctk.CTkFrame(self, fg_color=COLORS["surface"], corner_radius=0, height=56)
        bar.grid(row=0, column=0, sticky="ew")
        bar.grid_propagate(False)
        bar.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            bar, text="Captured moments",
            font=heading(20), text_color=COLORS["text_primary"],
        ).grid(row=0, column=0, padx=SPACE["lg"], sticky="w")

        right = ctk.CTkFrame(bar, fg_color="transparent")
        right.grid(row=0, column=1, padx=SPACE["md"])

        self._filter_var = tk.StringVar(value="All")
        filter_menu = ctk.CTkOptionMenu(
            right, values=["Today", "This Week", "All"],
            variable=self._filter_var,
            font=body(12),
            fg_color=COLORS["surface_alt"],
            text_color=COLORS["text_primary"],
            button_color=COLORS["surface_alt"],
            button_hover_color=COLORS["accent_blue"],
            dropdown_fg_color=COLORS["surface"],
            corner_radius=10, height=34, width=110,
            command=self._on_filter_change,
        )
        filter_menu.pack(side=tk.LEFT, padx=(0, SPACE["sm"]))

        self._pull_btn = ctk.CTkButton(
            right, text="Pull from device",
            font=body(13),
            fg_color=COLORS["accent_blue"],
            text_color=COLORS["text_primary"],
            hover_color=self._darken(COLORS["accent_blue"], 15),
            corner_radius=24, height=34, width=130,
            command=self._pull_photos,
        )
        self._pull_btn.pack(side=tk.LEFT)

        tk.Frame(self, bg=COLORS["border"], height=1).grid(
            row=0, column=0, sticky="sew"
        )

        # Progress bar (hidden initially)
        self._progress_frame = ctk.CTkFrame(self, fg_color="transparent", height=4)
        self._progress_frame.grid(row=0, column=0, sticky="sew")
        self._progress_canvas = tk.Canvas(
            self._progress_frame, height=4, bg=COLORS["surface"], highlightthickness=0,
        )
        self._progress_canvas.pack(fill=tk.X)
        self._progress_frame.grid_remove()

    def _build_grid_area(self) -> None:
        self._scroll = ctk.CTkScrollableFrame(
            self, fg_color="transparent",
            scrollbar_button_color=COLORS["border"],
        )
        self._scroll.grid(row=1, column=0, sticky="nsew",
                          padx=SPACE["xl"], pady=SPACE["xl"])

        self._render_grid()

    # ── Rendering ────────────────────────────────────────────────────────────

    def _render_grid(self) -> None:
        for w in self._scroll.winfo_children():
            w.destroy()

        filtered = self._apply_filter()

        if not filtered:
            self._render_empty_state()
            return

        COLS = 3
        grid = ctk.CTkFrame(self._scroll, fg_color="transparent")
        grid.pack(fill=tk.BOTH, expand=True)
        for c in range(COLS):
            grid.grid_columnconfigure(c, weight=1)

        for i, photo in enumerate(filtered):
            row, col = divmod(i, COLS)
            tile = _PhotoTile(grid, photo=photo)
            tile.grid(row=row, column=col, padx=SPACE["sm"],
                      pady=(0, SPACE["sm"]), sticky="nsew")

    def _render_empty_state(self) -> None:
        frame = ctk.CTkFrame(self._scroll, fg_color="transparent")
        frame.pack(expand=True, pady=SPACE["xxl"])

        # Camera illustration (drawn on canvas)
        cam = tk.Canvas(frame, width=80, height=80,
                        bg=COLORS["bg"], highlightthickness=0)
        cam.pack()
        _draw_camera_illustration(cam, 40, 40)

        ctk.CTkLabel(
            frame,
            text="No photos yet.",
            font=heading(16), text_color=COLORS["text_primary"],
        ).pack(pady=(SPACE["md"], SPACE["xs"]))

        ctk.CTkLabel(
            frame,
            text="Complete a habit on your device to capture one.",
            font=muted(13), text_color=COLORS["text_muted"],
        ).pack()

    def _apply_filter(self) -> list[dict]:
        f = self._filter_var.get()
        if f == "All":
            return list(self._photos)
        elif f == "Today":
            return [p for p in self._photos if "Today" in p.get("ts", "")]
        elif f == "This Week":
            return [p for p in self._photos
                    if any(kw in p.get("ts", "")
                           for kw in ("Today", "Yesterday", "days ago"))]
        return list(self._photos)

    # ── Events ───────────────────────────────────────────────────────────────

    def _register_events(self) -> None:
        self.bridge.on("photo_list", self._on_photo_list)
        self.bridge.on("connected",  lambda p: self.bridge.get_photos())

    def _on_photo_list(self, photos: list[dict]) -> None:
        self._photos = photos
        self._hide_progress()
        self._render_grid()

    def _on_filter_change(self, _value: str) -> None:
        self._render_grid()

    def _pull_photos(self) -> None:
        self._show_progress()
        self.bridge.get_photos()
        self._animate_progress()

    def _show_progress(self) -> None:
        self._progress_frame.grid()
        self._pull_progress = 0.0

    def _hide_progress(self) -> None:
        self._progress_frame.grid_remove()

    def _animate_progress(self) -> None:
        self._pull_progress = min(1.0, self._pull_progress + 0.05)
        c = self._progress_canvas
        c.delete("all")
        w = c.winfo_width() or 400
        fw = int(w * self._pull_progress)
        c.create_rectangle(0, 0, fw, 4, fill=COLORS["accent_blue"], outline="")
        if self._pull_progress < 0.95:
            self.after(100, self._animate_progress)

    @staticmethod
    def _darken(hex_color: str, amt: int) -> str:
        r, g, b = int(hex_color[1:3],16), int(hex_color[3:5],16), int(hex_color[5:7],16)
        return f"#{max(0,r-amt):02X}{max(0,g-amt):02X}{max(0,b-amt):02X}"


# ── PhotoTile ─────────────────────────────────────────────────────────────────

class _PhotoTile(tk.Frame):
    """
    A gallery tile: photo thumbnail (or placeholder) with hover overlay
    showing habit name, timestamp, and save button.
    """

    TILE_W = 180
    TILE_H = 160

    def __init__(self, parent, photo: dict, **kwargs):
        super().__init__(
            parent, bg=COLORS["surface_alt"],
            width=self.TILE_W, height=self.TILE_H,
            cursor="hand2", **kwargs,
        )
        self.pack_propagate(False)
        self.photo = photo
        self._hover = False
        self._overlay_alpha = 0.0

        self._canvas = tk.Canvas(
            self, width=self.TILE_W, height=self.TILE_H,
            bg=COLORS["surface_alt"], highlightthickness=0,
        )
        self._canvas.pack(fill=tk.BOTH, expand=True)
        self._draw_placeholder()

        self._canvas.bind("<Enter>", self._on_enter)
        self._canvas.bind("<Leave>", self._on_leave)
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)

    def _draw_placeholder(self) -> None:
        c = self._canvas
        c.delete("photo")
        w, h = self.TILE_W, self.TILE_H
        col = self.photo.get("color", COLORS["accent_blue"])

        # Soft gradient background
        for i in range(h):
            t = i / h
            r1, g1, b1 = _hex_to_rgb(col)
            r2, g2, b2 = _hex_to_rgb(COLORS["surface"])
            r = int(r1*(1-t) + r2*t)
            g_val = int(g1*(1-t) + g2*t)
            b = int(b1*(1-t) + b2*t)
            color = f"#{r:02X}{g_val:02X}{b:02X}"
            c.create_line(0, i, w, i, fill=color, tags="photo")

        # Center emoji
        emoji = self.photo.get("habit", "📷")[:1] + "📷"
        c.create_text(w//2, h//2 - 10, text="📷",
                      font=("Segoe UI Emoji", 28), fill="white", tags="photo")
        c.create_text(w//2, h//2 + 20,
                      text=self.photo.get("habit", ""),
                      font=("DM Sans", 10), fill="white", tags="photo")

    def _on_enter(self, _event=None) -> None:
        self._hover = True
        self._animate_overlay(1.0)

    def _on_leave(self, _event=None) -> None:
        self._hover = False
        self._animate_overlay(0.0)

    def _animate_overlay(self, target: float) -> None:
        diff = target - self._overlay_alpha
        self._overlay_alpha += diff * 0.25
        self._draw_overlay()
        if abs(diff) > 0.01:
            self.after(16, lambda: self._animate_overlay(target))

    def _draw_overlay(self) -> None:
        c = self._canvas
        c.delete("overlay")
        if self._overlay_alpha < 0.02:
            return

        w, h = self.TILE_W, self.TILE_H
        # White overlay strip from bottom
        ov_h = int(70 * self._overlay_alpha)
        oy = h - ov_h

        # Fade overlay
        steps = max(1, ov_h)
        for i in range(steps):
            t = i / steps
            alpha = int(200 * t * self._overlay_alpha)
            r = g = b = 255
            color = f"#{r:02X}{g:02X}{b:02X}"
            c.create_rectangle(0, oy+i, w, oy+i+1,
                               fill=color, outline="", tags="overlay")

        # Habit + timestamp
        c.create_text(
            12, oy + 14,
            text=f"{self.photo.get('habit','')}",
            font=("DM Sans", 11, "bold"),
            fill=COLORS["text_primary"],
            anchor="w", tags="overlay",
        )
        c.create_text(
            12, oy + 30,
            text=self.photo.get("ts", ""),
            font=("DM Sans", 10),
            fill=COLORS["text_muted"],
            anchor="w", tags="overlay",
        )

        # Save button (icon: down arrow into box)
        bx, by = w - 28, oy + 10
        c.create_rectangle(bx, by, bx+20, by+20,
                           fill=COLORS["surface"], outline=COLORS["border"],
                           tags="overlay")
        c.create_text(bx+10, by+10, text="↓",
                      font=("DM Sans", 12, "bold"),
                      fill=COLORS["text_primary"], tags="overlay")

        # Bind save button click
        c.tag_bind("overlay", "<Button-1>", self._save_photo)

    def _save_photo(self, _event=None) -> None:
        path = filedialog.asksaveasfilename(
            defaultextension=".jpg",
            filetypes=[("JPEG", "*.jpg"), ("All files", "*.*")],
            initialfile=self.photo.get("name", "photo.jpg"),
        )
        if path and self.photo.get("data"):
            with open(path, "wb") as f:
                f.write(self.photo["data"])


# ── Helpers ───────────────────────────────────────────────────────────────────

def _draw_camera_illustration(canvas: tk.Canvas, cx: int, cy: int) -> None:
    """Simple outlined camera illustration for empty state."""
    col = COLORS["text_muted"]
    # Body
    canvas.create_rectangle(cx-28, cy-14, cx+28, cy+18,
                            outline=col, width=2, dash=(4, 3))
    # Lens circle
    canvas.create_oval(cx-10, cy-8, cx+10, cy+8,
                       outline=col, width=2, dash=(4, 3))
    # Viewfinder bump
    canvas.create_rectangle(cx-10, cy-20, cx-2, cy-14,
                            outline=col, width=2, dash=(4, 3))
    # Shutter dot
    canvas.create_oval(cx+18, cy-12, cx+24, cy-6,
                       fill=col, outline="")


def _hex_to_rgb(hex_color: str) -> tuple[int, int, int]:
    h = hex_color.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
