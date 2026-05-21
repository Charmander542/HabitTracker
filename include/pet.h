#pragma once
// =============================================================
// pet.h — The living pet: vitality, emotional states, dialogue
//
// The pet is the soul of the device. Its vitality (0–100) drives
// its visual state and the messages it shows to the user. The
// goal is for the user to feel emotionally connected — the pet
// must feel alive, reactive, and slightly guilt-inducing when
// neglected (in a healthy, motivating way).
// =============================================================

#include <Arduino.h>
#include <time.h>
#include "config.h"

// -------------------------------------------------------------
// PetState — mapped to vitality score ranges
// -------------------------------------------------------------
enum PetState {
  PET_THRIVING,    // 80–100  Bouncing, sparkle eyes, big smile
  PET_HAPPY,       // 60–79   Gentle idle, neutral encouragement
  PET_TIRED,       // 40–59   Slow blink, droopy eyes, gentle reminders
  PET_STRUGGLING,  // 20–39   Shaky, worried face, pleading
  PET_CRITICAL     // 0–19    Flickering, ghostly, SOS haptic
};

// -------------------------------------------------------------
// DialogueContext — what triggered the message display
// -------------------------------------------------------------
enum DialogueContext {
  DIALOGUE_IDLE_MORNING,     // First look of the day
  DIALOGUE_IDLE_GENERAL,     // Ambient idle message
  DIALOGUE_HABIT_COMPLETE,   // User just logged a habit
  DIALOGUE_STREAK_MILESTONE, // Streak hit 3, 7, 14, 30 days
  DIALOGUE_MISSED_HABIT,     // Day rolled over with a miss
  DIALOGUE_CRITICAL          // Vitality in danger zone
};

// -------------------------------------------------------------
// Pet — manages vitality, state, animation frame, and dialogue
// -------------------------------------------------------------
class Pet {
public:
  // Load persisted state from LittleFS; call in setup()
  void begin();

  // Persist current state to /pet_config.json
  void save();

  // Add vitality, clamped to VITALITY_MAX
  void addVitality(int amount);

  // Subtract vitality, clamped to VITALITY_MIN
  void subtractVitality(int amount);

  // Forcefully set vitality (used during initial load)
  void setVitality(int value);

  int      getVitality() const;
  PetState getState()    const;

  // Advance the animation frame counter; call every ANIMATION_TICK_MS.
  // `now` = millis() value for smooth delta-based animation.
  void update(unsigned long now);

  // Apply time-based vitality loss (awake or sleep). `nowEpoch` = Unix seconds (RTC).
  void tickVitalityDecay(time_t nowEpoch);

  // On cold boot: apply capped decay for time spent powered off.
  void applyBootDecay(time_t nowEpoch);

  // Returns the current animation frame number (0–63 cycling).
  int getAnimFrame() const;

  // Set the context for the next dialogue pick; GUI reads it via getDialogue()
  void setLastDialogueContext(DialogueContext ctx);

  // Pick a random message for the current state + context.
  // Never returns the same message twice in a row.
  String getDialogue();

  // Specialist call: get a streak message for a specific streak count.
  // Returns empty string if no special message exists for that count.
  String getStreakDialogue(int streak);

private:
  int              _vitality   = VITALITY_START;
  time_t           _lastDecayEpoch = 0;
  int              _animFrame  = 0;
  unsigned long    _lastTick   = 0;
  DialogueContext  _lastCtx    = DIALOGUE_IDLE_GENERAL;
  int              _lastMsgIdx = -1;   // Prevents identical consecutive messages

  PetState _computeState() const;

  // Message pools per state — each pool has at least 4 entries.
  // Defined in pet.cpp to keep this header clean.
  static const char* _thrivingMsgs[];
  static const char* _happyMsgs[];
  static const char* _tiredMsgs[];
  static const char* _strugglingMsgs[];
  static const char* _criticalMsgs[];
  static const char* _morningMsgs[];
  static const char* _habitCompleteMsgs[];
  static const char* _missedMsgs[];

  static const int _thrivingCount;
  static const int _happyCount;
  static const int _tiredCount;
  static const int _strugglingCount;
  static const int _criticalCount;
  static const int _morningCount;
  static const int _habitCompleteCount;
  static const int _missedCount;

  // Pick a random index from [0, count) that is != _lastMsgIdx
  int _pickRandom(int count);
};
