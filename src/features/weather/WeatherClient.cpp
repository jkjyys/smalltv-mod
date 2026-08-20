#include "WeatherClient.h"
#include "Platform.h"
#include "config.h"
#include <ArduinoJson.h>

static WeatherNow g_w = {};
static uint32_t   g_nextPollMs = 0;

// TLS receive-buffer size for the Open-Meteo handshake, probed once (same
// approach as RadarClient's adsb.fi probe) so BearSSL can use the smallest
// buffer this host's certificate chain allows.
static uint16_t g_tlsRx = 0;

const WeatherNow& weatherCurrent() { return g_w; }

void weatherInit(const Settings& s) {
  (void)s;
  g_w = WeatherNow{};
  g_nextPollMs = millis();
}

void weatherForceRefresh() { g_nextPollMs = millis(); }

static void probeTls() {
#if defined(SMALLTV_ESP8266)
  if (g_tlsRx) return;
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(WEATHER_HOST, 443, 512))       g_tlsRx = 512;
  else if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(WEATHER_HOST, 443, 1024)) g_tlsRx = 1024;
  else                                                                                 g_tlsRx = 4096;
#endif
}

static String buildUrl(const Settings& s) {
  String u = F("https://");
  u += F(WEATHER_HOST);
  u += F(WEATHER_PATH);
  u += F("?latitude=");
  u += String(s.weather.lat, 4);
  u += F("&longitude=");
  u += String(s.weather.lon, 4);
  u += F("&current=temperature_2m,weather_code,is_day");
  u += F("&daily=temperature_2m_max,temperature_2m_min");
  u += F("&forecast_days=1&timezone=auto&temperature_unit=");
  u += s.weather.fahrenheit ? F("fahrenheit") : F("celsius");
  return u;
}

// Keep only the fields WeatherMode draws — the full Open-Meteo response also
// carries units/metadata blocks we don't need on a memory-tight ESP8266.
static bool parseForecast(Stream& stream) {
  JsonDocument filter;
  JsonObject cur = filter["current"].to<JsonObject>();
  cur["temperature_2m"] = true;
  cur["weather_code"]   = true;
  cur["is_day"]         = true;
  JsonObject day = filter["daily"].to<JsonObject>();
  day["temperature_2m_max"] = true;
  day["temperature_2m_min"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonObjectConst cu = doc["current"].as<JsonObjectConst>();
  if (cu.isNull() || !(cu["temperature_2m"].is<float>() || cu["temperature_2m"].is<int>()))
    return false;

  g_w.temp   = cu["temperature_2m"].as<float>();
  g_w.code   = cu["weather_code"] | 0;
  g_w.isDay  = (cu["is_day"] | 1) != 0;

  JsonArrayConst hiArr = doc["daily"]["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst loArr = doc["daily"]["temperature_2m_min"].as<JsonArrayConst>();
  g_w.hi = (!hiArr.isNull() && hiArr.size() > 0) ? hiArr[0].as<float>() : g_w.temp;
  g_w.lo = (!loArr.isNull() && loArr.size() > 0) ? loArr[0].as<float>() : g_w.temp;

  g_w.valid = true;
  g_w.error = false;
  g_w.lastOkMs = millis();
  return true;
}

static bool fetchOnce(const Settings& s) {
  // TLS needs a contiguous heap chunk; skip rather than reset-loop if too low.
  // (Lower than radar's 18000 — weather has no aircraft-array footprint of its
  // own, so it fits in less free heap; tune here first if fetches keep failing
  // on a fully-loaded build with every feature compiled in.)
  if (ESP.getFreeHeap() < 13000) return false;
  probeTls();

  std::unique_ptr<NetClient> client(platformMakeSecureClient(g_tlsRx));

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  String url = buildUrl(s);
  if (!http.begin(*client, url)) return false;
  http.addHeader("Accept", "application/json");
  http.setUserAgent(F(WEATHER_USER_AGENT));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  bool ok = parseForecast(http.getStream());
  http.end();
  return ok;
}

void weatherService(const Settings& s) {
  // No location set yet -> nothing to fetch (the mode shows a prompt instead).
  if (s.weather.lat == 0.0f && s.weather.lon == 0.0f) return;

  if ((int32_t)(millis() - g_nextPollMs) < 0) return;
  g_nextPollMs = millis() + (uint32_t)s.weather.pollSec * 1000UL;

  if (!fetchOnce(s)) g_w.error = true;   // keep stale reading, flag the error
}
