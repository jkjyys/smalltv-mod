// WeatherMode.h — current conditions + today's high/low, one screen.
//
// Owns its fetch (WeatherClient), its rendering, and its dirty state. Mirrors
// TickerMode/RadarMode's DisplayMode shape (see Mode.h).
#pragma once
#include "Mode.h"
#include "config.h"

class WeatherMode : public DisplayMode {
 public:
  const char* id() const override { return "weather"; }
  uint8_t     modeConst() const override { return MODE_WEATHER; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override {   // carousel switched back: repaint only
    needRender_ = true;
  }

 private:
  void render(const Settings& s);

  uint32_t renderedLastOk_ = 0xFFFFFFFF;
  bool     renderedError_  = false;
  int8_t   renderedMin_    = -1;   // last clock minute we painted, so the face ticks over
  bool     needRender_     = true;
};

extern WeatherMode g_weatherMode;
