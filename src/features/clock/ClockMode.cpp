#include "ClockMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Clock.h"

ClockMode g_clockMode;

static const char* WDAY[7] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

void ClockMode::render(const Settings& s) {
  (void)s;
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  struct tm tmNow;
  if (!(clockTrusted() && clockNow(tmNow))) {
    // No trusted NTP time yet (just booted, or WiFi/NTP unreachable) — say so
    // rather than show a wrong or frozen clock (same rule WeatherMode follows).
    gfxDrawCentered("Clock", 90, 3, C_WHITE);
    gfxDrawCentered("syncing...", 130, 2, C_GRAY);
    return;
  }

  int y = 26;
  gfxDrawCentered(WDAY[tmNow.tm_wday], y, 2, C_GRAY);
  y += 28;

  char dbuf[16];
  snprintf(dbuf, sizeof(dbuf), "%04d.%02d.%02d", tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
  gfxDrawCentered(dbuf, y, 2, C_WHITE);
  y += 34;

  // Big HH:MM fills most of what's left — the star of a desk-clock screen.
  // Centered in the space down to 236, not 240: the same bottom-row overscan
  // margin the other modes leave (see TickerMode's chart-bottom comment).
  char tbuf[6];
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
  uint8_t sz = gfxFitSize(tbuf, 228, 8);
  int th = 8 * sz;
  int ty = y + ((236 - y) - th) / 2;
  gfxDrawCentered(tbuf, ty, sz, C_WHITE);
}

// ---- DisplayMode ------------------------------------------------------------
void ClockMode::begin(const Settings& s) {
  (void)s;
  renderedMin_ = -1;
  renderedSynced_ = false;
  needRender_ = true;
}

void ClockMode::invalidate(const Settings& s) {
  (void)s;
  renderedMin_ = -1;
  needRender_ = true;
}

void ClockMode::service(const Settings& s) {
  struct tm tmNow;
  bool synced = clockTrusted() && clockNow(tmNow);

  if (synced != renderedSynced_) {
    renderedSynced_ = synced;
    needRender_ = true;
  }
  if (synced && tmNow.tm_min != renderedMin_) {
    renderedMin_ = tmNow.tm_min;
    needRender_ = true;
  }
  // Not yet synced: clockNow() is just a struct-copy read, so re-checking every
  // tick to catch the moment NTP lands is free — no separate retry timer needed.

  if (needRender_) {
    render(s);
    needRender_ = false;
  }
}
