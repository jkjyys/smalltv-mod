#include "WeatherMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Net.h"
#include "WeatherClient.h"

WeatherMode g_weatherMode;

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

static void drawIcon(Arduino_GFX* gfx, WIcon b, bool isDay, int cx, int cy, uint16_t trendC) {
  switch (b) {
    case WI_CLEAR:
      if (isDay) drawSun(gfx, cx, cy, 26, C_YELLOW);
      else       drawMoon(gfx, cx, cy, 24, C_GRAY);
      break;
    case WI_CLOUD:
      if (isDay) drawSun(gfx, cx - 16, cy - 8, 14, C_YELLOW);
      drawCloud(gfx, cx + 6, cy + 6, 18, C_GRAY);
      break;
    case WI_FOG:
      for (int i = 0; i < 4; i++) {
        int y = cy - 18 + i * 12;
        gfx->drawFastHLine(cx - 34, y, 68, C_GRAY);
        gfx->drawFastHLine(cx - 34, y + 1, 68, C_GRAY);
      }
      break;
    case WI_RAIN:
      drawCloud(gfx, cx, cy - 12, 18, C_GRAY);
      for (int i = -1; i <= 1; i++)
        gfx->drawLine(cx + i * 16, cy + 20, cx + i * 16 - 6, cy + 36, C_BLUE);
      break;
    case WI_SNOW:
      drawCloud(gfx, cx, cy - 12, 18, C_GRAY);
      for (int i = -1; i <= 1; i++)
        gfx->fillCircle(cx + i * 16, cy + 28, 3, C_WHITE);
      break;
    case WI_STORM:
      drawCloud(gfx, cx, cy - 12, 18, C_DGRAY);
      gfx->fillTriangle(cx + 6, cy + 14, cx - 8, cy + 34, cx + 4, cy + 30, C_YELLOW);
      gfx->fillTriangle(cx + 4, cy + 30, cx + 14, cy + 30, cx - 2, cy + 46, C_YELLOW);
      break;
  }
  (void)trendC;
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

  int y = 8;
  if (s.weather.place.length()) {
    uint8_t sz = gfxFitSize(s.weather.place.c_str(), 220, 2);
    gfxDrawCentered(s.weather.place.c_str(), y, sz, C_GRAY);
    y += 8 * sz + 4;
  }

  WIcon b = bucketOf(w.code);
  int iconCy = y + 30;
  drawIcon(gfx, b, w.isDay, TFT_WIDTH / 2, iconCy, C_WHITE);
  y = iconCy + 46;

  char tbuf[16];
  snprintf(tbuf, sizeof(tbuf), "%.0f%s", w.temp, s.weather.fahrenheit ? "F" : "C");
  uint8_t tsz = gfxFitSize(tbuf, 200, 6);
  gfxDrawCentered(tbuf, y, tsz, C_WHITE);
  y += 8 * tsz + 6;

  gfxDrawCentered(labelOf(b), y, 2, C_GRAY);
  y += 24;

  char hl[32];
  snprintf(hl, sizeof(hl), "H:%.0f  L:%.0f", w.hi, w.lo);
  gfxDrawCentered(hl, y, 2, C_WHITE);

  // Updated-ago (bottom-left), same convention as the ticker.
  if (w.lastOkMs) {
    uint32_t ago = (millis() - w.lastOkMs) / 1000;
    char buf[12];
    if (ago < 100) snprintf(buf, sizeof(buf), "%lus", (unsigned long)ago);
    else           snprintf(buf, sizeof(buf), "%lum", (unsigned long)(ago / 60));
    gfx->setTextSize(2);
    gfx->setTextColor(w.error ? C_RED : C_DGRAY);
    gfx->setCursor(4, 224);
    gfx->print(buf);
  }
  if (w.error) gfx->fillCircle(6, 6, 3, C_RED);
}

// ---- DisplayMode ------------------------------------------------------------
void WeatherMode::begin(const Settings& s) {
  weatherInit(s);
  renderedLastOk_ = 0xFFFFFFFF;
  renderedError_ = false;
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

  if (needRender_) {
    render(s);
    needRender_ = false;
  }
}
