// CrashTrace.cpp — see CrashTrace.h.
#include "CrashTrace.h"
#include "Platform.h"
#include "config.h"
#include <LittleFS.h>

#if defined(SMALLTV_ESP8266)

extern "C" {
// From the core's umm_malloc: the caller of the last malloc/realloc that failed.
extern void* umm_last_fail_alloc_addr;
extern int   umm_last_fail_alloc_size;
}

// RTC user memory, addressed directly: user block b is at 0x60001200 + 4*b
// (the same place ESP.rtcUserMemoryRead/Write reach, via system_rtc_mem_*).
// Direct stores keep the crash callback free of SDK calls. Blocks 0-31 hold
// eboot's OTA command and 96-101 OtaUpdate.cpp's update breadcrumb.
static volatile uint32_t* const RTC_USER = reinterpret_cast<volatile uint32_t*>(0x60001200);

static const uint32_t CRASH_BLOCK  = 104;
static const uint32_t CRASH_MAGIC  = 0x43525331UL;   // "CRS1"
static const uint32_t TRACE_MAX    = 12;
// magic, reason, exccause, epc1, excvaddr, uptimeMs, failAlloc, failSize, n,
// trace[TRACE_MAX], check
static const uint32_t CRASH_WORDS  = 9 + TRACE_MAX + 1;   // 22 words: blocks 104..125
static const uint32_t STREAK_BLOCK = 126;
static const uint32_t STREAK_MAGIC = 0x5AFE0000UL;

static const char*  CRASH_LOG      = "/crash.log";
static const size_t CRASH_LOG_KEEP = 5;

static bool isCodeAddr(uint32_t v) {
  return (v >= 0x40100000UL && v < 0x40110000UL) ||   // IRAM
         (v >= 0x40200000UL && v < 0x40300000UL);     // flash-mapped sketch
}

// Runs inside the core's crash handler (exceptions, soft watchdog, panics,
// stack-smash detection), right before the chip restarts. Keep it simple:
// no heap, no SDK calls, no flash writes.
extern "C" void custom_crash_callback(struct rst_info* ri, uint32_t stack, uint32_t stack_end) {
  uint32_t w[CRASH_WORDS];
  for (uint32_t i = 0; i < CRASH_WORDS; i++) w[i] = 0;
  w[0] = CRASH_MAGIC;
  w[1] = ri ? ri->reason : 0;
  w[2] = ri ? ri->exccause : 0;
  w[3] = ri ? ri->epc1 : 0;
  w[4] = ri ? ri->excvaddr : 0;
  w[5] = millis();
  w[6] = (uint32_t)umm_last_fail_alloc_addr;
  w[7] = (uint32_t)umm_last_fail_alloc_size;
  uint32_t n = 0;
  uint32_t scanned = 0;
  for (uint32_t p = stack; p + 4 <= stack_end && n < TRACE_MAX && scanned < 1536; p += 4, scanned++) {
    uint32_t v = *reinterpret_cast<const uint32_t*>(p);
    if (isCodeAddr(v)) w[9 + n++] = v;
  }
  w[8] = n;
  uint32_t check = 0xA5A5A5A5UL;
  for (uint32_t i = 0; i < CRASH_WORDS - 1; i++) check ^= w[i] + i;
  w[CRASH_WORDS - 1] = check;
  for (uint32_t i = 0; i < CRASH_WORDS; i++) RTC_USER[CRASH_BLOCK + i] = w[i];
}

static const char* reasonText(uint32_t r) {
  switch (r) {
    case 1:   return "hardware watchdog";
    case 2:   return "exception";
    case 3:   return "soft watchdog";
    case 253: return "stack overflow";
    case 254: return "panic/abort";
    default:  return "reset";
  }
}

void crashTraceBoot() {
  uint32_t w[CRASH_WORDS];
  for (uint32_t i = 0; i < CRASH_WORDS; i++) w[i] = RTC_USER[CRASH_BLOCK + i];
  if (w[0] != CRASH_MAGIC) return;
  uint32_t check = 0xA5A5A5A5UL;
  for (uint32_t i = 0; i < CRASH_WORDS - 1; i++) check ^= w[i] + i;
  RTC_USER[CRASH_BLOCK] = 0;                     // consume: log each crash once
  if (check != w[CRASH_WORDS - 1]) return;

  // One line per crash, e.g.
  // "v2.9.34 exception(28) epc 0x4000df64 addr 0x00000000 up 812s trace 0x4020a1b2 ..."
  char line[260];
  int len = snprintf(line, sizeof(line), "v" FW_VERSION " %s(%u) epc 0x%08x addr 0x%08x up %us",
                     reasonText(w[1]), (unsigned)w[2], (unsigned)w[3], (unsigned)w[4],
                     (unsigned)(w[5] / 1000));
  if (w[6] && len > 0 && len < (int)sizeof(line)) {
    len += snprintf(line + len, sizeof(line) - len, " failedAlloc 0x%08x(%u)",
                    (unsigned)w[6], (unsigned)w[7]);
  }
  uint32_t n = w[8] > TRACE_MAX ? TRACE_MAX : w[8];
  if (n && len > 0 && len < (int)sizeof(line)) {
    len += snprintf(line + len, sizeof(line) - len, " trace");
  }
  for (uint32_t i = 0; i < n && len > 0 && len < (int)sizeof(line); i++) {
    len += snprintf(line + len, sizeof(line) - len, " 0x%08x", (unsigned)w[9 + i]);
  }

  // Keep the newest CRASH_LOG_KEEP lines.
  String kept[CRASH_LOG_KEEP];
  size_t count = 0;
  File in = LittleFS.open(CRASH_LOG, "r");
  if (in) {
    while (in.available()) {
      String l = in.readStringUntil('\n');
      l.trim();
      if (!l.length()) continue;
      if (count < CRASH_LOG_KEEP) {
        kept[count++] = l;
      } else {
        for (size_t i = 1; i < CRASH_LOG_KEEP; i++) kept[i - 1] = kept[i];
        kept[CRASH_LOG_KEEP - 1] = l;
      }
    }
    in.close();
  }
  File out = LittleFS.open(CRASH_LOG, "w");
  if (!out) return;
  size_t start = (count == CRASH_LOG_KEEP) ? 1 : 0;   // drop the oldest to make room
  for (size_t i = start; i < count; i++) { out.print(kept[i]); out.print('\n'); }
  out.print(line);
  out.print('\n');
  out.close();
}

void crashTraceJson(JsonArray out) {
  File in = LittleFS.open(CRASH_LOG, "r");
  if (!in) return;
  while (in.available()) {
    String l = in.readStringUntil('\n');
    l.trim();
    if (l.length()) out.add(l);
  }
  in.close();
}

uint8_t crashStreakOnBoot(bool lastBootCrashed) {
  uint32_t v = RTC_USER[STREAK_BLOCK];
  uint8_t n = ((v & 0xFFFF0000UL) == STREAK_MAGIC) ? (uint8_t)(v & 0xFF) : 0;
  // A clean boot does NOT reset the streak: safe mode's own recovery reboot is
  // a clean boot too. Only a long-enough healthy run does (see main.cpp).
  if (lastBootCrashed && n < 255) n++;
  RTC_USER[STREAK_BLOCK] = STREAK_MAGIC | n;
  return n;
}

void crashStreakNoteHealthy() {
  RTC_USER[STREAK_BLOCK] = STREAK_MAGIC;
}

#else   // ESP32 targets: no custom_crash_callback hook here

void    crashTraceBoot() {}
void    crashTraceJson(JsonArray) {}
uint8_t crashStreakOnBoot(bool) { return 0; }
void    crashStreakNoteHealthy() {}

#endif
