"""
Font downloading, caching, and registration utilities.
Fonts are fetched from GitHub on first launch and cached at ~/.habit-companion/fonts/.
On Windows, fonts are registered with GDI so tkinter can use them by family name.
"""

from __future__ import annotations
import sys
import os
import ctypes
import urllib.request
from pathlib import Path

APP_DIR = Path.home() / ".habit-companion"
FONTS_DIR = APP_DIR / "fonts"

# Direct TTF download URLs (Google Fonts GitHub mirrors)
FONT_URLS = {
    "Nunito-Bold.ttf": (
        "https://github.com/googlefonts/nunito/raw/main/fonts/ttf/Nunito-Bold.ttf"
    ),
    "Nunito-Regular.ttf": (
        "https://github.com/googlefonts/nunito/raw/main/fonts/ttf/Nunito-Regular.ttf"
    ),
    "DMSans-Regular.ttf": (
        "https://github.com/googlefonts/dm-fonts/raw/main/fonts/ttf/DMSans-Regular.ttf"
    ),
    "DMSans-Medium.ttf": (
        "https://github.com/googlefonts/dm-fonts/raw/main/fonts/ttf/DMSans-Medium.ttf"
    ),
}


def ensure_fonts() -> None:
    """
    Download fonts if not cached, then register with the OS font system.
    Falls back silently — UI will use system fonts if this fails.
    """
    APP_DIR.mkdir(exist_ok=True)
    FONTS_DIR.mkdir(exist_ok=True)

    for filename, url in FONT_URLS.items():
        dest = FONTS_DIR / filename
        if not dest.exists():
            _try_download(url, dest)

    _register_fonts()


def _try_download(url: str, dest: Path) -> None:
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "HabitCompanion/1.0"})
        with urllib.request.urlopen(req, timeout=10) as response:
            data = response.read()
        dest.write_bytes(data)
    except Exception:
        pass  # network unavailable or rate-limited — use system fonts


def _register_fonts() -> None:
    """Register all cached TTF files with the OS so tkinter can address them."""
    if sys.platform == "win32":
        _register_fonts_windows()
    elif sys.platform == "darwin":
        _register_fonts_macos()
    else:
        _register_fonts_linux()


def _register_fonts_windows() -> None:
    gdi = ctypes.windll.gdi32
    for font_file in FONTS_DIR.glob("*.ttf"):
        path_str = str(font_file)
        result = gdi.AddFontResourceW(path_str)
        if result > 0:
            # Broadcast WM_FONTCHANGE so all windows pick up the new font
            ctypes.windll.user32.SendMessageW(0xFFFF, 0x001D, 0, 0)


def _register_fonts_macos() -> None:
    try:
        from ctypes import cdll, c_char_p, c_void_p
        core_text = cdll.LoadLibrary(
            "/System/Library/Frameworks/CoreText.framework/CoreText"
        )
        for font_file in FONTS_DIR.glob("*.ttf"):
            url = f"file://{font_file}".encode()
            core_text.CTFontManagerRegisterFontsForURL(c_char_p(url), 1, None)
    except Exception:
        pass


def _register_fonts_linux() -> None:
    # Copy fonts to ~/.fonts so fontconfig picks them up
    home_fonts = Path.home() / ".fonts"
    home_fonts.mkdir(exist_ok=True)
    for font_file in FONTS_DIR.glob("*.ttf"):
        dest = home_fonts / font_file.name
        if not dest.exists():
            try:
                dest.write_bytes(font_file.read_bytes())
            except Exception:
                pass


def fonts_ready() -> bool:
    """Return True if at least Nunito Bold was downloaded successfully."""
    return (FONTS_DIR / "Nunito-Bold.ttf").exists()
