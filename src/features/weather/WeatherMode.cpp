#include "WeatherMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Net.h"
#include "Clock.h"
#include "WeatherClient.h"

WeatherMode g_weatherMode;

// Weather palette — a blue accent for the humidity gauge, otherwise the
// shared C_* constants from Gfx.h (all through gfxTint(), so the Display
// tab's colour correction reaches this mode too).
#define C_BARBG   gfxTint(0x2945)   // unfilled gauge track
#define C_HUMID   gfxTint(0x3D7F)   // humidity fill, cyan-ish blue
#define C_DATEC   gfxTint(0x2645)   // date row / place label, muted green

// ---- WMO weather_code -> icon bucket ---------------------------------------
// https://open-meteo.com/en/docs — grouped into the handful of icons we draw.
enum WIcon { WI_CLEAR, WI_CLOUD, WI_FOG, WI_RAIN, WI_SNOW, WI_STORM };

static WIcon bucketOf(int16_t code) {
  if (code == 0) return WI_CLEAR;
  if (code == 1 || code == 2 || code == 3) return WI_CLOUD;
  if (code == 45 || code == 48) return WI_FOG;
  if (code >= 51 && code <= 67) return WI_RAIN;
  if (code >= 80 && code <= 82) return WI_RAIN;
  if (code >= 71 && code <= 77) return WI_SNOW;
  if (code == 95 || code == 96 || code == 99) return WI_STORM;
  return WI_CLOUD;
}

static const char* labelOf(WIcon b) {
  switch (b) {
    case WI_CLEAR: return "Clear";
    case WI_CLOUD: return "Cloudy";
    case WI_FOG:   return "Fog";
    case WI_RAIN:  return "Rain";
    case WI_SNOW:  return "Snow";
    case WI_STORM: return "Storm";
  }
  return "";
}

static const char* WDAY[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

// ---- icon drawing (vector shapes, no bitmap assets) ------------------------
static void drawSun(Arduino_GFX* gfx, int cx, int cy, int r, uint16_t c) {
  gfx->fillCircle(cx, cy, r, c);
  for (int i = 0; i < 8; i++) {
    float a = i * (float)PI / 4.0f;
    int x1 = cx + (int)((r + 5) * cosf(a));
    int y1 = cy + (int)((r + 5) * sinf(a));
    int x2 = cx + (int)((r + 12) * cosf(a));
    int y2 = cy + (int)((r + 12) * sinf(a));
    gfx->drawLine(x1, y1, x2, y2, c);
  }
}

static void drawMoon(Arduino_GFX* gfx, int cx, int cy, int r, uint16_t c) {
  gfx->fillCircle(cx, cy, r, c);
  gfx->fillCircle(cx + r / 2, cy - r / 3, r, C_BLACK);   // bite out a crescent
}

static void drawCloud(Arduino_GFX* gfx, int cx, int cy, int r, uint16_t c) {
  gfx->fillCircle(cx - r, cy + r / 3, r, c);
  gfx->fillCircle(cx + r / 2, cy - r / 4, (int)(r * 1.15f), c);
  gfx->fillCircle(cx + r * 3 / 2, cy + r / 3, r, c);
  gfx->fillRoundRect(cx - r - 2, cy + r / 3, r * 3, r, r / 2, c);
}

// Compact icon variant, sized down for the condition-row slot next to the
// big clock — same shapes as before, just half the radius.
static void drawIconSmall(Arduino_GFX* gfx, WIcon b, bool isDay, int cx, int cy) {
  switch (b) {
    case WI_CLEAR:
      if (isDay) drawSun(gfx, cx, cy, 13, C_YELLOW);
      else       drawMoon(gfx, cx, cy, 12, C_GRAY);
      break;
    case WI_CLOUD:
      if (isDay) drawSun(gfx, cx - 8, cy - 4, 7, C_YELLOW);
      drawCloud(gfx, cx + 3, cy + 3, 9, C_GRAY);
      break;
    case WI_FOG:
      for (int i = 0; i < 3; i++) gfx->drawFastHLine(cx - 16, cy - 8 + i * 8, 32, C_GRAY);
      break;
    case WI_RAIN:
      drawCloud(gfx, cx, cy - 6, 9, C_GRAY);
      for (int i = -1; i <= 1; i++)
        gfx->drawLine(cx + i * 8, cy + 10, cx + i * 8 - 3, cy + 18, C_BLUE);
      break;
    case WI_SNOW:
      drawCloud(gfx, cx, cy - 6, 9, C_GRAY);
      for (int i = -1; i <= 1; i++) gfx->fillCircle(cx + i * 8, cy + 14, 2, C_WHITE);
      break;
    case WI_STORM:
      drawCloud(gfx, cx, cy - 6, 9, C_DGRAY);
      gfx->fillTriangle(cx + 3, cy + 7, cx - 4, cy + 17, cx + 2, cy + 15, C_YELLOW);
      break;
  }
}

// A rounded horizontal gauge, Usage-mode style: track + proportional fill.
static void drawGauge(Arduino_GFX* gfx, int x, int y, int w, int h, float pct, uint16_t fillC) {
  gfx->fillRoundRect(x, y, w, h, h / 2, C_BARBG);
  int fw = (int)(w * constrain(pct, 0.0f, 100.0f) / 100.0f);
  if (fw >= h)     gfx->fillRoundRect(x, y, fw, h, h / 2, fillC);
  else if (fw > 0) gfx->fillRect(x, y, fw, h, fillC);
}

// ---- rendering --------------------------------------------------------------
static void renderPrompt(const Settings& s) {
  gfxMessage("Set location", netIP().c_str(), C_YELLOW);
  (void)s;
}

void WeatherMode::render(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  if (s.weather.lat == 0.0f && s.weather.lon == 0.0f) { renderPrompt(s); return; }

  const WeatherNow& w = weatherCurrent();
  gfx->fillScreen(C_BLACK);

  if (!w.valid) {
    gfxDrawCentered("Weather", 80, 3, C_WHITE);
    gfxDrawCentered(w.error ? "fetch error" : "loading...", 120, 2, C_GRAY);
    return;
  }

  const int PAD = 10;
  int y = 8;

  // Row 1: place name, left-aligned (no country badge — we don't have one to show honestly)
  if (s.weather.place.length()) {
    gfx->setTextSize(2);
    gfx->setTextColor(C_DATEC);
    gfx->setCursor(PAD, y);
    gfx->print(s.weather.place);
  }
  y += 22;

  // Row 2: big clock, only while NTP is actually trusted — showing a wrong
  // or frozen time would be worse than not showing one at all.
  struct tm tmNow;
  bool haveClock = clockTrusted() && clockNow(tmNow);
  WIcon b = bucketOf(w.code);
  if (haveClock) {
    char tbuf[6];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
    gfx->setTextSize(6);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(PAD, y);
    gfx->print(tbuf);

    // Compact condition icon + label, right-aligned next to the clock.
    drawIconSmall(gfx, b, w.isDay, TFT_WIDTH - 44, y + 20);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GRAY);
    int lw = gfxTextW(labelOf(b), 1);
    gfx->setCursor(TFT_WIDTH - 10 - lw, y + 40);
    gfx->print(labelOf(b));
    y += 54;

    // Row 3: date + weekday
    char dbuf[24];
    snprintf(dbuf, sizeof(dbuf), "%d/%d/%d  %s",
             tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_year + 1900, WDAY[tmNow.tm_wday]);
    gfx->setTextSize(2);
    gfx->setTextColor(C_DATEC);
    gfx->setCursor(PAD, y);
    gfx->print(dbuf);
    y += 30;
  } else {
    // No trusted clock yet: fall back to icon + condition centered, same as
    // before this layout grew a clock.
    int iconCy = y + 24;
    drawIconSmall(gfx, b, w.isDay, TFT_WIDTH / 2 - 30, iconCy);
    gfx->setTextSize(2);
    gfx->setTextColor(C_GRAY);
    gfx->setCursor(TFT_WIDTH / 2 - 10, iconCy - 6);
    gfx->print(labelOf(b));
    y = iconCy + 40;
  }

  // Row: big current temperature.
  char tbuf2[16];
  snprintf(tbuf2, sizeof(tbuf2), "%.0f%s", w.temp, s.weather.fahrenheit ? "F" : "C");
  uint8_t tsz = gfxFitSize(tbuf2, TFT_WIDTH - 2 * PAD, 5);
  gfx->setTextSize(tsz);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(PAD, y);
  gfx->print(tbuf2);
  y += 8 * tsz + 10;

  // Row: today's high/low, plain text (no gauge — a min/max temp has no
  // natural 0-100% scale the way a usage % or humidity % does).
  {
    char hl[32];
    snprintf(hl, sizeof(hl), "H:%.0f%s  L:%.0f%s", w.hi, s.weather.fahrenheit ? "F" : "C",
             w.lo, s.weather.fahrenheit ? "F" : "C");
    gfx->setTextSize(2);
    gfx->setTextColor(C_GRAY);
    gfx->setCursor(PAD, y);
    gfx->print(hl);
    y += 28;
  }

  // Row: humidity gauge.
  {
    char lbl[8]; snprintf(lbl, sizeof(lbl), "%.0f%%", w.humidity);
    gfx->setTextSize(2);
    gfx->setTextColor(C_GRAY);
    gfx->setCursor(PAD, y);
    gfx->print("Humidity");
    gfx->setCursor(TFT_WIDTH - PAD - gfxTextW(lbl, 2), y);
    gfx->print(lbl);
    y += 22;
    drawGauge(gfx, PAD, y, TFT_WIDTH - 2 * PAD, 12, w.humidity, C_HUMID);
  }

  // Updated-ago (bottom-left), same convention as the ticker.
  if (w.lastOkMs) {
    uint32_t ago = (millis() - w.lastOkMs) / 1000;
    char buf[12];
    if (ago < 100) snprintf(buf, sizeof(buf), "%lus", (unsigned long)ago);
    else           snprintf(buf, sizeof(buf), "%lum", (unsigned long)(ago / 60));
    gfx->setTextSize(1);
    gfx->setTextColor(w.error ? C_RED : C_DGRAY);
    gfx->setCursor(4, 232);
    gfx->print(buf);
  }
  if (w.error) gfx->fillCircle(6, 6, 3, C_RED);
}

// ---- DisplayMode ------------------------------------------------------------
void WeatherMode::begin(const Settings& s) {
  weatherInit(s);
  renderedLastOk_ = 0xFFFFFFFF;
  renderedError_ = false;
  renderedMin_ = -1;
  needRender_ = true;
}

void WeatherMode::invalidate(const Settings& s) {
  weatherInit(s);
  weatherForceRefresh();
  renderedLastOk_ = 0xFFFFFFFF;
  needRender_ = true;
}

void WeatherMode::service(const Settings& s) {
  weatherService(s);

  const WeatherNow& w = weatherCurrent();
  if (w.lastOkMs != renderedLastOk_ || w.error != renderedError_) {
    needRender_ = true;
    renderedLastOk_ = w.lastOkMs;
    renderedError_ = w.error;
  }

  // The clock face needs a repaint every minute even if the weather data
  // itself hasn't changed — otherwise it just sits frozen between polls.
  struct tm tmNow;
  if (clockTrusted() && clockNow(tmNow) && tmNow.tm_min != renderedMin_) {
    renderedMin_ = tmNow.tm_min;
    needRender_ = true;
  }

  if (needRender_) {
    render(s);
    needRender_ = false;
  }
}
