// CrashTrace.h — remember why and where the device last crashed.
//
// The SmallTV has no serial console, so a crash's own report (the stack dump
// the ESP8266 core prints to UART) is lost, and all that survives is the reset
// reason plus one exception address. That was not enough to explain the
// crashes seen in September 2026 (a memcpy on a null pointer in one, a jump
// into data RAM in another). On the ESP8266 the core's crash handler calls
// custom_crash_callback() just before rebooting; CrashTrace.cpp implements it
// to save the reset cause, exception address, uptime, the last failed
// allocation and the code addresses found on the crashed stack into RTC
// memory, which survives the reboot. The next boot moves that record into a
// small log in LittleFS (last few crashes, kept across power cuts) that
// /api/status shows as "crashes". The addresses decode to source lines with
// the release's smalltv-mod-firmware.elf:
//   xtensa-lx106-elf-addr2line -pfiaC -e smalltv-mod-firmware.elf 0x4020abcd ...
// No-ops on the ESP32 targets.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Call once in setup(), after LittleFS is mounted.
void crashTraceBoot();

// Appends the logged crashes (newest last) to a JSON array.
void crashTraceJson(JsonArray out);

// Crash-streak bookkeeping for main.cpp's safe mode: how many crashes happened
// without a long-enough healthy run in between (kept in RTC memory, so a power
// cut clears it). crashStreakOnBoot() counts this boot's crash, if any, and
// returns the streak; crashStreakNoteHealthy() resets it.
uint8_t crashStreakOnBoot(bool lastBootCrashed);
void    crashStreakNoteHealthy();
