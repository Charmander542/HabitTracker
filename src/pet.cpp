// =============================================================
// pet.cpp — Pet vitality, emotional states, and dialogue system
//
// DIALOGUE DESIGN NOTES:
//   - Each emotional state has a pool of ≥4 distinct messages.
//   - _pickRandom() ensures the same message is never shown twice
//     in a row, making the pet feel more alive and less scripted.
//   - Messages are intentionally warm and slightly guilt-inducing
//     in a healthy, motivating way — not punishing.
// =============================================================

#include "pet.h"
#include "storage.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

extern Storage storage;

// ---------------------------------------------------------------
// Dialogue pools — all messages are short enough to fit in the
// speech bubble on the round display (≤ ~40 chars)
// ---------------------------------------------------------------

const char* Pet::_thrivingMsgs[] = {
  "I feel amazing today! Let's go!",
  "You're my absolute hero! \x01",
  "We're unstoppable together!",
  "I could bounce forever right now!",
  "This is the best day ever!",
};
const int Pet::_thrivingCount = 5;

const char* Pet::_happyMsgs[] = {
  "Good work! Keep it up.",
  "I'm feeling good today! \x02",
  "You're doing great, one step at a time.",
  "I believe in you!",
  "Today is a good day.",
};
const int Pet::_happyCount = 5;

const char* Pet::_tiredMsgs[] = {
  "A little tired... but still here.",
  "Don't forget about me today?",
  "I could use a little boost...",
  "Just one habit. That's all I ask.",
  "I'm holding on. Are you?",
};
const int Pet::_tiredCount = 5;

const char* Pet::_strugglingMsgs[] = {
  "Please... I really need you.",
  "I'm not doing so well...",
  "One small step could save us both.",
  "I miss when we were thriving...",
  "I'm trying to stay positive.",
};
const int Pet::_strugglingCount = 5;

const char* Pet::_criticalMsgs[] = {
  "I'm fading... I need you now.",
  "Please don't give up on me...",
  "Just one habit. Save me.",
  "I can barely hold on...",
  "SOS. I'm almost gone.",
};
const int Pet::_criticalCount = 5;

const char* Pet::_morningMsgs[] = {
  "Good morning! Ready for today?",
  "Rise and shine! Let's do this.",
  "A brand new day! You've got this.",
  "Morning! I missed you.",
  "New day, new chances! Let's go!",
};
const int Pet::_morningCount = 5;

const char* Pet::_habitCompleteMsgs[] = {
  "Yay! I feel so much better!",
  "That's what I'm talking about!",
  "You did it! I'm so proud!",
  "Another one! You're on fire!",
  "Yes! Keep going, you're amazing!",
};
const int Pet::_habitCompleteCount = 5;

const char* Pet::_missedMsgs[] = {
  "I'm a little sad today...",
  "Please try again? I believe in you.",
  "It's okay. Tomorrow is a new day.",
  "I miss you when you're gone.",
  "Don't give up on us.",
};
const int Pet::_missedCount = 5;

// ---------------------------------------------------------------
// begin — load persisted vitality from /pet_config.json
// ---------------------------------------------------------------
void Pet::begin() {
  JsonDocument doc;
  if (storage.readJSON(PATH_PET_CONFIG, doc)) {
    _vitality = doc["vitality"] | VITALITY_START;
    // Clamp in case the file was hand-edited to an invalid value
    _vitality = constrain(_vitality, VITALITY_MIN, VITALITY_MAX);
    Serial.printf("[Pet] Loaded vitality: %d\n", _vitality);
  } else {
    _vitality = VITALITY_START;
    Serial.printf("[Pet] First boot — vitality set to %d\n", VITALITY_START);
    save();
  }
}

// ---------------------------------------------------------------
// save — persist vitality to /pet_config.json
// ---------------------------------------------------------------
void Pet::save() {
  JsonDocument doc;
  doc["vitality"] = _vitality;

  if (!storage.writeJSON(PATH_PET_CONFIG, doc)) {
    Serial.println("[Pet] ERROR: Failed to save pet_config.json");
  }
}

// ---------------------------------------------------------------
// addVitality — gain health, clamped to VITALITY_MAX
// ---------------------------------------------------------------
void Pet::addVitality(int amount) {
  _vitality = min(_vitality + amount, VITALITY_MAX);
  Serial.printf("[Pet] +%d vitality → %d\n", amount, _vitality);
}

// ---------------------------------------------------------------
// subtractVitality — lose health, clamped to VITALITY_MIN
// ---------------------------------------------------------------
void Pet::subtractVitality(int amount) {
  _vitality = max(_vitality - amount, VITALITY_MIN);
  Serial.printf("[Pet] -%d vitality → %d\n", amount, _vitality);
}

void Pet::setVitality(int value) {
  _vitality = constrain(value, VITALITY_MIN, VITALITY_MAX);
}

int      Pet::getVitality() const { return _vitality; }
PetState Pet::getState()    const { return _computeState(); }
int      Pet::getAnimFrame() const { return _animFrame; }

// ---------------------------------------------------------------
// update — advance animation frame counter
// ---------------------------------------------------------------
void Pet::update(unsigned long now) {
  if (now - _lastTick >= ANIMATION_TICK_MS) {
    _lastTick = now;
    _animFrame = (_animFrame + 1) % 64;   // 64-frame cycle at 50ms = ~3.2 sec loop
  }
}

// ---------------------------------------------------------------
// setLastDialogueContext — set what triggered the next message
// ---------------------------------------------------------------
void Pet::setLastDialogueContext(DialogueContext ctx) {
  _lastCtx = ctx;
}

// ---------------------------------------------------------------
// getDialogue — pick a context-appropriate, non-repeated message
// ---------------------------------------------------------------
String Pet::getDialogue() {
  PetState state = _computeState();

  switch (_lastCtx) {
    case DIALOGUE_IDLE_MORNING:
      return String(_morningMsgs[_pickRandom(_morningCount)]);

    case DIALOGUE_HABIT_COMPLETE:
      return String(_habitCompleteMsgs[_pickRandom(_habitCompleteCount)]);

    case DIALOGUE_MISSED_HABIT:
      return String(_missedMsgs[_pickRandom(_missedCount)]);

    case DIALOGUE_CRITICAL:
      return String(_criticalMsgs[_pickRandom(_criticalCount)]);

    case DIALOGUE_IDLE_GENERAL:
    default:
      // Pick from the state-appropriate pool
      switch (state) {
        case PET_THRIVING:    return String(_thrivingMsgs[_pickRandom(_thrivingCount)]);
        case PET_HAPPY:       return String(_happyMsgs[_pickRandom(_happyCount)]);
        case PET_TIRED:       return String(_tiredMsgs[_pickRandom(_tiredCount)]);
        case PET_STRUGGLING:  return String(_strugglingMsgs[_pickRandom(_strugglingCount)]);
        case PET_CRITICAL:    return String(_criticalMsgs[_pickRandom(_criticalCount)]);
        default:              return String(_happyMsgs[_pickRandom(_happyCount)]);
      }
  }
}

// ---------------------------------------------------------------
// getStreakDialogue — special milestone messages
// ---------------------------------------------------------------
String Pet::getStreakDialogue(int streak) {
  if (streak == 3)  return "3 days in a row!! You're my hero!";
  if (streak == 7)  return "A whole week! I love you so much!";
  if (streak == 14) return "Two weeks strong! Incredible!";
  if (streak == 30) return "30 DAYS! You're a legend!";
  return "";   // No special message for this streak count
}

// ---------------------------------------------------------------
// _computeState — map vitality score to PetState enum
// ---------------------------------------------------------------
PetState Pet::_computeState() const {
  if (_vitality >= VITALITY_THRIVING_MIN)   return PET_THRIVING;
  if (_vitality >= VITALITY_HAPPY_MIN)      return PET_HAPPY;
  if (_vitality >= VITALITY_TIRED_MIN)      return PET_TIRED;
  if (_vitality >= VITALITY_STRUGGLING_MIN) return PET_STRUGGLING;
  return PET_CRITICAL;
}

// ---------------------------------------------------------------
// _pickRandom — pick a random index != _lastMsgIdx
// ---------------------------------------------------------------
int Pet::_pickRandom(int count) {
  if (count <= 1) return 0;

  int idx;
  int attempts = 0;
  do {
    idx = random(0, count);
    attempts++;
  } while (idx == _lastMsgIdx && attempts < 10);

  _lastMsgIdx = idx;
  return idx;
}
