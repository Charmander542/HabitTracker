"""
Dashboard view — live device status hub.

Layout:
  Top bar: connection pill + Sync button
  Left column (wider): Pet status card + Habit progress grid
  Right column: Activity feed
"""

from __future__ import annotations
import math
import tkinter as tk
from typing import TYPE_CHECKING

import customtkinter as ctk

from theme import COLORS, SPACE, heading, body, muted, label

if TYPE_CHECKING:
    from main import HabitCompanionApp


class DashboardView(ctk.CTkFrame):

    def __init__(self, parent, app, config, serial_bridge, **kwargs):
        super().__init__(parent, fg_color=COLORS["bg"], corner_radius=0, **kwargs)
        self.app = app
        self.config = config
        self.bridge = serial_bridge
        self._status: dict = {}
        self._vitality = 78
        self._streak = 0

        self._build()
        self._register_events()

    # ── Build ────────────────────────────────────────────────────────────────

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=1)

        self._build_topbar()
        self._build_body()

    def _build_topbar(self) -> None:
        bar = ctk.CTkFrame(self, fg_color=COLORS["surface"], corner_radius=0, height=56)
        bar.grid(row=0, column=0, sticky="ew")
        bar.grid_propagate(False)
        bar.grid_columnconfigure(1, weight=1)

        # Page title
        ctk.CTkLabel(
            bar, text="Dashboard",
            font=heading(20), text_color=COLORS["text_primary"],
        ).grid(row=0, column=0, padx=SPACE["lg"], pady=0, sticky="w")

        # Right side: status pill + sync
        right = ctk.CTkFrame(bar, fg_color="transparent")
        right.grid(row=0, column=2, padx=SPACE["md"], sticky="e")

        self._status_pill = _StatusPill(right)
        self._status_pill.pack(side=tk.LEFT, padx=(0, SPACE["sm"]))

        ctk.CTkButton(
            right, text="Sync Now",
            font=body(13), fg_color=COLORS["accent_blue"],
            text_color=COLORS["text_primary"],
            hover_color=self._darken(COLORS["accent_blue"], 15),
            corner_radius=24, height=34, width=100,
            command=self._sync_now,
        ).pack(side=tk.LEFT)

        # Bottom border
        tk.Frame(self, bg=COLORS["border"], height=1).grid(
            row=0, column=0, sticky="sew", padx=0
        )

    def _build_body(self) -> None:
        body_frame = ctk.CTkFrame(self, fg_color=COLORS["bg"], corner_radius=0)
        body_frame.grid(row=1, column=0, sticky="nsew", padx=SPACE["xl"], pady=SPACE["xl"])
        body_frame.grid_columnconfigure(0, weight=3)
        body_frame.grid_columnconfigure(1, weight=1)
        body_frame.grid_rowconfigure(0, weight=1)

        # Left column
        left = ctk.CTkFrame(body_frame, fg_color="transparent")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, SPACE["lg"]))
        left.grid_rowconfigure(1, weight=1)
        left.grid_columnconfigure(0, weight=1)

        self._pet_card = _PetStatusCard(left, config=self.config)
        self._pet_card.grid(row=0, column=0, sticky="ew", pady=(0, SPACE["lg"]))

        self._habits_grid = _HabitProgressGrid(left)
        self._habits_grid.grid(row=1, column=0, sticky="nsew")

        # Right column — activity feed
        self._feed = _ActivityFeed(body_frame)
        self._feed.grid(row=0, column=1, sticky="nsew")

    # ── Events ───────────────────────────────────────────────────────────────

    def _register_events(self) -> None:
        self.bridge.on("connected",    self._on_connected)
        self.bridge.on("disconnected", self._on_disconnected)
        self.bridge.on("status",       self._on_status)

    def _on_connected(self, port: str) -> None:
        self._status_pill.set_connected(True, port)

    def _on_disconnected(self, _) -> None:
        self._status_pill.set_connected(False)

    def _on_status(self, data: dict) -> None:
        self._status = data
        self._vitality = data.get("vitality", 78)
        self._streak   = data.get("streak", 0)
        self._pet_card.update(vitality=self._vitality, streak=self._streak)
        self._habits_grid.update(data.get("habits", []))
        self._feed.update(data.get("feed", []))

    def _sync_now(self) -> None:
        self.bridge.get_status()

    @staticmethod
    def _darken(hex_color: str, amt: int) -> str:
        r, g, b = int(hex_color[1:3],16), int(hex_color[3:5],16), int(hex_color[5:7],16)
        return f"#{max(0,r-amt):02X}{max(0,g-amt):02X}{max(0,b-amt):02X}"


# ── Sub-components ────────────────────────────────────────────────────────────

class _StatusPill(tk.Frame):
    """Green/red connection status pill."""

    def __init__(self, parent, **kwargs):
        super().__init__(parent, bg=COLORS["bg"], **kwargs)
        self._canvas = tk.Canvas(self, width=140, height=30,
                                 bg=COLORS["bg"], highlightthickness=0)
        self._canvas.pack()
        self._draw(False, "No device")

    def set_connected(self, connected: bool, port: str = "") -> None:
        text = f"Connected · {port}" if connected else "No device"
        self._draw(connected, text)

    def _draw(self, connected: bool, text: str) -> None:
        c = self._canvas
        c.delete("all")
        bg = "#D4F5E2" if connected else "#FFE8E8"
        dot_col = "#3DBF7A" if connected else "#E05555"
        # Pill bg
        r = 15
        w = min(140, len(text) * 7 + 40)
        c.configure(width=w)
        c.create_rectangle(r, 0, w-r, 30, fill=bg, outline="")
        c.create_oval(0, 0, r*2, 30, fill=bg, outline="")
        c.create_oval(w-r*2, 0, w, 30, fill=bg, outline="")
        # Dot
        c.create_oval(10, 11, 20, 21, fill=dot_col, outline="")
        # Text
        c.create_text(26, 15, text=text, anchor="w",
                      font=body(11), fill=COLORS["text_primary"])


class _PetStatusCard(ctk.CTkFrame):
    """Large center card with pet preview, name, state badge, and vitality ring."""

    def __init__(self, parent, config, **kwargs):
        super().__init__(
            parent, fg_color=COLORS["surface"],
            corner_radius=16, border_width=1,
            border_color=COLORS["border"], **kwargs,
        )
        self.config = config
        self._vitality = 78
        self._streak = 7

        self._build()

    def _build(self) -> None:
        inner = ctk.CTkFrame(self, fg_color="transparent")
        inner.pack(fill=tk.BOTH, expand=True, padx=SPACE["xl"], pady=SPACE["xl"])
        inner.grid_columnconfigure(0, weight=1)

        # Vitality ring + pet name row
        ring_row = ctk.CTkFrame(inner, fg_color="transparent")
        ring_row.grid(row=0, column=0, sticky="ew")
        ring_row.grid_columnconfigure(1, weight=1)

        self._ring = _VitalityRing(ring_row, size=100)
        self._ring.grid(row=0, column=0, padx=(0, SPACE["xl"]))

        name_col = ctk.CTkFrame(ring_row, fg_color="transparent")
        name_col.grid(row=0, column=1, sticky="w")

        self._name_label = ctk.CTkLabel(
            name_col, text=self.config.pet.name,
            font=heading(22), text_color=COLORS["text_primary"],
        )
        self._name_label.pack(anchor="w")

        self._state_badge = _StateBadge(name_col, text="Thriving", color=COLORS["accent_mint"])
        self._state_badge.pack(anchor="w", pady=(SPACE["sm"], 0))

        self._streak_label = ctk.CTkLabel(
            name_col, text=f"🔥 Day {self._streak}",
            font=body(14), text_color=COLORS["text_muted"],
        )
        self._streak_label.pack(anchor="w", pady=(SPACE["sm"], 0))

    def update(self, vitality: int, streak: int) -> None:
        self._vitality = vitality
        self._streak = streak
        self._ring.set_value(vitality)
        self._streak_label.configure(text=f"🔥 Day {streak}")
        self._name_label.configure(text=self.config.pet.name)

        # Update emotional state badge based on vitality
        if vitality >= 70:
            state, color = "Thriving", COLORS["accent_mint"]
        elif vitality >= 40:
            state, color = "Okay", COLORS["accent_peach"]
        else:
            state, color = "Struggling", COLORS["danger"]
        self._state_badge.set(state, color)


class _VitalityRing(tk.Canvas):
    """Circular progress arc (Apple Watch ring style)."""

    def __init__(self, parent, size: int = 100, **kwargs):
        super().__init__(
            parent, width=size, height=size,
            bg=COLORS["surface"], highlightthickness=0, **kwargs,
        )
        self.size = size
        self._value = 78
        self._anim_current = 0.0
        self._draw()
        self._animate()

    def set_value(self, value: int) -> None:
        self._value = value

    def _animate(self) -> None:
        target = self._value
        diff = target - self._anim_current
        self._anim_current += diff * 0.12
        self._draw()
        self.after(16, self._animate)

    def _draw(self) -> None:
        self.delete("all")
        s = self.size
        pad = 8
        x1, y1, x2, y2 = pad, pad, s - pad, s - pad

        # Track
        self.create_arc(x1, y1, x2, y2, start=90, extent=-360,
                        style=tk.ARC, outline=COLORS["surface_alt"], width=10)
        # Filled arc
        extent = -(self._anim_current / 100) * 360
        color = COLORS["accent_mint"] if self._value >= 70 else \
                COLORS["accent_peach"] if self._value >= 40 else COLORS["danger"]
        self.create_arc(x1, y1, x2, y2, start=90, extent=extent,
                        style=tk.ARC, outline=color, width=10)
        # Center text
        cx, cy = s // 2, s // 2
        self.create_text(cx, cy - 6, text=f"{int(self._anim_current)}",
                         font=heading(18), fill=COLORS["text_primary"])
        self.create_text(cx, cy + 10, text="vitality",
                         font=muted(10), fill=COLORS["text_muted"])


class _StateBadge(tk.Frame):
    def __init__(self, parent, text: str, color: str, **kwargs):
        super().__init__(parent, bg=COLORS["surface"], **kwargs)
        self._canvas = tk.Canvas(self, height=24, bg=COLORS["surface"], highlightthickness=0)
        self._canvas.pack()
        self.set(text, color)

    def set(self, text: str, color: str) -> None:
        c = self._canvas
        c.delete("all")
        w = len(text) * 8 + 24
        c.configure(width=w)
        r = 12
        c.create_rectangle(r, 0, w-r, 24, fill=color, outline="")
        c.create_oval(0, 0, r*2, 24, fill=color, outline="")
        c.create_oval(w-r*2, 0, w, 24, fill=color, outline="")
        c.create_text(w//2, 12, text=text, font=body(11, "bold"),
                      fill=COLORS["text_primary"])


class _HabitProgressGrid(ctk.CTkScrollableFrame):
    """Grid of habit progress cards."""

    def __init__(self, parent, **kwargs):
        super().__init__(
            parent, fg_color="transparent",
            scrollbar_button_color=COLORS["border"], **kwargs,
        )
        self._cards: list[_HabitCard] = []

    def update(self, habits: list[dict]) -> None:
        for w in self.winfo_children():
            w.destroy()
        self._cards = []

        if not habits:
            ctk.CTkLabel(
                self, text="No habits — add some in the Habits tab.",
                font=muted(12), text_color=COLORS["text_muted"],
            ).pack(pady=SPACE["lg"])
            return

        header = ctk.CTkLabel(
            self, text="Today's Progress",
            font=heading(16), text_color=COLORS["text_primary"],
        )
        header.pack(anchor="w", pady=(0, SPACE["sm"]))

        for h in habits:
            card = _HabitCard(self, habit=h)
            card.pack(fill=tk.X, pady=(0, SPACE["sm"]))
            self._cards.append(card)


class _HabitCard(ctk.CTkFrame):
    """Single habit progress card with animated progress bar."""

    def __init__(self, parent, habit: dict, **kwargs):
        super().__init__(
            parent, fg_color=COLORS["surface"],
            corner_radius=12, border_width=1,
            border_color=COLORS["border"], **kwargs,
        )
        self.habit = habit
        self._anim_val = 0.0
        self._target_val = habit.get("completed", 0) / max(1, habit.get("goal", 1))

        self._build()
        self._animate()

    def _build(self) -> None:
        row = ctk.CTkFrame(self, fg_color="transparent")
        row.pack(fill=tk.X, padx=SPACE["md"], pady=SPACE["md"])
        row.grid_columnconfigure(1, weight=1)

        # Color dot
        dot_c = tk.Canvas(row, width=12, height=12, bg=COLORS["surface"],
                          highlightthickness=0)
        dot_c.grid(row=0, column=0, rowspan=2, padx=(0, SPACE["sm"]))
        col = self.habit.get("color", COLORS["accent_blue"])
        dot_c.create_oval(0, 0, 12, 12, fill=col, outline="")

        # Name + progress text
        name_frame = ctk.CTkFrame(row, fg_color="transparent")
        name_frame.grid(row=0, column=1, sticky="ew")

        ctk.CTkLabel(
            name_frame, text=f'{self.habit.get("emoji","")} {self.habit.get("name","")}',
            font=body(13, "bold"), text_color=COLORS["text_primary"],
        ).pack(side=tk.LEFT)

        comp = self.habit.get("completed", 0)
        goal = self.habit.get("goal", 1)
        ctk.CTkLabel(
            name_frame, text=f"{comp}/{goal}",
            font=muted(12), text_color=COLORS["text_muted"],
        ).pack(side=tk.RIGHT)

        # Progress bar canvas
        self._bar_canvas = tk.Canvas(
            row, height=8, bg=COLORS["surface"], highlightthickness=0,
        )
        self._bar_canvas.grid(row=1, column=1, sticky="ew", pady=(4, 0))
        self._bar_canvas.bind("<Configure>", lambda e: self._draw_bar())

    def _draw_bar(self) -> None:
        c = self._bar_canvas
        c.delete("all")
        w = c.winfo_width()
        if w < 4:
            return
        h = 8
        r = 4
        col = self.habit.get("color", COLORS["accent_blue"])

        # Track
        c.create_rectangle(r, 0, w-r, h, fill=COLORS["surface_alt"], outline="")
        c.create_oval(0, 0, r*2, h, fill=COLORS["surface_alt"], outline="")
        c.create_oval(w-r*2, 0, w, h, fill=COLORS["surface_alt"], outline="")

        # Fill
        fw = max(r*2, int(w * self._anim_val))
        if fw > r*2:
            c.create_rectangle(r, 0, fw-r, h, fill=col, outline="")
            c.create_oval(0, 0, r*2, h, fill=col, outline="")
            c.create_oval(fw-r*2, 0, fw, h, fill=col, outline="")

    def _animate(self) -> None:
        diff = self._target_val - self._anim_val
        self._anim_val += diff * 0.15
        self._draw_bar()
        if abs(diff) > 0.001:
            self.after(16, self._animate)


class _ActivityFeed(ctk.CTkFrame):
    """Scrollable list of recent device log entries."""

    def __init__(self, parent, **kwargs):
        super().__init__(
            parent, fg_color=COLORS["surface"],
            corner_radius=16, border_width=1,
            border_color=COLORS["border"], **kwargs,
        )
        ctk.CTkLabel(
            self, text="Activity",
            font=heading(16), text_color=COLORS["text_primary"],
        ).pack(anchor="w", padx=SPACE["lg"], pady=(SPACE["lg"], SPACE["sm"]))

        # Divider
        tk.Frame(self, bg=COLORS["border"], height=1).pack(fill=tk.X)

        self._scroll = ctk.CTkScrollableFrame(
            self, fg_color="transparent",
            scrollbar_button_color=COLORS["border"],
        )
        self._scroll.pack(fill=tk.BOTH, expand=True, padx=SPACE["sm"])

    def update(self, feed: list[dict]) -> None:
        for w in self._scroll.winfo_children():
            w.destroy()

        if not feed:
            ctk.CTkLabel(
                self._scroll, text="No activity yet.",
                font=muted(12), text_color=COLORS["text_muted"],
            ).pack(pady=SPACE["lg"])
            return

        for entry in feed:
            self._add_entry(entry)

    def _add_entry(self, entry: dict) -> None:
        row = ctk.CTkFrame(self._scroll, fg_color="transparent")
        row.pack(fill=tk.X, pady=(0, 1))

        col = entry.get("color", COLORS["accent_blue"])
        dot_c = tk.Canvas(row, width=8, height=8,
                          bg=COLORS["surface"], highlightthickness=0)
        dot_c.pack(side=tk.LEFT, padx=(SPACE["sm"], SPACE["sm"]), pady=10)
        dot_c.create_oval(0, 0, 8, 8, fill=col, outline="")

        text_col = ctk.CTkFrame(row, fg_color="transparent")
        text_col.pack(side=tk.LEFT, fill=tk.X, expand=True)

        ts = entry.get("ts", "")
        habit = entry.get("habit", "")
        log = entry.get("log", "")
        full = f"{ts} · {habit} · {log}"

        ctk.CTkLabel(
            text_col, text=full,
            font=body(12), text_color=COLORS["text_muted"],
            anchor="w",
        ).pack(anchor="w")

        # Dashed divider
        tk.Frame(self._scroll, bg=COLORS["border"], height=1).pack(
            fill=tk.X, padx=SPACE["sm"]
        )
