#pragma once
// =============================================================
// duck_sprite.h — 25x25 pixel duck sprite from Duck.c
//
// Duck.c at the repo root is a Piskel export (ABGR32, 9 frames).
// This module converts those frames into RGB565 + alpha lookup on
// first use and exposes them to the GUI so the character on screen
// matches the Duck that the companion-app Python test shows.
//
// Frame index meaning (same semantics as companion-app/pet_preview.py):
//   0 OPEN          — eyes open, base happy pose
//   1 CLOSED        — eyes closed (blink)
//   2 SAD           — frowny / tired
//   3 LOW_OPEN      — struggling, eyes open
//   4 LOW_CLOSED    — struggling, eyes closed
//   5 LOW_SAD       — struggling + sad
//   6 DEAD          — critical / fainted
//   7 SLEEP_LAYER   — overlay (unused for now)
//   8 HAPPY_LAYER   — overlay (unused for now)
// =============================================================

#include <stdint.h>
#include "pet.h"

#define DUCK_W            25
#define DUCK_H            25
#define DUCK_FRAME_COUNT  9

enum DuckFrameIdx {
  DUCK_FRAME_OPEN       = 0,
  DUCK_FRAME_CLOSED     = 1,
  DUCK_FRAME_SAD        = 2,
  DUCK_FRAME_LOW_OPEN   = 3,
  DUCK_FRAME_LOW_CLOSED = 4,
  DUCK_FRAME_LOW_SAD    = 5,
  DUCK_FRAME_DEAD       = 6,
  DUCK_FRAME_SLEEP_LAY  = 7,
  DUCK_FRAME_HAPPY_LAY  = 8,
};

// Build RGB565 + alpha tables on first call (idempotent).
void duck_init();

// RGB565 pixel data, row-major, DUCK_W * DUCK_H entries.
// Returns the clamped frame if idx is out of range.
const uint16_t* duck_getFrameRGB565(int frameIdx);

// 0x00 = transparent, 0xFF = opaque, per pixel (same layout).
const uint8_t* duck_getFrameAlpha(int frameIdx);

// Map PetState + animFrame counter to a Duck frame index, including
// blink and sad cycles. Mirrors the companion-app mapping so the
// device and the Python preview stay visually identical.
int duck_pickFrameForPet(PetState state, int animFrame);

// Pretty name for a frame index (debug prints).
const char* duck_frameName(int frameIdx);
