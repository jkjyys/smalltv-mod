#include "WeatherClient.h"
#include "Platform.h"
#include "config.h"
#include <ArduinoJson.h>

static WeatherNow g_w = {};
static uint32_t   g_nextPollMs = 0;
static String     g_parseErr;   // diagnostic detail behind lastCode == -800

// TLS receive-buffer size, probed once per host (same approach as RadarClient's
// adsb.fi probe) so BearSSL can use the smallest buffer that host's cert chain
// allows. Neither host seems to honor MFLN in practice, so both likely fall
// through to the generous "hope it fits" branch rather than the 512/1024 a
// probe would otherwise pick.
static uint16_t g_omTlsRx = 0;    // api.open-meteo.com
static uint16_t g_owmTlsRx = 0;   // api.openweathermap.org

const WeatherNow& weatherCurrent() { return g_w; }
const String&     weatherLastParseErr() { return g_parseErr; }

void weatherInit(const Settings& s) {
  (void)s;
  g_w = WeatherNow{};
  g_nextPollMs = millis();
}

void weatherForceRefresh() { g_nextPollMs = millis(); }

static uint16_t probeTls(const char* host, uint16_t& cache) {
#if defined(SMALLTV_ESP8266)
  if (cache) return cache;
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 512))       cache = 512;
  else if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 1024)) cache = 1024;
  else                                                                        cache = 5120;
  return cache;
#else
  return 0;
#endif
}

static String buildOpenMeteoUrl(const Settings& s) {
  String u = F("https://");
  u += F(WEATHER_HOST);
  u += F(WEATHER_PATH);
  u += F("?latitude=");
  u += String(s.weather.lat, 4);
  u += F("&longitude=");
  u += String(s.weather.lon, 4);
  u += F("&current=temperature_2m,weather_code,is_day,relative_humidity_2m");
  u += F("&daily=temperature_2m_max,temperature_2m_min");
  u += F("&forecast_days=1&timezone=auto&temperature_unit=");
  u += s.weather.fahrenheit ? F("fahrenheit") : F("celsius");
  return u;
}

static String buildOwmUrl(const Settings& s) {
  String u = F("https://");
  u += F(OWM_HOST);
  u += F(OWM_PATH);
  u += F("?lat=");
  u += String(s.weather.lat, 4);
  u += F("&lon=");
  u += String(s.weather.lon, 4);
  u += F("&units=");
  u += s.weather.fahrenheit ? F("imperial") : F("metric");
  u += F("&appid=");
  u += s.weather.owmKey;
  return u;
}

// Keep only the fields WeatherMode draws — the full Open-Meteo response also
// carries units/metadata blocks we don't need on a memory-tight ESP8266.
// Takes the whole body as a String (rather than streaming) so that, on an
// unexpected shape, we can put a snippet of what the server actually sent
// into g_parseErr instead of just "not what we expected".
static bool parseOpenMeteo(const String& body) {
  JsonDocument filter;
  JsonObject cur = filter["current"].to<JsonObject>();
  cur["temperature_2m"] = true;
  cur["weather_code"]   = true;
  cur["is_day"]         = true;
  cur["relative_humidity_2m"] = true;
  JsonObject day = filter["daily"].to<JsonObject>();
  day["temperature_2m_max"] = true;
  day["temperature_2m_min"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) { g_parseErr = String("deserialize: ") + err.c_str(); return false; }

  JsonObjectConst cu = doc["current"].as<JsonObjectConst>();
  if (cu.isNull()) {
    g_parseErr = "no current object, body starts: " + body.substring(0, 100);
    return false;
  }
  if (!(cu["temperature_2m"].is<float>() || cu["temperature_2m"].is<int>())) {
    g_parseErr = "no current.temperature_2m, body starts: " + body.substring(0, 100);
    return false;
  }

  g_w.temp   = cu["temperature_2m"].as<float>();
  g_w.code   = cu["weather_code"] | 0;
  g_w.isDay  = (cu["is_day"] | 1) != 0;
  g_w.humidity = cu["relative_humidity_2m"] | 0.0f;   // 0 if the API omitted it — not distinguishable from "0%", but harmless

  JsonArrayConst hiArr = doc["daily"]["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst loArr = doc["daily"]["temperature_2m_min"].as<JsonArrayConst>();
  g_w.hi = (!hiArr.isNull() && hiArr.size() > 0) ? hiArr[0].as<float>() : g_w.temp;
  g_w.lo = (!loArr.isNull() && loArr.size() > 0) ? loArr[0].as<float>() : g_w.temp;

  g_w.valid = true;
  g_w.error = false;
  g_parseErr = "";
  g_w.lastOkMs = millis();
  return true;
}

// OpenWeatherMap uses its own numeric condition codes (200s thunderstorm, 300s
// drizzle, 500s rain, 600s snow, 700s atmosphere/fog/haze, 800 clear, 801-804
// clouds) — different scheme from Open-Meteo's WMO codes. Mapped here to a
// representative WMO-ish code so WeatherMode's existing icon bucketing (which
// only understands WMO codes) needs no changes for either source.
static int16_t owmCodeToWmo(int owmId) {
  if (owmId >= 200 && owmId <= 232) return 95;   // thunderstorm
  if (owmId >= 300 && owmId <= 321) return 51;   // drizzle
  if (owmId >= 500 && owmId <= 531) return 61;   // rain (incl. showers)
  if (owmId >= 600 && owmId <= 622) return 71;   // snow
  if (owmId >= 701 && owmId <= 781) return 45;   // mist/fog/haze/dust/etc
  if (owmId == 800) return 0;                     // clear
  if (owmId == 801) return 1;                     // few clouds
  if (owmId == 802 || owmId == 803) return 2;     // scattered/broken
  if (owmId == 804) return 3;                     // overcast
  return 2;
}

// OpenWeatherMap "current weather" endpoint: observation-based (nearest
// station), unlike Open-Meteo's forecast-model grid — this is what tends to
// match a phone's weather app, and what GeekMagic's own stock firmware used.
// temp_min/temp_max here are the current spread across nearby stations, not a
// forecast for the day the way Open-Meteo's `daily` block is; shown in the
// same H/L slot regardless, since neither is exposed as a true forecast high/
// low without a second (paid-tier) OWM call.
static bool parseOwm(const String& body) {
  JsonDocument filter;
  JsonObject w0 = filter["weather"].add<JsonObject>();
  w0["id"] = true;
  w0["icon"] = true;
  JsonObject main_ = filter["main"].to<JsonObject>();
  main_["temp"] = true;
  main_["temp_min"] = true;
  main_["temp_max"] = true;
  main_["humidity"] = true;
  filter["cod"] = true;
  filter["message"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) { g_parseErr = String("deserialize: ") + err.c_str(); return false; }

  JsonObjectConst main = doc["main"].as<JsonObjectConst>();
  if (main.isNull() || !(main["temp"].is<float>() || main["temp"].is<int>())) {
    const char* msg = doc["message"] | "";
    g_parseErr = msg[0] ? (String("owm: ") + msg) : ("no main.temp, body starts: " + body.substring(0, 100));
    return false;
  }

  g_w.temp = main["temp"].as<float>();
  g_w.hi   = main["temp_max"] | g_w.temp;
  g_w.lo   = main["temp_min"] | g_w.temp;
  g_w.humidity = main["humidity"] | 0.0f;

  JsonObjectConst w = doc["weather"][0];
  int id = w["id"] | 800;
  g_w.code = owmCodeToWmo(id);
  const char* icon = w["icon"] | "01d";
  g_w.isDay = (icon[0] && icon[strlen(icon) - 1] != 'n');

  g_w.valid = true;
  g_w.error = false;
  g_parseErr = "";
  g_w.lastOkMs = millis();
  return true;
}

static bool fetchOnce(const Settings& s) {
  bool owm = (s.weather.source == WSRC_OWM);
  if (owm && s.weather.owmKey.length() < 4) { g_w.lastCode = -700; g_parseErr = "no OpenWeatherMap API key set"; return false; }

  // TLS needs a contiguous heap chunk; skip rather than reset-loop if too low.
  // Sized for the larger 5120 B fallback buffer below (radar's 18000 assumed a
  // 4096 B buffer) — tune here first if fetches keep failing for lack of heap.
  if (ESP.getFreeHeap() < 16000) { g_w.lastCode = -1000; return false; }

  const char* host = owm ? OWM_HOST : WEATHER_HOST;
  uint16_t rx = probeTls(host, owm ? g_owmTlsRx : g_omTlsRx);

  std::unique_ptr<NetClient> client(platformMakeSecureClient(rx));

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  String url = owm ? buildOwmUrl(s) : buildOpenMeteoUrl(s);
  if (!http.begin(*client, url)) { g_w.lastCode = -900; return false; }
  http.addHeader("Accept", "application/json");
  http.setUserAgent(F(WEATHER_USER_AGENT));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  g_w.lastCode = (int16_t)code;   // diagnostic: surfaced via /api/status
  if (code != HTTP_CODE_OK) {
    // OWM's own error body is short and worth surfacing (e.g. a bad key gives
    // a plain-text reason), unlike a generic HTTP failure.
    if (owm) { String body = http.getString(); g_parseErr = "http " + String(code) + ": " + body.substring(0, 100); }
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  bool ok = owm ? parseOwm(body) : parseOpenMeteo(body);
  if (!ok) g_w.lastCode = -800;   // got a 200 but couldn't parse it — different bug class
  return ok;
}

void weatherService(const Settings& s) {
  // No location set yet -> nothing to fetch (the mode shows a prompt instead).
  if (s.weather.lat == 0.0f && s.weather.lon == 0.0f) return;

  if ((int32_t)(millis() - g_nextPollMs) < 0) return;
  g_nextPollMs = millis() + (uint32_t)s.weather.pollSec * 1000UL;

  if (!fetchOnce(s)) g_w.error = true;   // keep stale reading, flag the error
}
