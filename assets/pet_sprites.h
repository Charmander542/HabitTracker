#pragma once
// =============================================================
// pet_sprites.h — Custom pixel-art sprite definitions
//
// HOW TO USE THIS FILE:
//   1. Draw 64×64 pixel art for each pet state in any pixel editor
//      (Aseprite, LibreSprite, Piskel, etc.)
//   2. Export as PNG files into assets/sprites/:
//        thriving.png, happy.png, tired.png, struggling.png, critical.png
//   3. Run the converter:
//        python tools/img2sprite.py assets/sprites/ assets/pet_sprites.h
//   4. The script will overwrite this file with real RGB565 arrays.
//   5. In gui.cpp, call _canvas->drawRGBBitmap(x, y, SPRITE_THRIVING, 64, 64)
//      instead of the procedural drawPetFace() to use your pixel art.
//
// CURRENT STATE:
//   The firmware uses procedural drawing by default (see gui.cpp).
//   The structs below define the interface for custom sprites.
//   Replace the placeholder arrays with real pixel data once you
//   have artwork ready.
//
// SPRITE FORMAT: 64×64 pixels, RGB565 little-endian, row-major order.
//   Total size per sprite: 64 × 64 × 2 = 8,192 bytes.
//   All 5 states = 40,960 bytes — fits comfortably in 8MB PSRAM.
//
// ANIMATION FRAMES:
//   For animated states (THRIVING, CRITICAL), provide multiple
//   64×64 frames. The animFrame counter in pet.cpp cycles 0–63.
//   Lay out frames consecutively: SPRITE_THRIVING_FRAMES[frame][pixel].
// =============================================================

#include <stdint.h>

// Number of pixels in one 64×64 sprite frame
#define SPRITE_WIDTH   64
#define SPRITE_HEIGHT  64
#define SPRITE_PIXELS  (SPRITE_WIDTH * SPRITE_HEIGHT)   // 4096

// Number of animation frames per state
// (Increase for smoother animation; each frame costs 8KB of flash/PSRAM)
#define SPRITE_FRAMES_THRIVING   4    // Bouncing + sparkle eyes
#define SPRITE_FRAMES_HAPPY      2    // Gentle blink cycle
#define SPRITE_FRAMES_TIRED      2    // Slow droop cycle
#define SPRITE_FRAMES_STRUGGLING 4    // Shake + worry cycle
#define SPRITE_FRAMES_CRITICAL   8    // Flicker + ghost cycle

// =============================================================
// PLACEHOLDER ARRAYS — replace these with real pixel data output
// from tools/img2sprite.py once you have artwork.
//
// These are intentionally left empty here. The firmware uses
// procedural drawing (gui.cpp drawPetFace()) by default, so
// no sprite data is needed until you add pixel art.
//
// When img2sprite.py runs, it will OVERWRITE this entire file
// with fully populated uint16_t arrays.
//
// If you want to hand-edit placeholder frames before running
// the script, each frame must be exactly SPRITE_PIXELS values.
// =============================================================

// Forward declarations — the actual arrays are populated by img2sprite.py.
// Until then, the firmware uses procedural pet face rendering (see gui.cpp).
//
// Example of what img2sprite.py generates (2 frames for "happy"):
//
//   static const uint16_t SPRITE_HAPPY_F0[SPRITE_PIXELS] = { 0xFDD7, 0xFDD7, ... };
//   static const uint16_t SPRITE_HAPPY_F1[SPRITE_PIXELS] = { 0xFCB5, 0xFCB5, ... };
//   static const uint16_t* const SPRITE_HAPPY[2] = { SPRITE_HAPPY_F0, SPRITE_HAPPY_F1 };
//
// TODO: [Run python tools/img2sprite.py assets/sprites/ assets/pet_sprites.h
//        after placing 64×64 PNG files in assets/sprites/ to populate these arrays.]

// =============================================================
// Sprite lookup helper — call from gui.cpp instead of procedural
// drawing once you have real artwork:
//
//   const uint16_t* getSprite(PetState state, int animFrame) {
//     switch (state) {
//       case PET_THRIVING:
//         return SPRITE_THRIVING[animFrame % SPRITE_FRAMES_THRIVING];
//       case PET_HAPPY:
//         return SPRITE_HAPPY[animFrame % SPRITE_FRAMES_HAPPY];
//       case PET_TIRED:
//         return SPRITE_TIRED[animFrame % SPRITE_FRAMES_TIRED];
//       case PET_STRUGGLING:
//         return SPRITE_STRUGGLING[animFrame % SPRITE_FRAMES_STRUGGLING];
//       case PET_CRITICAL:
//         return SPRITE_CRITICAL[animFrame % SPRITE_FRAMES_CRITICAL];
//     }
//     return SPRITE_HAPPY[0];
//   }
//
// Then in gui.cpp drawPetFace():
//   const uint16_t* frame = getSprite(state, animFrame);
//   _canvas->draw16bitRGBBitmap(cx - 32, cy - 32, frame, 64, 64);
// =============================================================
