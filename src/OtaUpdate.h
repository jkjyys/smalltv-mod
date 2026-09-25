// OtaUpdate.h — pull the latest release from GitHub and flash it over the air.
//
// Checks the repo's newest release via the GitHub API, compares its tag to
// FW_VERSION, and (on request) streams the target's firmware asset (UPDATE_ASSET
// in config.h) straight into the OTA partition — ESP8266httpUpdate on the
// ESP8266, HTTPUpdate on the ESP32 targets. The write is atomic: a failed
// download leaves the running firmware untouched, so this cannot brick the device.
//
// HTTPS on the ESP8266 is RAM-tight: github.com and its asset CDN do not
// negotiate small TLS fragments, so the download needs a full 16 KB BearSSL
// receive buffer that does not fit next to the running features (~26 KB free).
// The ESP8266 therefore updates AT BOOT: the web UI queues the request in
// LittleFS and reboots, otaBootUpdate() runs early in setup() with ~45 KB
// free, and the device reboots again into the new image. Manual OTA (upload
// the .bin in the System tab) stays as the fallback.
#pragma once
#include <Arduino.h>
#include "Settings.h"

struct OtaLatest {
  bool   ok = false;      // the check itself succeeded
  bool   newer = false;   // the latest release is newer than FW_VERSION
  String tag;             // e.g. "v2.1.0"
  String url;             // firmware asset download URL
  String error;           // human-readable reason when ok == false
};

OtaLatest otaCheckLatest(const Settings& s);   // query the GitHub API (blocking)

// Run the update (blocking). Returns "" once the download starts and the device is
// about to reboot into the new image; otherwise a human-readable error.
// ESP32 targets only — the ESP8266 uses the boot-update flow below.
String otaUpdateFromGitHub(const Settings& s);

// Update-at-boot flow (real on the ESP8266; no-op stubs on the ESP32 targets,
// which update directly). The request and the failure message live in LittleFS
// so they survive the reboot.
bool   otaBootRequested();                     // a boot update is queued
bool   otaRequestBootUpdate(const char* tag);  // queue it (false if storage write failed)
void   otaBootUpdate(const Settings& s);       // consume the request; reboots on success
String otaTakeBootResult();                    // last boot attempt's error, "" if none

// Call once early in setup(), right after LittleFS is mounted and before the
// web portal starts. If the previous boot's update was cut off by a reset
// (watchdog, exception) -- which leaves no error message of its own, since the
// code that would write one never ran -- this turns the progress breadcrumb it
// left in RTC memory into the "Last update" message, naming the step and byte
// offset it died at. After a successful update it reports the version change
// instead. ESP8266 only; a no-op on the ESP32 targets.
void   otaReportInterrupted(const char* resetReason);

// Automatic-update loop guard (all targets). The first automatic check after
// every boot fires a few minutes in, and on the ESP8266 a failed attempt ends
// in a reboot -- so without this, a release the device can't install would
// reboot it every few minutes, forever (v2.9.28 did exactly that for ~2 days).
// The first check after a boot installs a given release at most
// OTA_AUTO_MAX_TRIES times; after that only the regular every-N-hours check
// (and the Update now button, which is never limited) tries it again.
static const uint8_t OTA_AUTO_MAX_TRIES = 2;
bool   otaAutoAttemptAllowed(const String& tag);   // fewer than OTA_AUTO_MAX_TRIES automatic tries of tag so far
void   otaNoteAutoAttempt(const String& tag);      // count one automatic try of tag (before queueing it)
