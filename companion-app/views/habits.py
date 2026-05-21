"""
Habits view — manage the habit list that lives on the device.

Layout:
  Header row: title + "+ Add habit" button
  Scrollable drag-sortable habit list cards
  Slide-up Add/Edit modal from bottom of list
"""

from __future__ import annotations
import uuid
import tkinter as tk
from tkinter import ttk
from typing import TYPE_CHECKING, Optional

import customtkinter as ctk

from core.config_model import HabitConfig
from theme import (
    COLORS, SPACE,
    heading, body, muted,
    HABIT_COLORS, HABIT_EMOJIS, GOAL_UNITS,
)

if TYPE_CHECKING:
    from main import HabitCompanionApp


class HabitsView(ctk.CTkFrame):

    def __init__(self, parent, app, config, serial_bridge, **kwargs):
        super().__init__(parent, fg_color=COLORS["bg"], corner_radius=0, **kwargs)
        self.app = app
        self.config = config
        self.bridge = serial_bridge
        self._modal: Optional[_HabitModal] = None
        self._editing_index: Optional[int] = None

        self._build()

    # ── Build ────────────────────────────────────────────────────────────────

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=1)

        self._build_topbar()
        self._build_list()

    def _build_topbar(self) -> None:
        bar = ctk.CTkFrame(self, fg_color=COLORS["surface"], corner_radius=0, height=56)
        bar.grid(row=0, column=0, sticky="ew")
        bar.grid_propagate(False)
        bar.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            bar, text="Your habits",
            font=heading(20), text_color=COLORS["text_primary"],
        ).grid(row=0, column=0, padx=SPACE["lg"], sticky="w")

        ctk.CTkButton(
            bar, text="+ Add habit",
            font=body(13),
            fg_color=COLORS["accent_mint"],
            text_color=COLORS["text_primary"],
            hover_color=self._darken(COLORS["accent_mint"], 15),
            corner_radius=24, height=34, width=110,
            command=self._open_add_modal,
        ).grid(row=0, column=1, padx=SPACE["lg"])

        tk.Frame(self, bg=COLORS["border"], height=1).grid(
            row=0, column=0, sticky="sew"
        )

    def _build_list(self) -> None:
        # Container for list + modal
        self._container = ctk.CTkFrame(self, fg_color=COLORS["bg"], corner_radius=0)
        self._container.grid(row=1, column=0, sticky="nsew")
        self._container.grid_columnconfigure(0, weight=1)
        self._container.grid_rowconfigure(0, weight=1)

        self._scroll = ctk.CTkScrollableFrame(
            self._container, fg_color="transparent",
            scrollbar_button_color=COLORS["border"],
        )
        self._scroll.grid(row=0, column=0, sticky="nsew",
                          padx=SPACE["xl"], pady=SPACE["lg"])

        self._refresh_list()

    def _refresh_list(self) -> None:
        for w in self._scroll.winfo_children():
            w.destroy()

        if not self.config.habits:
            ctk.CTkLabel(
                self._scroll,
                text='No habits yet. Click "+ Add habit" to create one.',
                font=muted(13), text_color=COLORS["text_muted"],
            ).pack(pady=SPACE["xxl"])
            return

        for i, habit in enumerate(self.config.habits):
            card = _HabitRow(
                self._scroll, habit=habit,
                on_edit=lambda idx=i: self._open_edit_modal(idx),
                on_delete=lambda idx=i: self._delete_habit(idx),
                on_move_up=lambda idx=i: self._move_habit(idx, -1),
                on_move_down=lambda idx=i: self._move_habit(idx, 1),
                is_first=(i == 0),
                is_last=(i == len(self.config.habits) - 1),
            )
            card.pack(fill=tk.X, pady=(0, SPACE["sm"]))

    # ── Modal ────────────────────────────────────────────────────────────────

    def _open_add_modal(self) -> None:
        self._editing_index = None
        self._show_modal(HabitConfig(id=str(uuid.uuid4())))

    def _open_edit_modal(self, index: int) -> None:
        self._editing_index = index
        self._show_modal(self.config.habits[index])

    def _show_modal(self, habit: HabitConfig) -> None:
        if self._modal:
            self._modal.destroy()
        self._modal = _HabitModal(
            self._container,
            habit=habit,
            on_save=self._on_modal_save,
            on_cancel=self._close_modal,
        )
        self._modal.grid(row=1, column=0, sticky="ew",
                         padx=SPACE["xl"], pady=(0, SPACE["lg"]))
        self._modal.slide_in()

    def _close_modal(self) -> None:
        if self._modal:
            self._modal.slide_out(callback=self._modal.destroy)
            self._modal = None

    def _on_modal_save(self, habit: HabitConfig) -> None:
        if self._editing_index is not None:
            self.config.habits[self._editing_index] = habit
        else:
            self.config.habits.append(habit)
        self._close_modal()
        self._refresh_list()
        self.bridge.send_config(self.config.to_json())

    def _delete_habit(self, index: int) -> None:
        del self.config.habits[index]
        self._refresh_list()
        self.bridge.send_config(self.config.to_json())

    def _move_habit(self, index: int, direction: int) -> None:
        habits = self.config.habits
        new_i = index + direction
        if 0 <= new_i < len(habits):
            habits[index], habits[new_i] = habits[new_i], habits[index]
            self._refresh_list()

    @staticmethod
    def _darken(hex_color: str, amt: int) -> str:
        r, g, b = int(hex_color[1:3],16), int(hex_color[3:5],16), int(hex_color[5:7],16)
        return f"#{max(0,r-amt):02X}{max(0,g-amt):02X}{max(0,b-amt):02X}"


# ── HabitRow ──────────────────────────────────────────────────────────────────

class _HabitRow(ctk.CTkFrame):
    """A single habit card row with drag handle, info, edit and delete."""

    def __init__(self, parent, habit: HabitConfig,
                 on_edit, on_delete, on_move_up, on_move_down,
                 is_first: bool, is_last: bool, **kwargs):
        super().__init__(
            parent, fg_color=COLORS["surface"],
            corner_radius=12, border_width=1,
            border_color=COLORS["border"], **kwargs,
        )
        self.habit = habit

        inner = ctk.CTkFrame(self, fg_color="transparent")
        inner.pack(fill=tk.X, padx=SPACE["md"], pady=SPACE["md"])

        # Drag handle (up/down arrows)
        handle_col = ctk.CTkFrame(inner, fg_color="transparent", width=24)
        handle_col.pack(side=tk.LEFT, padx=(0, SPACE["sm"]))
        handle_col.pack_propagate(False)

        if not is_first:
            ctk.CTkButton(
                handle_col, text="↑", width=20, height=18,
                font=body(10), fg_color="transparent",
                text_color=COLORS["text_muted"],
                hover_color=COLORS["surface_alt"],
                corner_radius=4, command=on_move_up,
            ).pack()
        if not is_last:
            ctk.CTkButton(
                handle_col, text="↓", width=20, height=18,
                font=body(10), fg_color="transparent",
                text_color=COLORS["text_muted"],
                hover_color=COLORS["surface_alt"],
                corner_radius=4, command=on_move_down,
            ).pack()

        # Color dot
        dot = tk.Canvas(inner, width=12, height=12,
                        bg=COLORS["surface"], highlightthickness=0)
        dot.pack(side=tk.LEFT, padx=(0, SPACE["sm"]))
        dot.create_oval(0, 0, 12, 12, fill=habit.color, outline="")

        # Emoji + name
        ctk.CTkLabel(
            inner,
            text=f"{habit.emoji}  {habit.name}",
            font=body(14, "bold"), text_color=COLORS["text_primary"],
        ).pack(side=tk.LEFT)

        # Goal info
        ctk.CTkLabel(
            inner,
            text=f"Goal: {habit.goal}× / {habit.unit}",
            font=muted(12), text_color=COLORS["text_muted"],
        ).pack(side=tk.LEFT, padx=SPACE["lg"])

        # Spacer
        ctk.CTkFrame(inner, fg_color="transparent").pack(side=tk.LEFT, fill=tk.X, expand=True)

        # Action buttons
        ctk.CTkButton(
            inner, text="Edit", width=60, height=28,
            font=body(12),
            fg_color=COLORS["surface_alt"],
            text_color=COLORS["text_primary"],
            hover_color=COLORS["accent_blue"],
            corner_radius=20, command=on_edit,
        ).pack(side=tk.LEFT, padx=(0, SPACE["sm"]))

        ctk.CTkButton(
            inner, text="Delete", width=60, height=28,
            font=body(12),
            fg_color=COLORS["surface_alt"],
            text_color=COLORS["danger"],
            hover_color=COLORS["danger"],
            corner_radius=20, command=on_delete,
        ).pack(side=tk.LEFT)


# ── HabitModal ────────────────────────────────────────────────────────────────

class _HabitModal(ctk.CTkFrame):
    """Slide-up panel for adding/editing a habit."""

    def __init__(self, parent, habit: HabitConfig, on_save, on_cancel, **kwargs):
        super().__init__(
            parent, fg_color=COLORS["surface"],
            corner_radius=16, border_width=1,
            border_color=COLORS["border"], **kwargs,
        )
        self._habit = HabitConfig(
            id=habit.id, name=habit.name, emoji=habit.emoji,
            color=habit.color, goal=habit.goal,
            unit=habit.unit, min_goal=habit.min_goal,
        )
        self.on_save = on_save
        self.on_cancel = on_cancel
        self._build()

    def slide_in(self) -> None:
        # Just appears — animation can be added with place geometry
        pass

    def slide_out(self, callback=None) -> None:
        if callback:
            callback()

    def _build(self) -> None:
        pad = SPACE["lg"]
        inner = ctk.CTkFrame(self, fg_color="transparent")
        inner.pack(fill=tk.BOTH, padx=pad, pady=pad)

        # Title
        ctk.CTkLabel(
            inner,
            text="Add habit" if not self._habit.name else f'Edit "{self._habit.name}"',
            font=heading(16), text_color=COLORS["text_primary"],
        ).pack(anchor="w", pady=(0, SPACE["md"]))

        tk.Frame(inner, bg=COLORS["border"], height=1).pack(fill=tk.X, pady=(0, SPACE["md"]))

        form = ctk.CTkFrame(inner, fg_color="transparent")
        form.pack(fill=tk.X)
        form.grid_columnconfigure(1, weight=1)
        form.grid_columnconfigure(3, weight=1)

        # Row 0: Name + Emoji
        self._field_label(form, "Name", 0, 0)
        self._name_var = tk.StringVar(value=self._habit.name)
        name_entry = ctk.CTkEntry(
            form, textvariable=self._name_var,
            font=body(13), fg_color=COLORS["surface_alt"],
            border_color=COLORS["border"], text_color=COLORS["text_primary"],
            corner_radius=10, height=36,
        )
        name_entry.grid(row=0, column=1, sticky="ew", padx=(SPACE["sm"], SPACE["lg"]))

        self._field_label(form, "Emoji", 0, 2)
        self._emoji_var = tk.StringVar(value=self._habit.emoji)
        emoji_frame = ctk.CTkFrame(form, fg_color="transparent")
        emoji_frame.grid(row=0, column=3, sticky="ew", padx=(SPACE["sm"], 0))
        self._build_emoji_picker(emoji_frame)

        # Row 1: Color
        self._field_label(form, "Color", 1, 0)
        color_frame = ctk.CTkFrame(form, fg_color="transparent")
        color_frame.grid(row=1, column=1, columnspan=3, sticky="w",
                         padx=(SPACE["sm"], 0), pady=(SPACE["sm"], 0))
        from components.color_picker import ColorPicker
        self._color_picker = ColorPicker(
            color_frame, colors=HABIT_COLORS,
            selected=self._habit.color,
            on_select=lambda c: setattr(self._habit, "color", c),
        )
        self._color_picker.pack(side=tk.LEFT)

        # Row 2: Goal amount + unit + min goal
        self._field_label(form, "Goal", 2, 0)
        goal_row = ctk.CTkFrame(form, fg_color="transparent")
        goal_row.grid(row=2, column=1, columnspan=3, sticky="w",
                      padx=(SPACE["sm"], 0), pady=(SPACE["sm"], 0))

        self._goal_var = tk.StringVar(value=str(self._habit.goal))
        ctk.CTkEntry(
            goal_row, textvariable=self._goal_var,
            font=body(13), fg_color=COLORS["surface_alt"],
            border_color=COLORS["border"], text_color=COLORS["text_primary"],
            corner_radius=10, height=36, width=60,
        ).pack(side=tk.LEFT, padx=(0, SPACE["sm"]))

        self._unit_var = tk.StringVar(value=self._habit.unit)
        unit_menu = ctk.CTkOptionMenu(
            goal_row, values=GOAL_UNITS,
            variable=self._unit_var,
            font=body(12),
            fg_color=COLORS["surface_alt"],
            text_color=COLORS["text_primary"],
            button_color=COLORS["surface_alt"],
            button_hover_color=COLORS["accent_blue"],
            dropdown_fg_color=COLORS["surface"],
            corner_radius=10, height=36, width=100,
        )
        unit_menu.pack(side=tk.LEFT, padx=(0, SPACE["md"]))

        ctk.CTkLabel(
            goal_row, text="Min:",
            font=muted(12), text_color=COLORS["text_muted"],
        ).pack(side=tk.LEFT)
        self._min_var = tk.StringVar(value=str(self._habit.min_goal))
        ctk.CTkEntry(
            goal_row, textvariable=self._min_var,
            font=body(13), fg_color=COLORS["surface_alt"],
            border_color=COLORS["border"], text_color=COLORS["text_primary"],
            corner_radius=10, height=36, width=60,
        ).pack(side=tk.LEFT, padx=(SPACE["xs"], 0))

        # Buttons
        btn_row = ctk.CTkFrame(inner, fg_color="transparent")
        btn_row.pack(anchor="e", pady=(SPACE["md"], 0))

        ctk.CTkButton(
            btn_row, text="Cancel",
            font=body(13),
            fg_color=COLORS["surface_alt"],
            text_color=COLORS["text_muted"],
            hover_color=COLORS["border"],
            corner_radius=24, height=38, width=90,
            command=self.on_cancel,
        ).pack(side=tk.LEFT, padx=(0, SPACE["sm"]))

        ctk.CTkButton(
            btn_row, text="Save to device",
            font=body(13, "bold"),
            fg_color=COLORS["accent_mint"],
            text_color=COLORS["text_primary"],
            hover_color=self._darken(COLORS["accent_mint"], 15),
            corner_radius=24, height=38, width=130,
            command=self._save,
        ).pack(side=tk.LEFT)

    def _build_emoji_picker(self, parent) -> None:
        cols = 8
        for i, emoji in enumerate(HABIT_EMOJIS):
            row, col = divmod(i, cols)
            btn = tk.Button(
                parent, text=emoji,
                font=("Segoe UI Emoji", 13),
                bg=COLORS["surface_alt"], relief="flat",
                bd=0, padx=2, pady=2, cursor="hand2",
                command=lambda e=emoji: self._select_emoji(e, parent),
            )
            btn.grid(row=row, column=col, padx=1, pady=1)
            btn.emoji = emoji  # type: ignore[attr-defined]
            if emoji == self._habit.emoji:
                btn.configure(bg=COLORS["accent_pink"])
        self._emoji_buttons_parent = parent

    def _select_emoji(self, emoji: str, parent) -> None:
        self._habit.emoji = emoji
        self._emoji_var.set(emoji)
        for w in parent.winfo_children():
            if hasattr(w, "emoji"):
                w.configure(bg=COLORS["accent_pink"] if w.emoji == emoji
                            else COLORS["surface_alt"])

    def _field_label(self, parent, text: str, row: int, col: int) -> None:
        ctk.CTkLabel(
            parent, text=text,
            font=muted(12), text_color=COLORS["text_muted"],
        ).grid(row=row, column=col, sticky="w",
               padx=(0, 0), pady=(SPACE["sm"], 0))

    def _save(self) -> None:
        self._habit.name = self._name_var.get().strip()
        if not self._habit.name:
            return
        try:
            self._habit.goal = int(self._goal_var.get())
        except ValueError:
            self._habit.goal = 1
        try:
            self._habit.min_goal = int(self._min_var.get())
        except ValueError:
            self._habit.min_goal = 1
        self._habit.unit = self._unit_var.get()
        self.on_save(self._habit)

    @staticmethod
    def _darken(hex_color: str, amt: int) -> str:
        r, g, b = int(hex_color[1:3],16), int(hex_color[3:5],16), int(hex_color[5:7],16)
        return f"#{max(0,r-amt):02X}{max(0,g-amt):02X}{max(0,b-amt):02X}"
