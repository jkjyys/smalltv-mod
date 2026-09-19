// ClockMode.h — full-screen digital clock: weekday, date and a big HH:MM.
//
// A dedicated "desk clock" face for the carousel. It fetches nothing of its
// own — the wall clock is already kept by Clock.cpp (SNTP) at the src root —
// this mode is purely a renderer on top of it. Repaints once per minute (the
// display doesn't need to move any faster than the clock itself does), same
// dirty-check shape as WeatherMode's clock row.
#pragma once
#include "Mode.h"
#include "config.h"

class ClockMode : public DisplayMode {
 public:
  const char* id() const override { return "clock"; }
  uint8_t     modeConst() const override { return MODE_CLOCK; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override {   // carousel switched back: repaint only
    needRender_ = true;
  }

 private:
  void render(const Settings& s);

  int8_t renderedMin_     = -1;      // last minute painted, so the face ticks over
  bool   renderedSynced_  = false;   // last "do we trust the clock" state painted
  bool   needRender_      = true;
};

extern ClockMode g_clockMode;
