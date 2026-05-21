// =============================================================
// habits.cpp — Habit management, dynamic goals, and persistence
//
// Dynamic goal adaptation is the primary differentiator of this
// app. At day rollover, each habit's goalToday for tomorrow is
// adjusted based on how much of yesterday's goal was completed:
//
//   ≥ 100%  → goal increases by GOAL_STEP_AMOUNT (up to maxGoal)
//   50–99%  → goal stays the same
//   < 50%   → goal decreases by GOAL_STEP_AMOUNT (down to minGoal)
//
// The goal meets the user where they are, preventing burnout
// while gently pushing achievers forward.
// =============================================================

#include "habits.h"
#include "storage.h"
#include "rtc.h"
#include <Arduino.h>

// External storage instance (defined in main.cpp)
extern Storage storage;

// ---------------------------------------------------------------
// TodayRollup::dayPercent
// ---------------------------------------------------------------
int TodayRollup::dayPercent() const {
  if (habitCount <= 0) return 0;
  if (goalsMet >= habitCount) return 100;
  if (sumGoal <= 0) return (goalsMet > 0) ? 100 : 0;
  int p = (sumDone * 100) / sumGoal;
  return constrain(p, 0, 100);
}

namespace {

void habit_adapt_goal(Habit& h) {
  if (h.goalToday <= 0) return;
  const int pct = (h.completedToday * 100) / h.goalToday;
  if (pct >= GOAL_INCREASE_THRESHOLD) {
    h.goalToday = min(h.goalToday + GOAL_STEP_AMOUNT, h.maxGoal);
    Serial.printf("[Habits] %s goal increased to %d %s\n",
                  h.name.c_str(), h.goalToday, h.unit.c_str());
  } else if (pct < GOAL_STAY_THRESHOLD) {
    h.goalToday = max(h.goalToday - GOAL_STEP_AMOUNT, h.minGoal);
    Serial.printf("[Habits] %s goal decreased to %d %s (only %d%% yesterday)\n",
                  h.name.c_str(), h.goalToday, h.unit.c_str(), pct);
  }
}

// Close the day currently stamped on habits, then open `openingDate`.
int habit_close_frontier(Habit* habits, int count, const String& openingDate) {
  int missedCount = 0;
  for (int i = 0; i < count; i++) {
    Habit& h = habits[i];
    if (h.lastLogDate == openingDate) continue;

    const bool wasTracked = (h.lastLogDate.length() > 0);
    if (wasTracked) {
      const bool metGoal = (h.completedToday >= h.goalToday);
      if (metGoal) {
        h.streak++;
      } else {
        h.streak = 0;
        missedCount++;
      }
      habit_adapt_goal(h);
    }
    h.completedToday = 0;
    h.lastLogDate    = openingDate;
  }
  return missedCount;
}

}  // namespace

// ---------------------------------------------------------------
// begin — load from storage or seed defaults on first boot
// ---------------------------------------------------------------
void HabitManager::begin() {
  if (!load()) {
    Serial.println("[Habits] No habits.json found — seeding defaults");
    seedDefaults();
    save();
  }
  Serial.printf("[Habits] Loaded %d habits\n", _count);
}

// ---------------------------------------------------------------
// seedDefaults — populate the 5 default habits
// ---------------------------------------------------------------
void HabitManager::seedDefaults() {
  _count = 0;

  Habit hydrate;
  hydrate.name          = "Hydrate";
  hydrate.emoji         = "W";   // Water drop (UTF-8 may not render on basic font)
  hydrate.color         = 0x065F; // Cyan-blue
  hydrate.goalToday     = 8;
  hydrate.completedToday= 0;
  hydrate.streak        = 0;
  hydrate.lastLogDate   = "";
  hydrate.minGoal       = 4;
  hydrate.maxGoal       = 14;
  hydrate.unit          = "glasses";
  _habits[_count++] = hydrate;

  Habit read;
  read.name          = "Read";
  read.emoji         = "R";
  read.color         = 0xF9A0; // Warm amber
  read.goalToday     = 20;
  read.completedToday= 0;
  read.streak        = 0;
  read.lastLogDate   = "";
  read.minGoal       = 5;
  read.maxGoal       = 60;
  read.unit          = "min";
  _habits[_count++] = read;

  Habit meditate;
  meditate.name          = "Meditate";
  meditate.emoji         = "M";
  meditate.color         = 0x881F; // Purple
  meditate.goalToday     = 10;
  meditate.completedToday= 0;
  meditate.streak        = 0;
  meditate.lastLogDate   = "";
  meditate.minGoal       = 5;
  meditate.maxGoal       = 30;
  meditate.unit          = "min";
  _habits[_count++] = meditate;

  Habit move;
  move.name          = "Move";
  move.emoji         = "V";
  move.color         = 0x07E0; // Green
  move.goalToday     = 30;
  move.completedToday= 0;
  move.streak        = 0;
  move.lastLogDate   = "";
  move.minGoal       = 10;
  move.maxGoal       = 90;
  move.unit          = "min";
  _habits[_count++] = move;

  Habit sleep;
  sleep.name          = "Sleep Early";
  sleep.emoji         = "Z";
  sleep.color         = 0x4A1F; // Indigo
  sleep.goalToday     = 1;      // Binary: 1 = got to bed on time
  sleep.completedToday= 0;
  sleep.streak        = 0;
  sleep.lastLogDate   = "";
  sleep.minGoal       = 1;
  sleep.maxGoal       = 1;      // Binary habit — goal is always 1
  sleep.unit          = "night";
  _habits[_count++] = sleep;
}

// ---------------------------------------------------------------
// save — serialise all habits to /habits.json
// ---------------------------------------------------------------
void HabitManager::save() {
  JsonDocument doc;
  JsonArray arr = doc["habits"].to<JsonArray>();

  for (int i = 0; i < _count; i++) {
    JsonObject obj = arr.add<JsonObject>();
    _habitToJson(_habits[i], obj);
  }

  if (!storage.writeJSON(PATH_HABITS_JSON, doc)) {
    Serial.println("[Habits] ERROR: Failed to save habits.json");
  }
}

// ---------------------------------------------------------------
// load — deserialise habits from /habits.json
// ---------------------------------------------------------------
bool HabitManager::load() {
  JsonDocument doc;
  if (!storage.readJSON(PATH_HABITS_JSON, doc)) return false;

  JsonArray arr = doc["habits"].as<JsonArray>();
  if (!arr) return false;

  _count = 0;
  for (JsonObject obj : arr) {
    if (_count >= MAX_HABIT_COUNT) break;
    _jsonToHabit(obj, _habits[_count]);
    _count++;
  }

  return (_count > 0);
}

// ---------------------------------------------------------------
// logProgress — add `amount` units to habit at `index`
// ---------------------------------------------------------------
void HabitManager::logProgress(int index, int amount) {
  if (index < 0 || index >= _count) return;
  Habit& h = _habits[index];

  h.completedToday += amount;
  // Clamp to avoid integer overflow on excessive tapping
  if (h.completedToday > 9999) h.completedToday = 9999;

  Serial.printf("[Habits] %s: %d/%d %s\n",
                h.name.c_str(), h.completedToday, h.goalToday, h.unit.c_str());
}

// ---------------------------------------------------------------
// needsRollover — check if any habit's date is behind today
// ---------------------------------------------------------------
bool HabitManager::needsRollover(const String& todayDate) {
  for (int i = 0; i < _count; i++) {
    if (_habits[i].lastLogDate != todayDate) return true;
  }
  return false;
}

// ---------------------------------------------------------------
// summarizeToday — aggregate progress for habits stamped `today`
// ---------------------------------------------------------------
TodayRollup HabitManager::summarizeToday(const String& today) const {
  TodayRollup r = {};
  for (int i = 0; i < _count; i++) {
    const Habit& h = _habits[i];
    if (h.lastLogDate != today) continue;
    r.habitCount++;
    if (h.goalToday > 0) {
      r.sumDone += h.completedToday;
      r.sumGoal += h.goalToday;
    }
    if (h.goalToday > 0 && h.completedToday >= h.goalToday) r.goalsMet++;
    if (h.completedToday > 0) r.withAnyProgress++;
  }
  return r;
}

// ---------------------------------------------------------------
// catchUpToDate — consecutive calendar nights until all == targetDate
// ---------------------------------------------------------------
int HabitManager::catchUpToDate(const String& targetDate) {
  if (_count <= 0 || targetDate.length() < 10) return 0;

  int totalMissed = 0;

  String frontier;
  for (int i = 0; i < _count; i++) {
    const String& d = _habits[i].lastLogDate;
    if (d.length() == 0) continue;
    if (frontier.length() == 0 || d < frontier) frontier = d;
  }

  int guard = 0;
  while (guard++ < 4000) {
    bool allAtTarget = true;
    for (int i = 0; i < _count; i++) {
      if (_habits[i].lastLogDate != targetDate) {
        allAtTarget = false;
        break;
      }
    }
    if (allAtTarget) break;

    if (frontier.length() == 0) {
      for (int i = 0; i < _count; i++) {
        _habits[i].lastLogDate    = targetDate;
        _habits[i].completedToday = 0;
      }
      break;
    }

    if (RTC::compareIsoDate(frontier, targetDate) > 0) {
      for (int i = 0; i < _count; i++) {
        _habits[i].lastLogDate    = targetDate;
        _habits[i].completedToday = 0;
      }
      break;
    }

    if (frontier == targetDate) break;

    const String opening = RTC::dateAddDays(frontier, 1);
    if (opening.length() == 0) {
      Serial.println("[Habits] catchUpToDate: dateAddDays failed");
      break;
    }

    totalMissed += habit_close_frontier(_habits, _count, opening);
    frontier = opening;
  }

  Serial.printf("[Habits] Calendar catch-up → %s missed-nights: %d\n",
                targetDate.c_str(), totalMissed);
  return totalMissed;
}

// ---------------------------------------------------------------
// _adaptGoal — adjust goalToday based on yesterday's completion
// ---------------------------------------------------------------
void HabitManager::_adaptGoal(Habit& h) {
  habit_adapt_goal(h);
}

// ---------------------------------------------------------------
// addHabit — add a new habit at runtime
// ---------------------------------------------------------------
bool HabitManager::addHabit(const Habit& h) {
  if (_count >= MAX_HABIT_COUNT) {
    Serial.println("[Habits] ERROR: Max habit count reached");
    return false;
  }
  _habits[_count++] = h;
  return true;
}

// ---------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------
Habit& HabitManager::getHabit(int index) {
  // Clamp to avoid out-of-bounds on stale indices
  if (index < 0) index = 0;
  if (index >= _count) index = _count - 1;
  return _habits[index];
}

int HabitManager::getCount() const { return _count; }

// ---------------------------------------------------------------
// _habitToJson — serialise one Habit to a JsonObject
// ---------------------------------------------------------------
void HabitManager::_habitToJson(const Habit& h, JsonObject obj) const {
  obj["name"]           = h.name;
  obj["emoji"]          = h.emoji;
  obj["color"]          = h.color;
  obj["goalToday"]      = h.goalToday;
  obj["completedToday"] = h.completedToday;
  obj["streak"]         = h.streak;
  obj["lastLogDate"]    = h.lastLogDate;
  obj["minGoal"]        = h.minGoal;
  obj["maxGoal"]        = h.maxGoal;
  obj["unit"]           = h.unit;
}

// ---------------------------------------------------------------
// _jsonToHabit — deserialise a JsonObject into a Habit struct
// ---------------------------------------------------------------
void HabitManager::_jsonToHabit(JsonObjectConst obj, Habit& h) {
  h.name           = obj["name"]          | "Habit";
  h.emoji          = obj["emoji"]         | "?";
  h.color          = obj["color"]         | (uint16_t)COLOR_WHITE;
  h.goalToday      = obj["goalToday"]     | 1;
  h.completedToday = obj["completedToday"]| 0;
  h.streak         = obj["streak"]        | 0;
  h.lastLogDate    = obj["lastLogDate"]   | "";
  h.minGoal        = obj["minGoal"]       | 1;
  h.maxGoal        = obj["maxGoal"]       | 100;
  h.unit           = obj["unit"]          | "unit";
}
