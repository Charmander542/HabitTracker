"""
Data model for CompanionConfig — the full configuration sent to/from the device.
All serialization/deserialization lives here.
"""

from __future__ import annotations
import json
from dataclasses import dataclass, field, asdict, fields
from pathlib import Path
from typing import Any

from core.animal_pets_data import FRAME_COUNT

APP_DIR = Path.home() / ".habit-companion"
CONFIG_PATH = APP_DIR / "config.json"

_STATES_PER_ANIMAL = 4
_ANIMAL_COUNT = 9
_PET_INDEX_MAX = 0
_PET_STATE_MAX = 6  # open, closed, sad, low-open, low-closed, low-sad, dead


@dataclass
class PetConfig:
    """Companion appearance: duck state + optional info overlays."""
    name: str = "Mochi"
    pet_index: int = 0  # fixed duck sprite set
    pet_state: int = 0  # 0..6 base duck state
    sleep_layer: bool = False
    happy_layer: bool = False

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "PetConfig":
        known = {f.name for f in fields(cls)}
        filtered = {k: v for k, v in d.items() if k in known}

        raw_index = filtered.get("pet_index", 0)
        try:
            idx = int(raw_index)
        except (TypeError, ValueError):
            idx = 0

        # Backward compatibility: older configs stored frame index directly.
        has_state = "pet_state" in d
        if not has_state and idx >= _ANIMAL_COUNT:
            state = idx % _STATES_PER_ANIMAL
            idx = idx // _STATES_PER_ANIMAL
        else:
            try:
                state = int(filtered.get("pet_state", 0))
            except (TypeError, ValueError):
                state = 0
            # Migrate older 5-state config to duck-only states only when no layer flags exist.
            legacy_state = ("sleep_layer" not in d and "happy_layer" not in d and state <= 4)
            if legacy_state:
                if state == 4:
                    state = 6
                elif state == 3:
                    state = 4

        idx = 0
        state = max(0, min(_PET_STATE_MAX, state))
        name = filtered.get("name", "Mochi")
        if not isinstance(name, str):
            name = "Mochi"
        sleep_layer = bool(filtered.get("sleep_layer", False))
        happy_layer = bool(filtered.get("happy_layer", False))
        return cls(name=name, pet_index=idx, pet_state=state, sleep_layer=sleep_layer, happy_layer=happy_layer)


@dataclass
class PersonalityConfig:
    # Each value maps to a firmware constant controlling device behavior.
    motivating: int = 82   # How often the pet sends encouraging messages (0–100)
    strict: int = 40       # How quickly vitality drops on missed habits (0–100)
    energetic: int = 70    # Speed of animations and bounce intensity (0–100)
    chatty: int = 95       # How frequently the pet speaks unprompted (0–100)
    dramatic: int = 30     # How extreme the "dying" animations get (0–100)


@dataclass
class HabitConfig:
    id: str = ""
    name: str = ""
    emoji: str = "💧"
    color: str = "#A7C8F2"
    goal: int = 8
    unit: str = "times"    # glasses | minutes | times | pages | steps | hours
    min_goal: int = 3      # floor for dynamic adaptation


@dataclass
class CompanionConfig:
    pet: PetConfig = field(default_factory=PetConfig)
    personality: PersonalityConfig = field(default_factory=PersonalityConfig)
    habits: list[HabitConfig] = field(default_factory=lambda: [
        HabitConfig(
            id="hydrate",
            name="Hydrate",
            emoji="💧",
            color="#A7C8F2",
            goal=8,
            unit="glasses",
            min_goal=3,
        )
    ])

    def to_json(self) -> str:
        """Serialize to JSON for sending to device."""
        d = {
            "pet": asdict(self.pet),
            "personality": asdict(self.personality),
            "habits": [asdict(h) for h in self.habits],
        }
        return json.dumps(d, indent=2)

    @classmethod
    def from_dict(cls, d: dict) -> "CompanionConfig":
        pet = PetConfig.from_dict(d.get("pet", {}) or {})
        personality = PersonalityConfig(**d.get("personality", {}))
        habits = [HabitConfig(**h) for h in d.get("habits", [])]
        return cls(pet=pet, personality=personality, habits=habits)


def load_config() -> CompanionConfig:
    """Load config from local backup, or return defaults."""
    APP_DIR.mkdir(exist_ok=True)
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
            return CompanionConfig.from_dict(data)
        except Exception:
            pass
    return CompanionConfig()


def save_config(config: CompanionConfig) -> None:
    """Persist config to local backup file."""
    APP_DIR.mkdir(exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        f.write(config.to_json())
