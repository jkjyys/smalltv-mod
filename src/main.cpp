// smalltv-mod — custom firmware for the GeekMagic SmallTV (ESP-12F / ESP8266)
//
// Each feature is a self-contained DisplayMode (see Mode.h), picked in the
// web UI and dispatched from the registry below:
//   - Ticker  (features/ticker):  stock/crypto price, % change, sparkline.
//   - Usage   (features/usage):   Claude 5h/7d usage bars + animated mascot.
//   - Radar   (features/radar):   live ADS-B plane radar (compiled in when WITH_RADAR).
//   - Weather (features/weather): current conditions + today's high/low.
//   - Clock   (features/clock):   full-screen digital clock (no network fetch).
// Shared plumbing (WiFi, web UI, OTA, display core, settings) lives at src root.
//
// License: WTFPL
#include <Arduino.h>
#include "Platform.h"
#include "config.h"
#include "Settings.h"
#include "Net.h"
#include "Gfx.h"
#include "WebPortal.h"
#include "OtaUpdate.h"
#include "CrashTrace.h"
#include "Mode.h"
#include "Clock.h"
#include "WgClient.h"
#include "NotifyMode.h"

#if WITH_TICKER
#include "TickerMode.h"
#endif
#if WITH_USAGE
#include "UsageMode.h"
#endif
#if WITH_RADAR
#include "RadarMode.h"
#endif
#if WITH_WEATHER
#include "WeatherMode.h"
#endif
#if WITH_CLOCK
#include "ClockMode.h"
#endif

// ---- mode registry --------------------------------------------------------
// The compiled-in features, in display order. main.cpp holds no per-feature
// state of its own — each mode owns its fetch/render/dirty tracking.
static DisplayMode* kModes[] = {
#if WITH_TICKER
  &g_tickerMode,
#endif
#if WITH_USAGE
  &g_usageMode,
#endif
#if WITH_RADAR
  &g_radarMode,
#endif
#if WITH_WEATHER
  &g_weatherMode,
#endif
#if WITH_CLOCK
  &g_clockMode,
#endif
};
static const size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

// ---- carousel -------------------------------------------------------------
// MODE_CAROUSEL rotates through the ticked features. Switches call wake() on
// the incoming mode: repaint from cached data, no refetch.
static size_t   g_carIdx = 0;
static uint32_t g_carSwitch = 0;

static bool carouselHas(const Settings& s, const DisplayMode* m) {
  switch (m->modeConst()) {
    case MODE_STOCKS: return s.carouselTicker;
    case MODE_USAGE:  return s.carouselUsage;
    case MODE_RADAR:  return s.carouselRadar;
    case MODE_WEATHER: return s.carouselWeather;
    case MODE_CLOCK:   return s.carouselClock;
    default:          return true;
  }
}

// Advance g_carIdx to the next ticked mode (stays put if none other is ticked).
static void carouselNext(const Settings& s) {
  for (size_t hop = 1; hop <= kModeCount; hop++) {
    size_t cand = (g_carIdx + hop) % kModeCount;
    if (!carouselHas(s, kModes[cand])) continue;
    if (cand != g_carIdx) {
      g_carIdx = cand;
      kModes[cand]->wake(s);
    }
    return;
  }
}

static DisplayMode* activeMode(const Settings& s) {
  if (s.mode == MODE_CAROUSEL && kModeCount > 0) {
    if (g_carSwitch == 0) g_carSwitch = millis();
    if (!carouselHas(s, kModes[g_carIdx])) carouselNext(s);   // settings changed
    if (millis() - g_carSwitch >= (uint32_t)s.carouselSec * 1000UL) {
      g_carSwitch = millis();
      carouselNext(s);
    }
    return kModes[g_carIdx];
  }
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i]->modeConst() == s.mode) return kModes[i];
  return kModeCount ? kModes[0] : nullptr;   // fall back to the first compiled mode
}

// ---- stack headroom per loop section (diagnostics) -------------------------
// The ESP8266 runs setup()/loop() on a fixed 4 KB stack. /api/status used to
// report only its all-time low-water mark ("contstk"), which sat within 32-256
// bytes of full in normal running -- close enough that an occasional overflow
// is a live suspect for the September 2026 crashes -- but not WHICH part of the
// loop takes it there. Each section below starts from a freshly repainted
// stack and keeps its own low-water mark, reported as /api/status "stk".
enum : uint8_t { STK_NET, STK_WEB, STK_CLOCK, STK_NOTIFY, STK_MODES };
static const size_t kStkSlots = STK_MODES + kModeCount;
static uint16_t g_stkMin[kStkSlots];

static inline void stkBegin() {
#if defined(SMALLTV_ESP8266)
  ESP.resetFreeContStack();
#endif
}
static inline void stkEnd(size_t slot) {
#if defined(SMALLTV_ESP8266)
  uint32_t f = ESP.getFreeContStack();
  if (slot < kStkSlots && f < g_stkMin[slot]) g_stkMin[slot] = (uint16_t)f;
#else
  (void)slot;
#endif
}

// Lowest free stack seen in any section (what "contstk" used to mean).
uint32_t appStackMin() {
#if defined(SMALLTV_ESP8266)
  uint32_t m = 0xFFFF;
  for (size_t i = 0; i < kStkSlots; i++) if (g_stkMin[i] < m) m = g_stkMin[i];
  return m;
#else
  return platformFreeContStack();
#endif
}

void appStackJson(JsonObject o) {
  static const char* const kNames[STK_MODES] = { "net", "web", "clock", "notify" };
  for (size_t i = 0; i < kStkSlots; i++) {
    if (g_stkMin[i] == 0xFFFF) continue;   // section hasn't run yet
    const char* name = (i < STK_MODES) ? kNames[i] : kModes[i - STK_MODES]->id();
    o[name] = g_stkMin[i];
  }
}

static Settings g_settings;
static String   g_resetReason;        // why the chip last reset (diagnostics)
static bool     g_safeMode = false;   // last reset was an exception -> don't re-enter the crash
static char     g_epcStr[16] = "";
static char     g_addrStr[16] = "";
static uint8_t  g_crashStreak = 0;    // crashes without a healthy run in between (CrashTrace)
static bool     g_streakCleared = false;
// Safe mode used to last until someone rebooted the device by hand, leaving the
// crash screen up (and every feature off) indefinitely after a single crash.
// Now it lasts SAFE_MODE_RECOVER_MS and then reboots normally -- unless crashes
// keep coming (SAFE_MODE_MAX_STREAK in a row, each within HEALTHY_RUN_MS of
// the last recovery), in which case it stays put so a crash loop can't hide.
static const uint32_t SAFE_MODE_RECOVER_MS  = 10UL * 60UL * 1000UL;
static const uint8_t  SAFE_MODE_MAX_STREAK  = 3;
static const uint32_t HEALTHY_RUN_MS        = 30UL * 60UL * 1000UL;
static int g_lastBr = -1;        // last effective brightness written (-1 = none yet)
#if HAS_LDR
static uint32_t g_lastAutoBr = 0;
static uint8_t  g_ldrCache   = DEFAULT_BRIGHTNESS;   // last LDR reading (2 s cadence)
#endif

// Single brightness resolver: night mode overrides auto-brightness overrides the
// manual level. Only writes the PWM when the effective target changes.
static uint8_t appEffectiveBrightness() {
  if (clockNightActive()) return g_settings.clock.nightLevel;
#if HAS_LDR
  if (g_settings.autoBrightness) {
    if (millis() - g_lastAutoBr > 2000) {
      g_lastAutoBr = millis();
      int raw = analogRead(LDR_PIN);
      g_ldrCache = (uint8_t)constrain(raw * 100 / ADC_MAX, 5, 100);
    }
    return g_ldrCache;
  }
#endif
  return g_settings.brightness;
}

void appApplyBrightness() {
  uint8_t t = appEffectiveBrightness();
  if ((int)t != g_lastBr) {
    g_lastBr = t;
    gfxSetBrightness(t, g_settings.backlightInverted);
  }
}

// Exposed to the web portal (/api/status) so the last reset reason is visible.
const char* appResetReason() { return g_resetReason.c_str(); }

// Called by the web portal after settings are applied: re-init every mode and
// force a fresh repaint so a mode/URL/symbol change takes effect immediately.
void appInvalidate() {
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->invalidate(g_settings);
}

static void bootProgress(const char* msg) {
  gfxBoot("SmallTV", msg);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(FW_NAME " " FW_VERSION);

  // Capture why we (re)booted. On a reboot loop this is the key clue, and the
  // device's UART isn't exposed — so we also show it on screen below. On the
  // ESP8266 we also keep the crash PC (epc1) for addr2line decoding; the
  // ESP32-C2 (RISC-V) doesn't expose it, so epc/addr come back empty there.
  PlatformReset pr = platformResetInfo();
  Serial.print("[boot] reset reason: ");
  Serial.println(pr.reason);

  if (pr.wasCrash) {
    g_safeMode = true;                   // crashed last boot -> stay out of the crash path
    strlcpy(g_epcStr,  pr.epc,  sizeof(g_epcStr));
    strlcpy(g_addrStr, pr.addr, sizeof(g_addrStr));
    char rich[80];
    snprintf(rich, sizeof(rich), "%s epc %s addr %s", pr.reason.c_str(),
             g_epcStr[0] ? g_epcStr : "-", g_addrStr[0] ? g_addrStr : "-");
    g_resetReason = rich;
  } else {
    g_resetReason = pr.reason;
  }

  Serial.println("[boot] settings");
  settingsBegin();
  // Needs LittleFS (just mounted) and must run before webPortalBegin() picks
  // up the last update's message: if the previous boot's GitHub update was
  // cut off by a reset, this names the step it died at (see OtaUpdate.h).
  otaReportInterrupted(g_resetReason.c_str());
  crashTraceBoot();                           // log the last crash's details, if any
  g_crashStreak = crashStreakOnBoot(pr.wasCrash);
  for (size_t i = 0; i < kStkSlots; i++) g_stkMin[i] = 0xFFFF;
  loadSettings(g_settings);

  Serial.println("[boot] display");
  gfxBegin(g_settings);
  gfxBoot(g_safeMode ? "Crashed" : "SmallTV", FW_VERSION);

  // Checked once, up front: the mDNS and SNTP arming below both get skipped
  // (and run later instead) exactly when a GitHub update is queued for this
  // boot -- see the comment above the OTA block for why.
  bool otaPending = otaBootRequested();

  Serial.println("[boot] net");
  netBegin(g_settings, bootProgress, otaPending);
  // Arm SNTP now that WiFi (STA) is up — but only if night mode is enabled, so a
  // ticker-only device doesn't pay the SNTP heap cost (which can starve the cash.ch
  // TLS handshake on the ESP8266). clockReapply arms it iff needed. Skipped after a
  // crash so a fault in here can't boot-loop before the web server starts (the
  // device then comes up in safe mode, OTA-recoverable, instead of needing UART).
  // ...unless a WireGuard tunnel is configured, which needs the clock and only
  // exists on an ESP32 where the heap argument for the skip does not apply.
  if (!otaPending && (!g_safeMode || wgNeedsClock(g_settings))) clockReapply(g_settings);

  // Optional WireGuard tunnel (ESP32 targets). Arms the state machine only;
  // the bring-up itself runs from loop(), so nothing here can delay the web
  // server. A crash last boot feeds the three-strikes hold that keeps a bad
  // tunnel config from locking the device out of its own web UI.
  wgBegin(g_settings, g_safeMode);

  // A GitHub update queued from the web UI runs now, before the features claim
  // the heap (the download needs a 16 KB TLS buffer that only fits at boot).
  // On success it reboots into the new image; a no-op stub on the ESP32 targets.
  //
  // mDNS and SNTP were skipped above specifically for this boot (netBegin's
  // deferMdns arg / the otaPending check): both start a permanent, mid-arena
  // heap allocation the moment WiFi comes up (see netStartMdns/clockReapply),
  // and on this memory-tight chip that's exactly the class of problem this
  // codebase already worked around once for the ticker's cash.ch fetch —
  // fragmenting the largest contiguous block below what a TLS handshake
  // needs, no matter how much *total* free heap remains. Every OTA download
  // failure logged this whole session was of that shape: the 16 KB fallback
  // attempt's own heap check never once passed, always short on contiguous
  // space specifically, never on total free bytes. Starting mDNS/SNTP here
  // instead costs nothing on a normal boot (they still run before the render
  // loop) and gives the attempt below the most contiguous heap this boot
  // will ever have.
  if (otaPending) {
    Serial.println("[boot] github update");
    gfxBoot("SmallTV", "updating...");
    otaBootUpdate(g_settings);
    gfxBoot("SmallTV", "update failed");   // still here -> failed; details in the web UI
    delay(1200);
    netStartMdns();
    if (!g_safeMode || wgNeedsClock(g_settings)) clockReapply(g_settings);
  }

  Serial.println("[boot] web");
  webPortalBegin(g_settings);

  Serial.println("[boot] modes");
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->begin(g_settings);
  Serial.println("[boot] done");

  if (netMode() == NET_AP) {
    gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str());
  } else if (g_safeMode) {
    // Last boot crashed: show the crash address (persistent) and keep the web
    // server up for OTA recovery — don't enter the render path that crashed.
    gfxCrash(g_epcStr, g_addrStr, netIP().c_str());
  } else {
    // Show which network we joined and how to reach the web UI, long enough to read.
    gfxStaInfo(netSSID().c_str(), netIP().c_str(), g_settings.hostname.c_str());
    delay(3500);
  }
}

void loop() {
  stkBegin(); netLoop();       stkEnd(STK_NET);
  stkBegin(); webPortalLoop(); stkEnd(STK_WEB);

  if (webPortalRebootDue()) {
    delay(120);
    ESP.restart();
  }

  // Before the safe-mode return on purpose: if the crash had nothing to do with
  // the tunnel, remote access survives it, and if it did, the three-strikes hold
  // stops the retries by itself.
  wgService(g_settings);

  if (g_safeMode) {
    // See SAFE_MODE_RECOVER_MS above: give the features another go after a
    // while, unless this is already a streak of crashes (or an upload is
    // being written right now).
    if (g_crashStreak < SAFE_MODE_MAX_STREAK && millis() >= SAFE_MODE_RECOVER_MS &&
        !Update.isRunning()) {
      delay(120);
      ESP.restart();
    }
    delay(5);
    return;  // crashed last boot: web UI stays up for OTA recovery, no rendering
  }

  if (netMode() == NET_AP) {
    delay(5);
    return;  // setup mode: AP info stays on screen
  }

  // --- STA mode: the active feature fetches + renders itself ---

  if (!g_streakCleared && millis() >= HEALTHY_RUN_MS) {
    crashStreakNoteHealthy();   // ran normally long enough: forget earlier crashes
    g_streakCleared = true;
  }

  // Night-mode state machine (NTP-trust gate), then apply the effective brightness
  // (night override / auto-brightness / manual level).
  stkBegin();
  clockService(g_settings);
  appApplyBrightness();
  stkEnd(STK_CLOCK);

  // On expiry the carousel dwell is credited back the time it was hidden, so it
  // resumes on the same feature with the same remaining slice.
  static bool wasNotifying = false;
  if (g_notifyMode.active()) {
    wasNotifying = true;
    stkBegin();
    g_notifyMode.service(g_settings);
    stkEnd(STK_NOTIFY);
    delay(5);
    return;
  }
  bool restore = wasNotifying;
  if (wasNotifying) {
    wasNotifying = false;
    if (g_carSwitch) g_carSwitch += g_notifyMode.heldMs();
  }

  stkBegin();
  DisplayMode* m = activeMode(g_settings);   // may wake() the incoming carousel mode
  if (m) {
    if (restore) m->wake(g_settings);
    m->service(g_settings);
    for (size_t i = 0; i < kModeCount; i++) {
      if (kModes[i] == m) { stkEnd(STK_MODES + i); break; }
    }
  }

  delay(5);
}
