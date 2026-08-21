// WeatherData.h — runtime (volatile) data for the weather feature.
#pragma once
#include <Arduino.h>

// One fetch's worth of current conditions + today's high/low, as parsed from
// Open-Meteo. WMO weather codes: https://open-meteo.com/en/docs — WeatherMode
// buckets `code` into a small set of icons (clear/cloud/rain/snow/storm).
struct WeatherNow {
  bool     valid;      // a successful fetch has landed at least once
  bool     error;       // last fetch failed (valid may still be true = stale data)
  int16_t  lastCode;      // diagnostic: HTTP status of the last attempt, or a
                          // negative HTTPClient error code (see HTTPClient.h),
                          // or -1000 if the heap gate skipped the attempt entirely
  // temp/hi/lo arrive already in the unit requested from the API (Settings.weather.fahrenheit
  // picks celsius vs fahrenheit in the request itself, so no conversion happens on-device).
  float    temp;
  float    hi, lo;       // today's forecast high/low
  int16_t  code;          // WMO weather code
  bool     isDay;
  uint32_t lastOkMs;
};
