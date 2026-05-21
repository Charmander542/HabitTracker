"""
Builder view — Companion creator / character creator screen.

Three-column layout:
  Col 1 (280px fixed): Live pet preview + name input + Send button
  Col 2: Pet sprite (Animal Pets.c frame picker)
  Col 3: Personality stat sliders + preset buttons
"""

from __future__ import annotations
import tkinter as tk
from pathlib import Path
from typing import TYPE_CHECKING

import customtkinter as ctk

from theme import (
    COLORS, SPACE,
    heading, body, muted,
    PERSONALITY_PRESETS,
    FONT_HEADING,
    PET_STATE_LABELS,
    PET_OVERLAY_LABELS,
)
from components.pet_preview import PetPreview
from components.stat_slider import StatSlider

try:
    from PIL import Image
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False

if TYPE_CHECKING:
    from main import HabitCompanionApp


class BuilderView(ctk.CTkFrame):

    def __init__(self, parent, app, config, serial_bridge, **kwargs):
        super().__init__(parent, fg_color=COLORS["bg"], corner_radius=0, **kwargs)
        self.app = app
        self.config = config
        self.bridge = serial_bridge

        self._build()
        self._register_events()

    # ── Build ────────────────────────────────────────────────────────────────

    def _build(self) -> None:
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=1)

        self._build_topbar()
        self._build_columns()

    def _build_topbar(self) -> None:
        bar = ctk.CTkFrame(self, fg_color=COLORS["surface"], corner_radius=0, height=56)
        bar.grid(row=0, column=0, sticky="ew")
        bar.grid_propagate(False)

        ctk.CTkLabel(
            bar, text="Build your companion",
            font=heading(20), text_color=COLORS["text_primary"],
        ).pack(side=tk.LEFT, padx=SPACE["lg"])

        tk.Frame(self, bg=COLORS["border"], height=1).grid(
            row=0, column=0, sticky="sew"
        )

    def _build_columns(self) -> None:
        content = ctk.CTkFrame(self, fg_color=COLORS["bg"], corner_radius=0)
        content.grid(row=1, column=0, sticky="nsew")
        content.grid_columnconfigure(1, weight=1)
        content.grid_columnconfigure(2, weight=1)
        content.grid_rowconfigure(0, weight=1)

        self._build_preview_col(content)
        self._build_appearance_col(content)
        self._build_personality_col(content)

    # ── Column 1: Preview ────────────────────────────────────────────────────

    def _build_preview_col(self, parent) -> None:
        col = ctk.CTkFrame(
            parent, fg_color=COLORS["surface"],
            corner_radius=0, width=280,
            border_width=0,
        )
        col.grid(row=0, column=0, sticky="nsew")
        col.grid_propagate(False)
        col.grid_rowconfigure(1, weight=1)
        col.grid_columnconfigure(0, weight=1)

        tk.Frame(col, bg=COLORS["border"], width=1).place(relx=1.0, rely=0,
                                                           relheight=1.0, anchor="ne")

        preview_wrap = ctk.CTkFrame(col, fg_color="transparent")
        preview_wrap.grid(row=0, column=0, pady=(SPACE["xl"], 0), padx=SPACE["xl"])

        self._preview = PetPreview(preview_wrap, size=200)
        self._preview.pack()
        self._preview.set_config(self.config.pet)

        self._name_var = tk.StringVar(value=self.config.pet.name)
        name_entry = tk.Entry(
            preview_wrap,
            textvariable=self._name_var,
            font=(FONT_HEADING, 18, "bold"),
            fg=COLORS["text_primary"],
            bg=COLORS["surface"],
            bd=0, highlightthickness=0,
            justify="center",
            width=14,
        )
        name_entry.pack(pady=(SPACE["sm"], 0))
        self._name_var.trace_add("write", self._on_name_change)

        name_entry.bind("<FocusIn>",  lambda e: name_entry.configure(
            highlightthickness=1, highlightcolor=COLORS["accent_blue"]))
        name_entry.bind("<FocusOut>", lambda e: name_entry.configure(
            highlightthickness=0))

        ctk.CTkFrame(col, fg_color="transparent").grid(row=1, column=0, sticky="nsew")

        btn_frame = ctk.CTkFrame(col, fg_color="transparent")
        btn_frame.grid(row=2, column=0, sticky="ew", padx=SPACE["lg"], pady=SPACE["xl"])

        self._send_btn = ctk.CTkButton(
            btn_frame, text="Send to Device",
            font=body(13, "bold"),
            fg_color=COLORS["accent_pink"],
            text_color=COLORS["text_primary"],
            hover_color=self._darken(COLORS["accent_pink"], 15),
            corner_radius=24, height=44,
            command=self._send_config,
        )
        self._send_btn.pack(fill=tk.X)

        self._send_status = ctk.CTkLabel(
            btn_frame, text="",
            font=muted(11), text_color=COLORS["text_muted"],
        )
        self._send_status.pack(pady=(SPACE["xs"], 0))

    # ── Column 2: Pet sprite ─────────────────────────────────────────────────

    def _build_appearance_col(self, parent) -> None:
        scroll = ctk.CTkScrollableFrame(
            parent, fg_color=COLORS["bg"],
            scrollbar_button_color=COLORS["border"],
        )
        scroll.grid(row=0, column=1, sticky="nsew",
                    padx=(SPACE["lg"], SPACE["sm"]),
                    pady=SPACE["lg"])

        ctk.CTkLabel(
            scroll, text="Pet",
            font=heading(18), text_color=COLORS["text_primary"],
        ).pack(anchor="w", pady=(0, SPACE["md"]))

        ctk.CTkLabel(
            scroll, text="State",
            font=body(12), text_color=COLORS["text_muted"],
        ).pack(anchor="w", pady=(0, SPACE["sm"]))
        self._state_toggle = ctk.CTkSegmentedButton(
            scroll,
            values=PET_STATE_LABELS,
            font=body(12),
            command=self._on_pet_state_change,
            selected_color=COLORS["accent_blue"],
            selected_hover_color=COLORS["accent_blue"],
            unselected_color=COLORS["surface_alt"],
            unselected_hover_color=COLORS["surface_alt"],
            text_color=COLORS["text_primary"],
            corner_radius=10,
            height=34,
            width=220,
        )
        state_idx = max(0, min(len(PET_STATE_LABELS) - 1, int(getattr(self.config.pet, "pet_state", 0))))
        self._state_toggle.set(PET_STATE_LABELS[state_idx])
        self._state_toggle.pack(anchor="w", pady=(0, SPACE["lg"]))

        ctk.CTkLabel(
            scroll, text="Overlay Layer",
            font=body(12), text_color=COLORS["text_muted"],
        ).pack(anchor="w", pady=(0, SPACE["sm"]))
        self._overlay_toggle = ctk.CTkSegmentedButton(
            scroll,
            values=PET_OVERLAY_LABELS,
            font=body(12),
            command=self._on_overlay_change,
            selected_color=COLORS["accent_blue"],
            selected_hover_color=COLORS["accent_blue"],
            unselected_color=COLORS["surface_alt"],
            unselected_hover_color=COLORS["surface_alt"],
            text_color=COLORS["text_primary"],
            corner_radius=10,
            height=34,
            width=220,
        )
        self._overlay_toggle.set(self._overlay_label_from_config())
        self._overlay_toggle.pack(anchor="w", pady=(0, SPACE["lg"]))

        self._divider(scroll)
        ctk.CTkLabel(
            scroll, text="Watcher Screen Concepts",
            font=body(13, "bold"), text_color=COLORS["text_muted"],
        ).pack(anchor="w", pady=(SPACE["sm"], SPACE["sm"]))
        self._concept_preview = _ScreenConceptPreview(scroll)
        self._concept_preview.pack(anchor="w", pady=(0, SPACE["lg"]))

    # ── Column 3: Personality ────────────────────────────────────────────────

    def _build_personality_col(self, parent) -> None:
        scroll = ctk.CTkScrollableFrame(
            parent, fg_color=COLORS["bg"],
            scrollbar_button_color=COLORS["border"],
        )
        scroll.grid(row=0, column=2, sticky="nsew",
                    padx=(SPACE["sm"], SPACE["lg"]),
                    pady=SPACE["lg"])

        ctk.CTkLabel(
            scroll, text="Personality",
            font=heading(18), text_color=COLORS["text_primary"],
        ).pack(anchor="w", pady=(0, SPACE["md"]))

        p = self.config.personality
        STAT_DEFS = [
            ("motivating", "Motivating",  p.motivating,
             "How often the pet sends encouraging messages"),
            ("strict",     "Strict",      p.strict,
             "How quickly vitality drops on missed habits"),
            ("energetic",  "Energetic",   p.energetic,
             "Speed of animations and bounce intensity"),
            ("chatty",     "Chatty",      p.chatty,
             "How frequently the pet speaks unprompted"),
            ("dramatic",   "Dramatic",    p.dramatic,
             "How extreme the 'dying' animations get"),
        ]

        self._sliders: dict[str, StatSlider] = {}
        for key, lbl, val, sub in STAT_DEFS:
            slider = StatSlider(
                scroll,
                label=lbl,
                subtitle=f"↳ {sub}",
                value=val,
                color=COLORS["accent_blue"],
                on_change=lambda v, k=key: self._on_stat_change(k, v),
            )
            slider.pack(fill=tk.X, pady=(0, SPACE["md"]))
            self._sliders[key] = slider

        self._divider(scroll)

        ctk.CTkLabel(
            scroll, text="Personality Preset",
            font=body(13, "bold"), text_color=COLORS["text_muted"],
        ).pack(anchor="w", pady=(SPACE["md"], SPACE["sm"]))

        preset_row = ctk.CTkFrame(scroll, fg_color="transparent")
        preset_row.pack(anchor="w")

        for preset_name in PERSONALITY_PRESETS:
            ctk.CTkButton(
                preset_row, text=preset_name,
                font=body(12),
                fg_color=COLORS["surface_alt"],
                text_color=COLORS["text_primary"],
                hover_color=COLORS["accent_blue"],
                corner_radius=24, height=32,
                command=lambda p=preset_name: self._apply_preset(p),
            ).pack(side=tk.LEFT, padx=(0, SPACE["sm"]))

    # ── Callbacks ────────────────────────────────────────────────────────────

    def _on_name_change(self, *_) -> None:
        self.config.pet.name = self._name_var.get()

    def _on_pet_state_change(self, state_label: str) -> None:
        try:
            self.config.pet.pet_state = PET_STATE_LABELS.index(state_label)
        except ValueError:
            self.config.pet.pet_state = 0
        self._preview.refresh()

    def _on_overlay_change(self, overlay_label: str) -> None:
        self.config.pet.sleep_layer = overlay_label == "Sleep"
        self.config.pet.happy_layer = overlay_label == "Happy"
        self._preview.refresh()

    def _overlay_label_from_config(self) -> str:
        if getattr(self.config.pet, "sleep_layer", False):
            return "Sleep"
        if getattr(self.config.pet, "happy_layer", False):
            return "Happy"
        return "None"

    def _on_stat_change(self, key: str, value: int) -> None:
        setattr(self.config.personality, key, value)

    def _apply_preset(self, preset_name: str) -> None:
        preset = PERSONALITY_PRESETS[preset_name]
        for key, value in preset.items():
            setattr(self.config.personality, key, value)
            if key in self._sliders:
                self._sliders[key].set_value(value)

    def _send_config(self) -> None:
        self.bridge.send_config(self.config.to_json())
        self._send_status.configure(text="Sending…")
        self.bridge.on("config_ack", self._on_config_ack)

    def _on_config_ack(self, _) -> None:
        self._send_status.configure(text="✓ Saved to device")
        self.after(3000, lambda: self._send_status.configure(text=""))

    # ── Events ───────────────────────────────────────────────────────────────

    def _register_events(self) -> None:
        pass

    # ── Helpers ──────────────────────────────────────────────────────────────

    def _divider(self, parent) -> None:
        tk.Frame(parent, bg=COLORS["border"], height=1).pack(
            fill=tk.X, pady=(0, SPACE["lg"])
        )

    @staticmethod
    def _darken(hex_color: str, amt: int) -> str:
        r, g, b = int(hex_color[1:3],16), int(hex_color[3:5],16), int(hex_color[5:7],16)
        return f"#{max(0,r-amt):02X}{max(0,g-amt):02X}{max(0,b-amt):02X}"


class _ScreenConceptPreview(ctk.CTkFrame):
    def __init__(self, parent, **kwargs):
        super().__init__(parent, fg_color="transparent", **kwargs)
        self._images: dict[str, ctk.CTkImage] = {}
        self._labels = {
            "Home": "home.png",
            "Habit Prompt": "habit_prompt.png",
            "Sleep": "sleep_mode.png",
            "Celebration": "celebration.png",
        }
        self._base_dir = Path(__file__).resolve().parent.parent / "assets" / "screen-concepts"
        self._build()

    def _build(self) -> None:
        self._menu = ctk.CTkSegmentedButton(
            self,
            values=list(self._labels.keys()),
            command=self._on_select,
            font=body(11),
            selected_color=COLORS["accent_blue"],
            selected_hover_color=COLORS["accent_blue"],
            unselected_color=COLORS["surface_alt"],
            unselected_hover_color=COLORS["surface_alt"],
            text_color=COLORS["text_primary"],
            height=32,
            width=260,
        )
        self._menu.pack(anchor="w", pady=(0, SPACE["sm"]))
        self._preview = ctk.CTkLabel(
            self,
            text="",
            fg_color=COLORS["surface_alt"],
            corner_radius=12,
            width=260,
            height=260,
        )
        self._preview.pack(anchor="w")
        first = list(self._labels.keys())[0]
        self._menu.set(first)
        self._on_select(first)

    def _on_select(self, label: str) -> None:
        filename = self._labels[label]
        p = self._base_dir / filename
        if PIL_AVAILABLE and p.exists():
            if filename not in self._images:
                im = Image.open(p)
                self._images[filename] = ctk.CTkImage(light_image=im, dark_image=im, size=(260, 260))
            self._preview.configure(image=self._images[filename], text="")
        else:
            self._preview.configure(image=None, text=f"Missing concept:\n{filename}", font=muted(11))
