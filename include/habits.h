#pragma once
// =============================================================
// habits.h — Habit data structure and manager
//
// Responsibilities:
//   - Store and load habits from LittleFS (habits.json)
//   - Track daily progress and streaks
//   - Implement dynamic goal adaptation (core differentiator)
//   - Handle day rollover logic
// =============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

// -------------------------------------------------------------
// TodayRollup — aggregate progress for the current calendar day
// -------------------------------------------------------------
struct TodayRollup {
  int habitCount;       // habits aligned to `today` (lastLogDate match)
  int goalsMet;         // habits with completedToday >= goalToday
  int withAnyProgress;  // completedToday > 0
  int sumDone;          // sum of completedToday (meaningful units)
  int sumGoal;          // sum of goalToday
  // 0–100: sumDone*100/sumGoal, or 100 if all goals met with sumGoal==0 edge
  int dayPercent() const;
};

// -------------------------------------------------------------
// Habit — one trackable daily behaviour
// -------------------------------------------------------------
struct Habit {
  String name;            // e.g. "Hydrate"
  String emoji;           // e.g. "💧" (stored as UTF-8 string)
  uint16_t color;         // RGB565 theme color for UI cards
  int goalToday;          // Dynamic daily target (adapts each day)
  int completedToday;     // Units completed so far today
  int streak;             // Consecutive days goal was fully met
  String lastLogDate;     // ISO "YYYY-MM-DD" — used for streak tracking
  int minGoal;            // Hard floor: goal will never drop below this
  int maxGoal;            // Hard ceiling: goal will never exceed this
  String unit;            // Display unit, e.g. "glasses", "min", "steps"
};

// -------------------------------------------------------------
// HabitManager — owns the array of habits and persistence
// -------------------------------------------------------------
class HabitManager {
public:
  // Initialise: loads from LittleFS or seeds defaults if missing
  void begin();

  // Persist current habit state to /habits.json
  void save();

  // Reload from /habits.json (call after OTA or manual edit)
  bool load();

  // Log `amount` units of progress for the habit at `index`.
  // Clamps completedToday at INT_MAX. Call save() separately.
  void logProgress(int index, int amount);

  // Check whether a day-rollover is needed for `todayDate`.
  // Returns true if lastLogDate of any habit != todayDate.
  bool needsRollover(const String& todayDate);

  // Walk the calendar from each habit's lastLogDate up to targetDate (exclusive
  // of gaps in one step — one calendar night per iteration). Fixes multi-day
  // power-off: each missed night is scored and goals adapt once per night.
  // Returns cumulative habit-miss count (same metric as legacy rolloverDay).
  int catchUpToDate(const String& targetDate);

  // Snapshot for idle HUD / pet mood (uses habits stamped for `today`).
  TodayRollup summarizeToday(const String& today) const;

  // Accessors
  Habit& getHabit(int index);
  int    getCount() const;

  // Add a new habit at runtime (up to MAX_HABIT_COUNT).
  // Returns false if already at capacity.
  bool addHabit(const Habit& h);

  // Seed Journal + Invisalign (once per day each).
  void seedDefaults();

  // True if loaded habits.json matches the current schema and habit set.
  bool isCurrentHabitSet() const;

private:
  Habit _habits[MAX_HABIT_COUNT];
  int   _count = 0;

  // Apply the adaptive goal algorithm to a single habit based on
  // what percentage of yesterday's goal was completed.
  void _adaptGoal(Habit& h);

  // Serialise one Habit struct into a JsonObject
  void _habitToJson(const Habit& h, JsonObject obj) const;

  // Deserialise a JsonObject into a Habit struct
  void _jsonToHabit(JsonObjectConst obj, Habit& h);
};
