#include "StockClient.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

// ---------------------------------------------------------------------------
// US market hours (for SymbolCfg.altSymbol — see config.h's BINANCE_HOST
// comment). Computed straight from UTC + a hand-rolled US DST rule, entirely
// independent of the device's own display timezone: NYSE/Nasdaq regular
// session is 9:30-16:00 America/New_York, Mon-Fri, DST 2nd-Sunday-March to
// 1st-Sunday-November. Doesn't know about market holidays — on a holiday this
// says "open" when it isn't, so a symbol with altSymbol set would show its
// Yahoo price frozen at the prior close instead of switching to Binance. A
// known, acceptable gap: holidays are a handful of days a year, and the
// symptom (a frozen-looking price) is the same one this feature exists to
// reduce, not a new failure mode.
static int8_t nthSundayOfMonth(int year, int month /*1-12*/, int n) {
  // Sakamoto's day-of-week algorithm — plain integer math, no struct tm and
  // no mktime(). mktime() normalizes a full tm struct and is surprisingly
  // stack-heavy on this platform's libc; calling it every stepSymbol tick ate
  // into the ESP8266's already-thin continuation stack enough to crash-loop
  // the device. This does the same job (day-of-week of the 1st of the month)
  // with a handful of local ints.
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = (month < 3) ? year - 1 : year;
  int dow1 = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + 1) % 7;   // 0 = Sunday, day = 1
  int firstSunday = 1 + ((7 - dow1) % 7);
  return firstSunday + 7 * (n - 1);
}

static bool usEasternIsDST(const struct tm& utc) {
  int y = utc.tm_year + 1900;
  int marchSun2 = nthSundayOfMonth(y, 3, 2);
  int novSun1   = nthSundayOfMonth(y, 11, 1);
  // Transitions are 2am ET (07:00 UTC in EST, 06:00 UTC in EDT) — a day-level
  // compare is precise enough here; the 1h edge case only matters in the hour
  // of the transition itself.
  if (utc.tm_mon + 1 < 3 || utc.tm_mon + 1 > 11) return false;
  if (utc.tm_mon + 1 > 3 && utc.tm_mon + 1 < 11) return true;
  if (utc.tm_mon + 1 == 3)  return utc.tm_mday >= marchSun2;
  /* November */            return utc.tm_mday < novSun1;
}

static bool usMarketRegularHoursNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) return true;   // clock not synced yet — assume open, so a symbol just falls back to its normal Yahoo path instead of guessing wrong
  struct tm utc;
  gmtime_r(&now, &utc);
  int offsetHours = usEasternIsDST(utc) ? -4 : -5;

  time_t etNow = now + (time_t)offsetHours * 3600;
  struct tm et;
  gmtime_r(&etNow, &et);   // "UTC" broken-down fields of a shifted epoch = ET wall-clock fields

  if (et.tm_wday == 0 || et.tm_wday == 6) return false;   // weekend
  int minutesIntoDay = et.tm_hour * 60 + et.tm_min;
  return minutesIntoDay >= (9 * 60 + 30) && minutesIntoDay < (16 * 60);
}


static StockData g_stocks[MAX_SYMBOLS];
static uint8_t   g_count = 0;

static bool     g_refreshing = false;
static uint8_t  g_fetchIdx = 0;

// ---------------------------------------------------------------------------
void stocksInit(const Settings& s) {
  g_count = s.ticker.symbolCount;
  for (uint8_t i = 0; i < g_count; i++) {
    g_stocks[i].clear();
    strlcpy(g_stocks[i].symbol, s.ticker.symbols[i].symbol, MAX_SYMBOL_LEN);
    g_stocks[i].source = s.ticker.symbols[i].source;
    g_stocks[i].qty  = s.ticker.symbols[i].qty;
    g_stocks[i].cost = s.ticker.symbols[i].cost;
    strlcpy(g_stocks[i].altSymbol, s.ticker.symbols[i].altSymbol, MAX_SYMBOL_LEN);
    g_stocks[i].userNamed = (s.ticker.symbols[i].name[0] != 0);
    strlcpy(g_stocks[i].name,
            g_stocks[i].userNamed ? s.ticker.symbols[i].name : s.ticker.symbols[i].symbol,
            MAX_NAME_LEN);
    g_stocks[i].nextTryMs = millis();     // every symbol is due right away
  }
  g_refreshing = false;
}

void stocksForceRefresh() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < g_count; i++) {
    g_stocks[i].nextTryMs = now;
    g_stocks[i].fails = 0;                // a manual refresh starts the backoff over
  }
  g_refreshing = false;
}

uint8_t          stocksCount()        { return g_count; }
const StockData& stockAt(uint8_t i)   { return g_stocks[i]; }

uint32_t stockRetryInSec(uint8_t i) {
  if (i >= g_count) return 0;
  int32_t left = (int32_t)(g_stocks[i].nextTryMs - millis());
  return left > 0 ? (uint32_t)left / 1000UL : 0;
}

bool stocksAnyValid() {
  for (uint8_t i = 0; i < g_count; i++)
    if (g_stocks[i].valid) return true;
  return false;
}

bool stockDisplayChange(const StockData& d, const TickerSettings& t,
                        float& chg, float& pct, bool* onRange) {
  if (t.changeOnRange && d.sparkCount >= 1 && d.spark[0] > 0 && d.price > 0) {
    chg = d.price - d.spark[0];
    pct = chg / d.spark[0] * 100.0f;
    if (onRange) *onRange = true;
    return true;
  }
  if (onRange) *onRange = false;
  chg = d.change;
  pct = d.changePct;
  return d.hasChange;
}

// ---- URL helpers ----------------------------------------------------------
static String urlEncode(const char* src) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (const char* p = src; *p; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

static String buildWebhookUrl(const Settings& s, const char* symbol) {
  String url = s.ticker.webhookUrl;
  char sep = (url.indexOf('?') >= 0) ? '&' : '?';
  url += sep;
  url += "symbol=" + urlEncode(symbol);
  url += "&range=" + urlEncode(s.ticker.range.c_str());
  url += "&points=" + String(s.ticker.points);
  return url;
}

// Map the chart timeframe to a sensible Yahoo candle interval (mirrors the
// reference n8n workflow so the sparkline has a useful number of points).
static const char* yahooInterval(const String& r) {
  if (r == "1d")  return "5m";
  if (r == "5d")  return "30m";
  if (r == "1mo" || r == "3mo" || r == "6mo" || r == "ytd") return "1d";
  if (r == "1y"  || r == "2y")  return "1wk";
  if (r == "5y"  || r == "10y" || r == "max") return "1mo";
  return "1d";
}

static String buildYahooUrl(const Settings& s, const char* host, const char* symbol) {
  String range = s.ticker.range;
  range.toLowerCase();
  if (range.length() == 0) range = "1d";
  String url = F("https://");
  url += host;
  url += F(YAHOO_CHART_PATH);
  url += urlEncode(symbol);          // e.g. AAPL, NESN.SW, BTC-USD, EURUSD=X
  url += F("?range=");
  url += range;
  url += F("&interval=");
  url += yahooInterval(range);
  url += F("&includePrePost=true");   // ask for extended-hours fields too — see StockClient.cpp's parseYahoo
  return url;
}

// v7/finance/quote — see config.h's YAHOO_QUOTE_HOST comment. One extra
// best-effort fetch, only for the marketState/post/pre fields the v8 chart
// endpoint above doesn't carry.
static String buildYahooQuoteUrl(const char* host, const char* symbol) {
  String url = F("https://");
  url += host;
  url += F(YAHOO_QUOTE_PATH);
  url += F("?symbols=");
  url += urlEncode(symbol);
  return url;
}

// Short, display-safe currency prefix. The built-in bitmap font has no glyphs
// for symbols like €, so non-USD currencies are shown as their ISO code.
static void yahooCurrency(const char* code, char* out, size_t n) {
  if (!code || !code[0]) { out[0] = 0; return; }
  if (!strcmp(code, "USD")) { strlcpy(out, "$", n); return; }
  snprintf(out, n, "%s ", code);     // "CHF 79.73", "EUR 1.08", ...
}

// ---- URL builders: cash.ch --------------------------------------------------
// cash.ch answers hand-written GraphQL sent as a plain GET ?query=... — no
// auth, no cookies, no required headers (see config.h). The symbol is the
// cash.ch listing key (valor-marketId-currencyId).

// Daily closes to request per chart timeframe. The server trims to the newest
// N (max=N) inside a fixed catch-all from/to window, so the device needs no
// clock; asking for more than cash.ch stores (~6 months) just returns it all.
static uint16_t cashRangeDays(const String& r) {
  if (r == "1d")  return 2;          // >=2 points so a line can be drawn
  if (r == "5d")  return 5;
  if (r == "1mo") return 22;
  if (r == "3mo") return 65;
  if (r == "6mo" || r == "ytd") return 130;
  if (r == "1y")  return 260;
  return 400;                        // 2y/5y/max: everything available
}

static String buildCashUrl(const String& query) {
  String url = F("https://" CASH_GQL_HOST CASH_GQL_PATH "?query=");
  url += urlEncode(query.c_str());
  return url;
}

static String buildCashQuoteUrl(const char* symbol) {
  String q = F("query{quoteList(listingKeys:\"");
  q += symbol;
  q += F("\"){quoteList{edges{node{...on Instrument{"
         "lval iNetVperprV perfPercentage mCur mShortName}}}}}}");
  return buildCashUrl(q);
}

static String buildCashChartUrl(const Settings& s, const char* symbol) {
  String q = F("query{integration{solid{chart(listingKey:\"");
  q += symbol;
  q += F("\" frequency:\"1d\" from:\"2020-01-01\" to:\"2100-01-01\" max:\"");
  q += String(cashRangeDays(s.ticker.range));
  q += F("\"){timeserie{prices{close}}}}}}");
  return buildCashUrl(q);
}

static String buildFinnhubUrl(const Settings& s, const char* symbol) {
  String url = F("https://" FINNHUB_HOST FINNHUB_QUOTE_PATH "?symbol=");
  url += urlEncode(symbol);
  url += F("&token=");
  url += urlEncode(s.ticker.finnhubKey.c_str());
  return url;
}

static String buildBinanceUrl(const char* symbol) {
  String url = F("https://" BINANCE_HOST BINANCE_TICKER_PATH "?symbol=");
  url += urlEncode(symbol);
  return url;
}

// Interval for the sparkline, matching yahooInterval's spirit (finer-grained
// for a shorter range) but with Binance's own interval vocabulary.
static const char* binanceInterval(const String& r) {
  if (r == "1d")  return "15m";
  if (r == "5d")  return "1h";
  if (r == "1mo" || r == "3mo" || r == "6mo" || r == "ytd") return "1d";
  if (r == "1y"  || r == "2y")  return "1d";
  if (r == "5y"  || r == "10y" || r == "max") return "1w";
  return "1d";
}

static String buildBinanceKlinesUrl(const Settings& s, const char* symbol) {
  String range = s.ticker.range;
  range.toLowerCase();
  if (range.length() == 0) range = "1d";
  String url = F("https://" BINANCE_HOST "/fapi/v1/klines?symbol=");
  url += urlEncode(symbol);
  url += F("&interval=");
  url += binanceInterval(range);
  url += F("&limit=20");   // kept modest: unfiltered parsing below keeps the whole 12-field row per candle
  return url;
}

// ---- URL builder: GitHub static quotes ------------------------------------
// The per-symbol file published by the quotes workflow. Same JSON as a webhook.
static String buildGithubUrl(const char* symbol) {
  String url = F(GH_QUOTES_BASE);
  url += urlEncode(symbol);
  url += F(".json");
  return url;
}

// ---- parse: custom webhook contract ---------------------------------------
static bool parseWebhook(const Settings& s, StockData& d, Stream& stream) {
  // Filter so unexpected/large fields don't blow up the heap.
  JsonDocument filter;
  filter["symbol"] = true;
  filter["name"] = true;
  filter["price"] = true;
  filter["currency"] = true;
  filter["change"] = true;
  filter["changePct"] = true;
  filter["range"] = true;
  filter["ok"] = true;
  filter["spark"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  if (doc["ok"].is<bool>() && doc["ok"].as<bool>() == false) return false;
  if (!doc["price"].is<float>() && !doc["price"].is<int>()) return false;

  float price = doc["price"].as<float>();
  if (isnan(price)) return false;
  d.price = price;

  if (!d.userNamed && doc["name"].is<const char*>())
    strlcpy(d.name, doc["name"].as<const char*>(), MAX_NAME_LEN);
  strlcpy(d.currency, doc["currency"] | "", sizeof(d.currency));

  const char* rng = doc["range"] | s.ticker.range.c_str();
  strlcpy(d.rangeLabel, rng, sizeof(d.rangeLabel));

  bool hasChg = doc["change"].is<float>() || doc["change"].is<int>();
  bool hasPct = doc["changePct"].is<float>() || doc["changePct"].is<int>();
  d.hasChange = hasChg || hasPct;
  if (hasChg) d.change = doc["change"].as<float>();
  if (hasPct) d.changePct = doc["changePct"].as<float>();
  if (d.hasChange) {
    if (!hasPct && hasChg) {               // derive % from absolute change
      float prev = price - d.change;
      d.changePct = (prev != 0) ? (d.change / prev * 100.0f) : 0;
    } else if (hasPct && !hasChg) {        // derive absolute change from %
      d.change = price * d.changePct / (100.0f + d.changePct);
    }
  }

  d.sparkCount = 0;
  if (doc["spark"].is<JsonArrayConst>()) {
    for (JsonVariantConst v : doc["spark"].as<JsonArrayConst>()) {
      if (!v.is<float>() && !v.is<int>()) continue;   // skip nulls, like the other parsers
      if (d.sparkCount >= MAX_SPARK_POINTS) break;
      d.spark[d.sparkCount++] = v.as<float>();
    }
  }

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// ---- parse: Yahoo Finance chart payload -----------------------------------
static bool parseYahoo(const Settings& s, StockData& d, Stream& stream) {
  // Keep only the handful of fields we need; the full payload is large and the
  // `meta` object alone has many nested members we don't care about.
  JsonDocument filter;
  JsonObject fmeta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  fmeta["regularMarketPrice"] = true;
  fmeta["chartPreviousClose"] = true;
  fmeta["previousClose"]      = true;
  fmeta["currency"]           = true;
  fmeta["shortName"]          = true;
  fmeta["longName"]           = true;
  fmeta["marketState"]           = true;   // "PRE" | "REGULAR" | "POST" | "POSTPOST" | "CLOSED" | ...
  fmeta["postMarketPrice"]       = true;
  fmeta["postMarketChange"]      = true;
  fmeta["postMarketChangePercent"] = true;
  fmeta["preMarketPrice"]        = true;
  fmeta["preMarketChange"]       = true;
  fmeta["preMarketChangePercent"] = true;
  fmeta["regularMarketTime"]     = true;   // Unix seconds of the last regular-session price — used for the holiday check below
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonObjectConst res  = doc["chart"]["result"][0];
  JsonObjectConst meta = res["meta"];
  if (meta.isNull()) return false;                 // bad symbol => result null
  if (!meta["regularMarketPrice"].is<float>() &&
      !meta["regularMarketPrice"].is<int>()) return false;

  float price = meta["regularMarketPrice"].as<float>();
  if (isnan(price)) return false;
  d.price = price;

  yahooCurrency(meta["currency"] | "", d.currency, sizeof(d.currency));
  d.regularMarketTime = meta["regularMarketTime"] | 0;

  if (!d.userNamed) {
    const char* nm = meta["shortName"] | (meta["longName"] | (const char*)d.symbol);
    if (nm && nm[0]) strlcpy(d.name, nm, MAX_NAME_LEN);
  }

  // Change vs the previous close.
  float prev = NAN;
  if (meta["chartPreviousClose"].is<float>() || meta["chartPreviousClose"].is<int>())
    prev = meta["chartPreviousClose"].as<float>();
  else if (meta["previousClose"].is<float>() || meta["previousClose"].is<int>())
    prev = meta["previousClose"].as<float>();

  if (!isnan(prev) && prev != 0) {
    d.change = price - prev;
    d.changePct = d.change / prev * 100.0f;
    d.hasChange = true;
  } else {
    d.hasChange = false;
  }

  // Extended-hours overlay: Yahoo keeps regularMarketPrice pinned to the last
  // regular-session trade even while pre/post-market trading is live, so a US
  // stock checked from Korea during the day (US pre/post hours) looked frozen
  // without this — apps like Toss show the moving extended-hours price instead.
  d.extHours = false;
  d.extLabel[0] = 0;
  const char* mkt = meta["marketState"] | "";
  strlcpy(d.dbgMarketState, mkt, sizeof(d.dbgMarketState));
  d.dbgHasPostPrice = !meta["postMarketPrice"].isNull();
  d.dbgHasPrePrice  = !meta["preMarketPrice"].isNull();
  bool isPost = !strcmp(mkt, "POST") || !strcmp(mkt, "POSTPOST");
  bool isPre  = !strcmp(mkt, "PRE") || !strcmp(mkt, "PREPRE");
  if (isPost && (meta["postMarketPrice"].is<float>() || meta["postMarketPrice"].is<int>())) {
    d.price = meta["postMarketPrice"].as<float>();
    if (meta["postMarketChangePercent"].is<float>() || meta["postMarketChangePercent"].is<int>()) {
      d.changePct = meta["postMarketChangePercent"].as<float>();
      d.change = meta["postMarketChange"] | (d.price - price);
      d.hasChange = true;
    }
    d.extHours = true;
    strlcpy(d.extLabel, "AH", sizeof(d.extLabel));
  } else if (isPre && (meta["preMarketPrice"].is<float>() || meta["preMarketPrice"].is<int>())) {
    d.price = meta["preMarketPrice"].as<float>();
    if (meta["preMarketChangePercent"].is<float>() || meta["preMarketChangePercent"].is<int>()) {
      d.changePct = meta["preMarketChangePercent"].as<float>();
      d.change = meta["preMarketChange"] | (d.price - price);
      d.hasChange = true;
    }
    d.extHours = true;
    strlcpy(d.extLabel, "PRE", sizeof(d.extLabel));
  }

  String rl = s.ticker.range;
  rl.toUpperCase();
  strlcpy(d.rangeLabel, rl.c_str(), sizeof(d.rangeLabel));

  // Sparkline from indicators.quote[0].close (oldest -> newest; may hold nulls).
  // Downsample on the fly to at most `points` evenly-spaced samples.
  d.sparkCount = 0;
  JsonArrayConst closes = res["indicators"]["quote"][0]["close"];
  uint16_t want = s.ticker.points;
  if (want > MAX_SPARK_POINTS) want = MAX_SPARK_POINTS;
  if (!closes.isNull() && want >= 2) {
    uint16_t valid = 0;
    for (JsonVariantConst v : closes) if (!v.isNull()) valid++;
    if (valid >= 2) {
      if (valid <= want) {
        for (JsonVariantConst v : closes) {
          if (v.isNull()) continue;
          if (d.sparkCount >= MAX_SPARK_POINTS) break;
          d.spark[d.sparkCount++] = v.as<float>();
        }
      } else {
        uint16_t i = 0, k = 0;
        float last = 0;
        for (JsonVariantConst v : closes) {
          if (v.isNull()) continue;
          last = v.as<float>();
          // k-th output maps to valid-index round(k*(valid-1)/(want-1)).
          uint16_t target =
              (uint16_t)(((uint32_t)k * (valid - 1) + (want - 1) / 2) / (want - 1));
          if (i == target && k < want) { d.spark[d.sparkCount++] = last; k++; }
          i++;
        }
        if (d.sparkCount > 0) d.spark[d.sparkCount - 1] = last;  // pin newest
      }
    }
  }

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// v7/finance/quote overlay — see config.h's YAHOO_QUOTE_HOST comment. Runs
// AFTER a successful parseYahoo() above, so `d` already has its regular-session
// price/change/name/spark/currency; this only overwrites the extHours fields
// if the market is genuinely in a pre/post session right now. Returns false
// (and leaves `d` untouched) on any failure or if the session is REGULAR/
// CLOSED — that's not an error, just nothing to overlay.
static bool applyYahooQuoteExt(StockData& d, Stream& stream) {
  JsonDocument filter;
  filter["quoteResponse"]["result"][0]["marketState"] = true;
  filter["quoteResponse"]["result"][0]["postMarketPrice"] = true;
  filter["quoteResponse"]["result"][0]["postMarketChange"] = true;
  filter["quoteResponse"]["result"][0]["postMarketChangePercent"] = true;
  filter["quoteResponse"]["result"][0]["preMarketPrice"] = true;
  filter["quoteResponse"]["result"][0]["preMarketChange"] = true;
  filter["quoteResponse"]["result"][0]["preMarketChangePercent"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonObjectConst res = doc["quoteResponse"]["result"][0];
  if (res.isNull()) return false;

  const char* mkt = res["marketState"] | "";
  strlcpy(d.dbgMarketState, mkt, sizeof(d.dbgMarketState));
  d.dbgHasPostPrice = !res["postMarketPrice"].isNull();
  d.dbgHasPrePrice  = !res["preMarketPrice"].isNull();

  bool isPost = !strcmp(mkt, "POST") || !strcmp(mkt, "POSTPOST");
  bool isPre  = !strcmp(mkt, "PRE") || !strcmp(mkt, "PREPRE");
  float regularPrice = d.price;   // parseYahoo()'s regular-session price, for a change fallback below

  if (isPost && (res["postMarketPrice"].is<float>() || res["postMarketPrice"].is<int>())) {
    d.price = res["postMarketPrice"].as<float>();
    if (res["postMarketChangePercent"].is<float>() || res["postMarketChangePercent"].is<int>()) {
      d.changePct = res["postMarketChangePercent"].as<float>();
      d.change = res["postMarketChange"] | (d.price - regularPrice);
      d.hasChange = true;
    }
    d.extHours = true;
    strlcpy(d.extLabel, "AH", sizeof(d.extLabel));
    return true;
  }
  if (isPre && (res["preMarketPrice"].is<float>() || res["preMarketPrice"].is<int>())) {
    d.price = res["preMarketPrice"].as<float>();
    if (res["preMarketChangePercent"].is<float>() || res["preMarketChangePercent"].is<int>()) {
      d.changePct = res["preMarketChangePercent"].as<float>();
      d.change = res["preMarketChange"] | (d.price - regularPrice);
      d.hasChange = true;
    }
    d.extHours = true;
    strlcpy(d.extLabel, "PRE", sizeof(d.extLabel));
    return true;
  }
  return false;   // parsed fine, just not in an extended session right now
}

// ---- parse: cash.ch quote ---------------------------------------------------
// {"data":{"quoteList":{"quoteList":{"edges":[{"node":{"lval":"1086.51",
//  "iNetVperprV":"7.36","perfPercentage":"0.68","mCur":"USD",...}}]}}}}
// Numeric fields arrive as JSON *strings*, so read them as text and atof().
static bool parseCashQuote(const Settings& s, StockData& d, Stream& stream) {
  JsonDocument filter;
  JsonObject node =
      filter["data"]["quoteList"]["quoteList"]["edges"][0]["node"].to<JsonObject>();
  node["lval"]           = true;   // last value (price)
  node["iNetVperprV"]    = true;   // absolute day change
  node["perfPercentage"] = true;   // day change in %
  node["mCur"]           = true;
  node["mShortName"]     = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonObjectConst n = doc["data"]["quoteList"]["quoteList"]["edges"][0]["node"];
  const char* lval = n["lval"] | "";       // empty => unknown key / no fix yet
  if (!lval[0]) return false;
  float price = atof(lval);
  if (isnan(price) || price <= 0) return false;
  d.price = price;

  yahooCurrency(n["mCur"] | "", d.currency, sizeof(d.currency));

  if (!d.userNamed) {
    const char* nm = n["mShortName"] | (const char*)d.symbol;
    if (nm && nm[0]) strlcpy(d.name, nm, MAX_NAME_LEN);
  }

  const char* chg = n["iNetVperprV"]    | "";
  const char* pct = n["perfPercentage"] | "";
  d.hasChange = chg[0] || pct[0];
  if (d.hasChange) {
    d.change    = atof(chg);
    d.changePct = pct[0] ? atof(pct) : 0;
    if (!pct[0]) {                          // derive % from the absolute change
      float prev = price - d.change;
      d.changePct = (prev != 0) ? (d.change / prev * 100.0f) : 0;
    }
  }

  String rl = s.ticker.range;
  rl.toUpperCase();
  strlcpy(d.rangeLabel, rl.c_str(), sizeof(d.rangeLabel));

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// ---- parse: Finnhub quote endpoint ------------------------------------------
// {"c":192.35,"d":1.20,"dp":0.63,"h":193.0,"l":190.1,"o":191.0,"pc":191.15,"t":1699999999}
// `c` is Finnhub's definition of "current price" — the latest tradable quote,
// which is the whole point of this source: unlike the Yahoo chart endpoint's
// regularMarketPrice, it moves during pre/post-market instead of freezing at
// the last regular-session trade. No sparkline: the free tier's candle
// endpoint isn't available without a paid plan, so Finnhub symbols show price
// + change only.
static bool parseFinnhubQuote(const Settings& s, StockData& d, Stream& stream) {
  JsonDocument filter;
  filter["c"] = true;
  filter["d"] = true;
  filter["dp"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  if (!(doc["c"].is<float>() || doc["c"].is<int>())) return false;
  float price = doc["c"].as<float>();
  if (isnan(price) || price <= 0) return false;   // Finnhub returns all-zero for an unknown symbol
  d.price = price;

  if (doc["dp"].is<float>() || doc["dp"].is<int>()) {
    d.changePct = doc["dp"].as<float>();
    d.change = doc["d"] | 0.0f;
    d.hasChange = true;
  } else {
    d.hasChange = false;
  }

  if (!d.userNamed && !d.name[0]) strlcpy(d.name, d.symbol, MAX_NAME_LEN);   // Finnhub's quote has no company name

  String rl = s.ticker.range;
  rl.toUpperCase();
  strlcpy(d.rangeLabel, rl.c_str(), sizeof(d.rangeLabel));

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// Binance /fapi/v1/ticker/24hr — all the numeric fields arrive as JSON
// *strings* (Binance convention across their whole API), same as cash.ch
// above: read as text and atof(), not as numbers.
static bool parseBinanceQuote(const Settings& s, StockData& d, Stream& stream) {
  JsonDocument filter;
  filter["lastPrice"] = true;
  filter["priceChange"] = true;
  filter["priceChangePercent"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  const char* lp = doc["lastPrice"] | "";
  if (!lp[0]) return false;   // e.g. {"code":-1121,"msg":"Invalid symbol."} for a typo'd symbol
  float price = atof(lp);
  if (isnan(price) || price <= 0) return false;
  d.price = price;

  const char* pct = doc["priceChangePercent"] | "";
  if (pct[0]) {
    d.changePct = atof(pct);
    d.change = atof(doc["priceChange"] | "0");
    d.hasChange = true;
  } else {
    d.hasChange = false;
  }

  if (!d.userNamed && !d.name[0]) strlcpy(d.name, d.symbol, MAX_NAME_LEN);   // Binance's ticker has no company name
  if (!d.currency[0]) strlcpy(d.currency, "$", sizeof(d.currency));          // USDT-margined ~= USD for display

  String rl2 = s.ticker.range;
  rl2.toUpperCase();
  strlcpy(d.rangeLabel, rl2.c_str(), sizeof(d.rangeLabel));

  d.valid = true;
  d.error = false;
  d.lastOkMs = millis();
  return true;
}

// Binance /fapi/v1/klines — each candle is a 12-element array; index 4 is the
// close price (as a string, same convention as the quote endpoint). No
// filter here — ArduinoJson's array filters are documented for object keys
// far more clearly than for picking one column out of an array-of-arrays,
// and an untested filter shape failing silently (candles kept but empty) is
// a worse failure mode than just parsing the whole row and indexing into it.
// The request itself stays small (limit=20 above) to keep this cheap anyway.
static bool parseBinanceKlines(StockData& d, Stream& stream) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, stream);
  if (err) return false;

  JsonArrayConst arr = doc.as<JsonArrayConst>();
  if (arr.isNull()) return false;

  d.sparkCount = 0;
  for (JsonArrayConst k : arr) {
    if (d.sparkCount >= MAX_SPARK_POINTS) break;
    const char* close = k[4] | "";
    if (!close[0]) continue;
    d.spark[d.sparkCount++] = atof(close);
  }
  return d.sparkCount > 0;
}

// ---- parse: cash.ch daily-close series --------------------------------------
// {"data":{"integration":{"solid":{"chart":{"timeserie":{"prices":
//  [{"close":998.45},...]}}}}}} — closes are real JSON numbers here (unlike
// the quote), oldest -> newest. Downsampling mirrors the Yahoo parser.
static bool parseCashChart(const Settings& s, StockData& d, Stream& stream) {
  JsonDocument filter;
  filter["data"]["integration"]["solid"]["chart"]["timeserie"]["prices"][0]["close"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, stream, DeserializationOption::Filter(filter));
  if (err) return false;

  JsonArrayConst prices =
      doc["data"]["integration"]["solid"]["chart"]["timeserie"]["prices"];
  if (prices.isNull()) return false;         // unknown key / empty series

  uint16_t want = s.ticker.points;
  if (want > MAX_SPARK_POINTS) want = MAX_SPARK_POINTS;
  if (want < 2) return true;                 // chart not wanted; keep quote data

  uint16_t valid = 0;
  for (JsonObjectConst p : prices)
    if (p["close"].is<float>() || p["close"].is<int>()) valid++;
  if (valid < 2) return false;               // keep the previous sparkline

  d.sparkCount = 0;
  if (valid <= want) {
    for (JsonObjectConst p : prices) {
      if (!p["close"].is<float>() && !p["close"].is<int>()) continue;
      if (d.sparkCount >= MAX_SPARK_POINTS) break;
      d.spark[d.sparkCount++] = p["close"].as<float>();
    }
  } else {
    uint16_t i = 0, k = 0;
    float last = 0;
    for (JsonObjectConst p : prices) {
      if (!p["close"].is<float>() && !p["close"].is<int>()) continue;
      last = p["close"].as<float>();
      // k-th output maps to valid-index round(k*(valid-1)/(want-1)).
      uint16_t target =
          (uint16_t)(((uint32_t)k * (valid - 1) + (want - 1) / 2) / (want - 1));
      if (i == target && k < want) { d.spark[d.sparkCount++] = last; k++; }
      i++;
    }
    if (d.sparkCount > 0) d.spark[d.sparkCount - 1] = last;  // pin newest
  }
  return true;
}

// ---- one HTTP(S) GET + parse ----------------------------------------------
enum ParseKind : uint8_t { PARSE_WEBHOOK, PARSE_YAHOO, PARSE_CASH_QUOTE, PARSE_CASH_CHART, PARSE_GITHUB, PARSE_FINNHUB, PARSE_YAHOO_QUOTE, PARSE_BINANCE, PARSE_BINANCE_KLINES };

// cash.ch is the only host we let negotiate ECDHE, and only the first handshake
// pays for it: this session resumes the rest for ~23 h.
static TlsSession g_cashSession;

static bool fetchUrl(const Settings& s, const String& url, ParseKind kind, StockData& d) {
  bool https = url.startsWith("https://");
  bool cash = (kind == PARSE_CASH_QUOTE || kind == PARSE_CASH_CHART);
  bool finnhub = (kind == PARSE_FINNHUB);
  bool binance = (kind == PARSE_BINANCE || kind == PARSE_BINANCE_KLINES);

  std::unique_ptr<NetClient> client;
  if (https) {
    // Per-source TLS shaping (ESP8266; the ESP32 ignores the hints):
    //  - cash.ch: must do ECDHE. Keep it cheap — 512 B buffers (it honors MFLN)
    //    and session resumption, and require a large CONTIGUOUS free block up
    //    front so a fragmented heap skips the fetch instead of crashing inside
    //    the handshake. The full cipher list (default) is left on for it.
    //  - finnhub.io / fapi.binance.com: also need the full (ECDHE-capable)
    //    cipher list — like most current API hosts they don't offer the cheap
    //    static-RSA suites the block below relies on. No MFLN assumption or
    //    session resumption (unlike cash.ch) since we haven't confirmed either
    //    honors it; sized generously instead (see the weather feature's
    //    Open-Meteo bug for what happens when this buffer is too small).
    //  - Yahoo / GitHub / webhook: forced to the cheap static-RSA suites, so
    //    those handshakes stay as light as the old BASIC build.
    if (cash) {
      if (platformMaxFreeBlock() < 16000) { d.dbgLastHttpCode = -1000; return false; }   // largest contiguous block, not total
      client.reset(platformMakeSecureClient(512, &g_cashSession, 512, /*cheapCiphers=*/false));
    } else if (finnhub || binance) {
      if (platformMaxFreeBlock() < 16000) { d.dbgLastHttpCode = -1000; return false; }
      client.reset(platformMakeSecureClient(5120, nullptr, 512, /*cheapCiphers=*/false));
    } else {
      // raw.githubusercontent.com sends a ~4 KB cert record and won't negotiate
      // MFLN, so it needs a bigger receive buffer than Yahoo's small records.
      uint16_t rx = (kind == PARSE_GITHUB) ? GH_QUOTES_RXBUF : 2048;
      if (ESP.getFreeHeap() < (uint32_t)rx + 12000) { d.dbgLastHttpCode = -1000; return false; }
      client.reset(platformMakeSecureClient(rx, nullptr, 512, /*cheapCiphers=*/true));
    }
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  // HTTP/1.0 so the server can't reply with chunked framing: the parsers read
  // the raw stream via getStream(), which neither core de-chunks. Yahoo chunks
  // its HTTP/1.1 responses, which broke Yahoo tickers on the ESP32 targets.
  http.useHTTP10(true);
  if (!http.begin(*client, url)) { d.dbgLastHttpCode = -900; return false; }
  http.addHeader("Accept", "application/json");
  if (kind == PARSE_YAHOO) {
    http.setUserAgent(F(YAHOO_USER_AGENT));   // empty UA => HTTP 429 from Yahoo
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  } else if (kind == PARSE_GITHUB) {
    http.setUserAgent(F(FW_NAME));            // GitHub rejects an empty UA
  } else if (kind == PARSE_FINNHUB) {
    http.setUserAgent(F(FW_NAME));
  } else if (kind == PARSE_BINANCE) {
    http.setUserAgent(F(FW_NAME));
  } else if (kind == PARSE_BINANCE_KLINES) {
    http.setUserAgent(F(FW_NAME));
  } else if (kind != PARSE_WEBHOOK) {
    http.setUserAgent(F(CASH_USER_AGENT));    // cash.ch requires none; sent to be identifiable
  }

  int code = http.GET();
  d.dbgLastHttpCode = (int16_t)code;   // diagnostic: surfaced via /api/status regardless of source
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  bool ok;
  switch (kind) {
    case PARSE_YAHOO:      ok = parseYahoo(s, d, http.getStream());     break;
    case PARSE_CASH_QUOTE: ok = parseCashQuote(s, d, http.getStream()); break;
    case PARSE_CASH_CHART: ok = parseCashChart(s, d, http.getStream()); break;
    case PARSE_FINNHUB:    ok = parseFinnhubQuote(s, d, http.getStream()); break;
    case PARSE_BINANCE:    ok = parseBinanceQuote(s, d, http.getStream()); break;
    case PARSE_BINANCE_KLINES: ok = parseBinanceKlines(d, http.getStream()); break;
    default:               ok = parseWebhook(s, d, http.getStream());   break;  // webhook + github: same JSON
  }
  if (!ok) d.dbgLastHttpCode = -800;   // 200 OK but the body didn't parse the way this source expects
  http.end();
  return ok;
}

// v7/finance/quote overlay fetch — see config.h's YAHOO_QUOTE_HOST comment
// and applyYahooQuoteExt() above. Kept as its own small function (rather than
// routed through fetchUrl()'s generic dispatch) since a miss here is never an
// error worth setting d.error for — `d` already has a valid regular-session
// price before this runs.
static void fetchYahooQuoteExt(const Settings& s, StockData& d) {
  if (platformMaxFreeBlock() < 16000) { d.dbgQuoteHttpCode = -1000; return; }
  std::unique_ptr<NetClient> client(platformMakeSecureClient(2048));

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(*client, buildYahooQuoteUrl(YAHOO_QUOTE_HOST1, d.symbol))) { d.dbgQuoteHttpCode = -900; return; }
  http.addHeader("Accept", "application/json");
  http.setUserAgent(F(YAHOO_USER_AGENT));

  int code = http.GET();
  d.dbgQuoteHttpCode = (int16_t)code;   // diagnostic: surfaced via /api/status regardless of outcome
  if (code != HTTP_CODE_OK) { http.end(); return; }   // 401/403/999 etc: Yahoo blocked it — silently skip
  applyYahooQuoteExt(d, http.getStream());
  http.end();
}

// ---- fetch one symbol, one network request per call -----------------------
// A symbol may need several requests (Yahoo mirror retry; cash.ch quote, its
// retry, then the chart). Each is a separate step so only ONE TLS handshake
// runs per service() call: a full-BearSSL ECDHE handshake takes ~1 s on the
// ESP8266, and chaining two or three in one loop() iteration starved the web
// server and tripped the watchdog. g_fetchPhase tracks the step for g_fetchIdx.
static uint8_t g_fetchPhase = 0;

// Returns true when this symbol is finished (caller advances to the next).
static bool stepSymbol(const Settings& s, StockData& d) {
  if (d.source == SRC_YAHOO) {
    // Off-hours hand-off to Binance (SymbolCfg.altSymbol) — see the US-hours
    // helper above and config.h's BINANCE_HOST comment. Single-phase, like
    // SRC_BINANCE below: skips the whole Yahoo chart/quote dance entirely
    // while the US market's closed, and resumes it automatically (fresh
    // sparkline included) the moment usMarketRegularHoursNow() flips back.
    if (d.altSymbol[0] && !usMarketRegularHoursNow()) {
      bool ok = fetchUrl(s, buildBinanceUrl(d.altSymbol), PARSE_BINANCE, d);
      if (ok) { d.extHours = true; strlcpy(d.extLabel, "24/7", sizeof(d.extLabel)); }
      else    d.error = true;
      return true;
    }
    if (g_fetchPhase == 0 || g_fetchPhase == 1) {
      const char* host = (g_fetchPhase == 0) ? YAHOO_CHART_HOST1 : YAHOO_CHART_HOST2;
      if (!fetchUrl(s, buildYahooUrl(s, host, d.symbol), PARSE_YAHOO, d)) {
        if (g_fetchPhase == 0) { g_fetchPhase = 1; return false; }   // transient drop: retry the mirror next tick
        d.error = true;
        return true;
      }
      // Holiday check, without hardcoding a calendar: our hours math above
      // said "should be open", but if regularMarketTime — just refreshed by
      // the fetch above — is still >18h old, today's session demonstrably
      // hasn't produced a fresh trade. Rechecked on every cycle (not just
      // once), so the moment a real session actually opens this notices
      // within one poll interval, rather than getting stuck showing Binance
      // for the rest of the day once a holiday is (correctly) detected.
      bool stale = d.regularMarketTime &&
                   ((uint32_t)time(nullptr) - d.regularMarketTime) > 18UL * 3600UL;
      if (d.altSymbol[0] && stale) { g_fetchPhase = 3; return false; }
      g_fetchPhase = 2;
      return false;
    }
    if (g_fetchPhase == 3) {   // holiday hand-off — see the staleness check above
      bool ok = fetchUrl(s, buildBinanceUrl(d.altSymbol), PARSE_BINANCE, d);
      if (ok) { d.extHours = true; strlcpy(d.extLabel, "24/7", sizeof(d.extLabel)); }
      // else: leave Yahoo's (stale but valid) price in place rather than erroring —
      // still better than nothing, and next cycle tries fresh Yahoo data again anyway.
      return true;
    }
    // phase 2: best-effort extended-hours overlay (see config.h's
    // YAHOO_QUOTE_HOST comment) — a separate tick, same one-handshake-per-call
    // rule as cash.ch's phases above. Failure here is silent and non-fatal:
    // `d` already has a valid regular-session price from phase 0/1.
    fetchYahooQuoteExt(s, d);
    return true;
  }

  if (d.source == SRC_CASH) {
    if (g_fetchPhase == 0) {            // quote: price + day change (~200 B)
      if (fetchUrl(s, buildCashQuoteUrl(d.symbol), PARSE_CASH_QUOTE, d)) { g_fetchPhase = 2; return false; }
      g_fetchPhase = 1;                 // retry the quote next tick
      return false;
    }
    if (g_fetchPhase == 1) {
      if (!fetchUrl(s, buildCashQuoteUrl(d.symbol), PARSE_CASH_QUOTE, d)) { d.error = true; return true; }
      g_fetchPhase = 2;
      return false;
    }
    // phase 2: sparkline series; its failure is non-fatal (stale chart beats
    // none). Also fetched with the chart hidden when the range-based change
    // needs the series' first point — but not when nothing on the device
    // would show it (cash TLS requests are expensive on the ESP8266).
    if ((s.ticker.showChart || (s.ticker.changeOnRange && s.ticker.showChange)) &&
        s.ticker.points >= 2)
      fetchUrl(s, buildCashChartUrl(s, d.symbol), PARSE_CASH_CHART, d);
    return true;
  }

  if (d.source == SRC_GHUB) {           // static per-symbol JSON from the repo
    if (!fetchUrl(s, buildGithubUrl(d.symbol), PARSE_GITHUB, d)) d.error = true;
    return true;
  }

  if (d.source == SRC_FINNHUB) {
    if (s.ticker.finnhubKey.length() < 4) { d.error = true; return true; }   // no key set yet
    if (!fetchUrl(s, buildFinnhubUrl(s, d.symbol), PARSE_FINNHUB, d)) d.error = true;
    return true;
  }

  if (d.source == SRC_BINANCE) {
    if (g_fetchPhase == 0) {
      if (!fetchUrl(s, buildBinanceUrl(d.symbol), PARSE_BINANCE, d)) { d.error = true; return true; }
      g_fetchPhase = 1;
      return false;
    }
    // phase 1: sparkline; failure is non-fatal (no chart beats none, same as
    // cash.ch's phase 2 above), and skipped entirely when nothing would show
    // it — a Binance TLS request isn't free on this chip.
    if (s.ticker.showChart && s.ticker.points >= 2)
      fetchUrl(s, buildBinanceKlinesUrl(s, d.symbol), PARSE_BINANCE_KLINES, d);
    return true;
  }

  if (s.ticker.webhookUrl.length() < 8) { d.error = true; return true; }
  if (!fetchUrl(s, buildWebhookUrl(s, d.symbol), PARSE_WEBHOOK, d)) d.error = true;
  return true;
}

// How long until this symbol is fetched again. A good fetch waits the poll
// interval; a failed one comes back on a short, doubling backoff (typically a
// cash fetch skipped because the heap was momentarily too fragmented for TLS,
// which recovers within seconds). The backoff is capped at the poll interval so
// a genuinely dead symbol settles into the normal cadence and keeps retrying
// there — it is never given up on.
static uint32_t symbolPeriodMs(const Settings& s, const StockData& d) {
  uint32_t poll = (uint32_t)s.ticker.pollSec * 1000UL;
  if (!d.error || d.fails > TICKER_RETRY_MAX) return poll;   // ladder spent -> normal cadence
  uint32_t back = (uint32_t)TICKER_RETRY_SEC * 1000UL;
  for (uint8_t i = 1; i < d.fails; i++) back <<= 1;          // 12 s, 24, 48, 96
  return back < poll ? back : poll;                          // never slower than a good fetch
}

// ---------------------------------------------------------------------------
// Shared USD/KRW rate for Settings.ticker.showKrw (see StockClient.h). Reuses
// the Yahoo chart plumbing above with a private StockData that is never one
// of the user's configured symbols — just a place to land KRW=X's price.
static StockData g_fx;
static bool      g_fxInited = false;
static uint32_t  g_fxNextMs = 0;

float fxUsdKrw() { return g_fx.valid ? g_fx.price : 0.0f; }

void fxService(const Settings& s) {
  if (!s.ticker.showKrw) return;
  if (g_refreshing) return;   // don't stack a 2nd TLS handshake onto an in-progress symbol refresh this tick
  if (!g_fxInited) {
    g_fx.clear();
    strlcpy(g_fx.symbol, "KRW=X", sizeof(g_fx.symbol));
    g_fxInited = true;
    g_fxNextMs = millis();
  }
  if ((int32_t)(millis() - g_fxNextMs) < 0) return;
  g_fxNextMs = millis() + 30UL * 60UL * 1000UL;   // 30 min — a spot rate doesn't need to be fresher

  if (!fetchUrl(s, buildYahooUrl(s, YAHOO_CHART_HOST1, "KRW=X"), PARSE_YAHOO, g_fx))
    fetchUrl(s, buildYahooUrl(s, YAHOO_CHART_HOST2, "KRW=X"), PARSE_YAHOO, g_fx);   // one mirror retry, same tick's worth of budget as a symbol gets
}

// True once the symbol's own due time has passed (millis()-safe subtraction).
static bool symbolDue(const StockData& d) {
  return (int32_t)(millis() - d.nextTryMs) >= 0;
}

// ---------------------------------------------------------------------------
void stocksService(const Settings& s) {
  if (g_count == 0) return;

  // A pass starts as soon as ANY symbol is due, and visits only the symbols
  // that are due. A failing ticker on its 12 s backoff therefore no longer
  // drags the healthy ones through an extra TLS handshake each time round.
  if (!g_refreshing) {
    uint8_t first = g_count;
    for (uint8_t i = 0; i < g_count; i++)
      if (symbolDue(g_stocks[i])) { first = i; break; }
    if (first >= g_count) return;
    g_refreshing = true;
    g_fetchIdx = first;
    g_fetchPhase = 0;
  }

  while (g_fetchIdx < g_count && !symbolDue(g_stocks[g_fetchIdx])) g_fetchIdx++;

  // One network request per call so net/web/display keep getting serviced
  // between the (slow, on the ESP8266) TLS handshakes.
  if (g_fetchIdx < g_count) {
    StockData& d = g_stocks[g_fetchIdx];
    if (stepSymbol(s, d)) {          // symbol finished -> schedule it, move on
      if (d.error) { if (d.fails < 250) d.fails++; }
      else         { d.fails = 0; }
      d.nextTryMs = millis() + symbolPeriodMs(s, d);
      g_fetchIdx++;
      g_fetchPhase = 0;
    }
    return;
  }

  g_refreshing = false;
}

bool tickerNeedsClock(const Settings& s) {
  for (uint8_t i = 0; i < s.ticker.symbolCount; i++)
    if (s.ticker.symbols[i].source == SRC_YAHOO && s.ticker.symbols[i].altSymbol[0]) return true;
  return false;
}