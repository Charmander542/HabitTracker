"""
Habit Companion Studio — Desktop app for the SenseCAP Watcher Habit Companion firmware.

Usage:
  python main.py          # normal mode (connects to device over USB serial)
  python main.py --demo   # demo mode (mock data, no device required)
"""

from __future__ import annotations
import sys
import argparse
from pathlib import Path

# Ensure companion-app root is on the path when run from a subdirectory
_HERE = Path(__file__).parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import customtkinter as ctk
import tkinter as tk

from core.config_model import CompanionConfig, load_config, save_config
from core.serial_bridge import SerialBridge
from core.image_utils import ensure_fonts
from components.nav_rail import NavRail
from views.dashboard import DashboardView
from views.builder import BuilderView
from views.habits import HabitsView
from views.gallery import GalleryView
from theme import COLORS, heading


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Habit Companion Studio — configure your SenseCAP Watcher pet.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="  --demo    Run with mock data (no physical device needed)\n",
    )
    parser.add_argument(
        "--demo", action="store_true",
        help="Run with mock data — no Watcher device required.",
    )
    return parser.parse_args()


class HabitCompanionApp(ctk.CTk):
    """Main application window."""

    def __init__(self, demo_mode: bool = False) -> None:
        super().__init__()
        self.demo_mode = demo_mode

        # Core state
        self.companion_config: CompanionConfig = load_config()
        self.bridge = SerialBridge(demo_mode=demo_mode)

        self._setup_window()
        self._build_ui()
        self._start_services()

        # Default to dashboard
        self.navigate_to("dashboard")

    # ── Window setup ─────────────────────────────────────────────────────────

    def _setup_window(self) -> None:
        self.title("Habit Companion Studio" + (" [DEMO]" if self.demo_mode else ""))
        self.geometry("1200x800")
        self.minsize(900, 640)
        self.configure(fg_color=COLORS["bg"])

        ctk.set_appearance_mode("light")
        ctk.set_default_color_theme("blue")

        # Center on screen
        self.update_idletasks()
        w, h = 1200, 800
        sw = self.winfo_screenwidth()
        sh = self.winfo_screenheight()
        x = (sw - w) // 2
        y = (sh - h) // 2
        self.geometry(f"{w}x{h}+{x}+{y}")

    # ── UI layout ────────────────────────────────────────────────────────────

    def _build_ui(self) -> None:
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # Left navigation rail
        self.nav_rail = NavRail(self, on_navigate=self.navigate_to)
        self.nav_rail.grid(row=0, column=0, sticky="nsew")

        # Content area (right of nav rail)
        self.content = ctk.CTkFrame(self, fg_color=COLORS["bg"], corner_radius=0)
        self.content.grid(row=0, column=1, sticky="nsew")
        self.content.grid_columnconfigure(0, weight=1)
        self.content.grid_rowconfigure(0, weight=1)

        # Instantiate all views (stacked, only one visible at a time)
        self.views: dict[str, ctk.CTkFrame] = {}
        for name, cls in [
            ("dashboard", DashboardView),
            ("builder",   BuilderView),
            ("habits",    HabitsView),
            ("gallery",   GalleryView),
        ]:
            view = cls(
                self.content,
                app=self,
                config=self.companion_config,
                serial_bridge=self.bridge,
            )
            view.grid(row=0, column=0, sticky="nsew")
            view.grid_remove()
            self.views[name] = view

        self._current_view: str = ""

    # ── Navigation ───────────────────────────────────────────────────────────

    def navigate_to(self, view_name: str) -> None:
        if self._current_view:
            self.views[self._current_view].grid_remove()
        self.views[view_name].grid()
        self._current_view = view_name
        self.nav_rail.set_active(view_name)

    # ── Services ─────────────────────────────────────────────────────────────

    def _start_services(self) -> None:
        self.bridge.start()
        self._poll_bridge()

    def _poll_bridge(self) -> None:
        """Drain the serial bridge event queue every 200 ms on the UI thread."""
        self.bridge.process_queue(self)
        self.after(200, self._poll_bridge)

    # ── Lifecycle ────────────────────────────────────────────────────────────

    def on_closing(self) -> None:
        save_config(self.companion_config)
        self.bridge.stop()
        self.destroy()


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    args = parse_args()

    # Download and register fonts (silently falls back to system fonts on failure)
    ensure_fonts()

    app = HabitCompanionApp(demo_mode=args.demo)
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()


if __name__ == "__main__":
    main()
