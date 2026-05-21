"""
Central theme constants for Habit Companion Studio.
All color values, font specs, and spacing live here — never hardcode elsewhere.
"""

from core.animal_pets_data import FRAME_COUNT

COLORS = {
    "bg":           "#F7F4F0",   # warm off-white, like aged paper
    "surface":      "#FFFFFF",   # pure white cards
    "surface_alt":  "#EEF0F5",   # cool light gray for alternating sections
    "accent_pink":  "#F2A7C3",   # soft bubblegum
    "accent_blue":  "#A7C8F2",   # powder blue
    "accent_mint":  "#A7F2D4",   # minty green
    "accent_peach": "#F2C9A7",   # warm peach
    "text_primary": "#2C2C2A",   # near-black, warm
    "text_muted":   "#8C8880",   # warm gray
    "border":       "#E0DDD8",   # barely-there border
    "danger":       "#F2A7A7",   # soft red for low health indicators
    "glow_inner":   "#F2E8FF",   # pet preview glow center
    "overlay":      "#FFFFFF",   # hover overlay on gallery tiles
}

# Spacing unit: 8px grid
SPACE = {
    "xs":  4,
    "sm":  8,
    "md":  16,
    "lg":  24,
    "xl":  32,
    "xxl": 48,
}

# Corner radii
RADIUS = {
    "card":   16,
    "button": 24,
    "input":  10,
    "tag":    12,
    "swatch": 14,  # half of 28px swatch = circle
}

# Font family names (registered at launch by image_utils.ensure_fonts)
FONT_HEADING  = "Nunito"
FONT_BODY     = "DM Sans"
FONT_FALLBACK = "Segoe UI"   # Windows system font if download fails

def heading(size: int = 22, weight: str = "bold") -> tuple:
    return (FONT_HEADING, size, weight)

def body(size: int = 13, weight: str = "normal") -> tuple:
    return (FONT_BODY, size, weight)

def label(size: int = 11, weight: str = "normal") -> tuple:
    return (FONT_BODY, size, weight)

def muted(size: int = 11) -> tuple:
    return (FONT_BODY, size, "normal")

# Personality preset values: motivating, strict, energetic, chatty, dramatic
PERSONALITY_PRESETS = {
    "Gentle":   {"motivating": 90, "strict": 15, "energetic": 40, "chatty": 60, "dramatic": 10},
    "Balanced": {"motivating": 70, "strict": 50, "energetic": 60, "chatty": 70, "dramatic": 40},
    "Coach":    {"motivating": 95, "strict": 85, "energetic": 85, "chatty": 80, "dramatic": 65},
}

# Habit color options
HABIT_COLORS = [
    "#A7C8F2",  # blue
    "#F2A7C3",  # pink
    "#A7F2D4",  # mint
    "#F2C9A7",  # peach
    "#D4C1F2",  # lavender
    "#F2ECA7",  # yellow
]

# Habit emoji presets (24 items)
HABIT_EMOJIS = [
    "💧", "📖", "🧘", "🚶", "😴", "🍎",
    "🏋️", "✍️", "📞", "🎸", "🌱", "🧹",
    "🎯", "💊", "🧴", "🏃", "🧠", "❤️",
    "🎨", "🍵", "🚲", "🌞", "📝", "🎵",
]

# Goal unit options
GOAL_UNITS = ["glasses", "minutes", "times", "pages", "steps", "hours"]

# Duck.c frame layout:
# 0 open, 1 closed, 2 sad, 3 low-open, 4 low-closed, 5 low-sad, 6 dead, 7 sleep layer, 8 happy layer
APP_PET_FRAME_COUNT = 9
PET_STATE_LABELS = [
    "Open",
    "Closed",
    "Sad",
    "Low Open",
    "Low Closed",
    "Low Sad",
    "Dead",
]
PET_OVERLAY_LABELS = ["None", "Sleep", "Happy"]

if FRAME_COUNT != APP_PET_FRAME_COUNT:
    raise RuntimeError(
        f"FRAME_COUNT ({FRAME_COUNT}) must be {APP_PET_FRAME_COUNT} for app subset"
    )

