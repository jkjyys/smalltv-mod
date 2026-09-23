#include "OtaUpdate.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
#include <HTTPUpdate.h>
#endif

#if defined(SMALLTV_ESP8266)
// Prefer MFLN so BearSSL can run with a tiny buffer; fall back to 4 KB.
static uint16_t probeMfln(const char* host) {
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 512))  return 512;
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 1024)) return 1024;
  return 4096;
}
#endif

// "a.b.c" -> a*10000 + b*100 + c, for a simple newer-than comparison.
static long verNum(const char* v) {
  int a = 0, b = 0, c = 0;
  sscanf(v, "%d.%d.%d", &a, &b, &c);
  return (long)a * 10000 + (long)b * 100 + c;
}

OtaLatest otaCheckLatest(const Settings& s) {
  OtaLatest r;
  if (ESP.getFreeHeap() < 20000) { r.error = F("low heap"); return r; }

  String url = F("https://");
  url += F(GH_API_HOST);
  url += F("/repos/");
  url += F(REPO_OWNER);
  url += "/";
  url += F(REPO_NAME);
  url += F("/releases/latest");

  // GitHub over TLS on this chip occasionally stalls a stream read (truncated
  // JSON -> "parse failed") or drops the connection; a couple of quick retries
  // clear the transient. A 403 with the rate-limit budget exhausted is NOT
  // retryable — surface a clear message so the user waits instead of hammering
  // the API (which is what turns an occasional hiccup into a persistent failure).
  const int kAttempts = 3;
  for (int attempt = 1; attempt <= kAttempts; attempt++) {
    r.tag = ""; r.url = ""; r.newer = false;   // clear partial state from any prior attempt
    bool retryable = false;

    SecureClient client;
    client.setInsecure();
#if defined(SMALLTV_ESP8266)
    client.setBufferSizes(probeMfln(GH_API_HOST), 512);
#endif

    HTTPClient http;
    // A stalled stream truncates into a "parse failed"; the retries below clear
    // that, so keep the per-attempt timeout modest to stay responsive (this runs
    // in the ESP32 web handler) rather than blocking long on each failing try.
    http.setTimeout(s.httpTimeout);
    http.setReuse(false);
    http.setUserAgent(F(FW_NAME));                 // GitHub rejects requests with no UA
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    // HTTP/1.0 forbids chunked responses. The body is parsed straight off
    // getStream(), which neither core de-chunks (same fix as StockClient v2.4.1).
    http.useHTTP10(true);
    const char* hdrKeys[] = { "x-ratelimit-remaining" };
    http.collectHeaders(hdrKeys, 1);

    if (!http.begin(client, url)) {
      r.error = F("connect failed"); retryable = true;
    } else {
      http.addHeader("Accept", "application/vnd.github+json");
      int code = http.GET();
      if (code == 403 && http.header("x-ratelimit-remaining") == "0") {
        r.error = F("GitHub rate limit, try again later");   // not retryable
      } else if (code != HTTP_CODE_OK) {
        r.error = "HTTP " + String(code);
        retryable = (code >= 500);                            // server-side -> transient
      } else {
        // Keep only the fields we need; the releases payload is large.
        JsonDocument filter;
        filter["tag_name"] = true;
        JsonObject fa = filter["assets"][0].to<JsonObject>();
        fa["name"] = true;
        fa["browser_download_url"] = true;

        JsonDocument doc;
        DeserializationError err =
            deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        if (err) {
          r.error = F("parse failed"); retryable = true;      // truncated/stalled stream
        } else {
          r.tag = (const char*)(doc["tag_name"] | "");
          for (JsonObjectConst a : doc["assets"].as<JsonArrayConst>()) {
            if (strcmp(a["name"] | "", UPDATE_ASSET) == 0) {
              r.url = (const char*)(a["browser_download_url"] | "");
              break;
            }
          }
          if (r.tag.length() == 0 || r.url.length() == 0) {
            r.error = F("no matching asset");                 // not retryable
          } else {
            String latest = r.tag;
            if (latest.startsWith("v")) latest.remove(0, 1);
            r.newer = verNum(latest.c_str()) > verNum(FW_VERSION);
            r.error = "";
            r.ok = true;
          }
        }
      }
      http.end();
    }

    if (r.ok || !retryable) return r;
    if (attempt < kAttempts) delay(500);           // brief backoff before the next try
  }
  return r;   // r.error holds the last (retryable) error after all attempts
}

String otaUpdateFromGitHub(const Settings& s) {
#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
  OtaLatest r = otaCheckLatest(s);
  if (!r.ok) return "check failed: " + r.error;
  if (!r.newer) return "already up to date (" FW_VERSION ")";
  if (ESP.getFreeHeap() < 22000) return F("not enough free heap for a TLS update");

  // mbedTLS manages its own buffers; each target pulls its own release asset
  // (UPDATE_ASSET in config.h). The two-slot OTA layout makes this atomic, so a
  // failed/interrupted download just leaves the running image untouched — retry
  // once on a transient stream stall before giving up.
  String lastErr;
  for (int attempt = 1; attempt <= 2; attempt++) {
    SecureClient client;
    client.setInsecure();

    HTTPUpdate up;
    up.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    up.rebootOnUpdate(true);

    t_httpUpdate_return ret = up.update(client, r.url);
    switch (ret) {
      case HTTP_UPDATE_OK:         return "";                     // reboots into the new image
      case HTTP_UPDATE_NO_UPDATES: return F("server reported no update");
      case HTTP_UPDATE_FAILED:     lastErr = up.getLastErrorString(); break;
    }
    if (attempt < 2) delay(1000);
  }
  return "download failed after retry: " + lastErr;
#else
  (void)s;
  return F("internal error: the ESP8266 updates at boot");   // WebPortal never calls this here
#endif
}

// ---- update-at-boot (ESP8266) ----------------------------------------------
// The asset download used to just assume it needed a full 16 KB BearSSL
// receive buffer (github.com and the release-asset CDN "probably" don't
// negotiate MFLN) -- inherited, never actually measured on this device. A
// small-buffer-first attempt sized off an unrelated host (raw.githubusercontent
// .com's quotes traffic, see GH_QUOTES_RXBUF) didn't hold up for the release
// CDN in practice. Below, the real download host is resolved and MFLN-probed
// live instead (see resolveDownloadTarget/probeMfln), so the small buffer this
// tries first is a server-acknowledged number, not another guess -- with the
// original, proven 16 KB path kept as an unconditional fallback so this still
// can't make a working device worse. The web UI queues the request in
// LittleFS and reboots; this runs early in setup() with the heap still free.
// The request is consumed BEFORE the attempt, so a crash or failure can never
// boot-loop.
#if defined(SMALLTV_ESP8266)
static const char* OTA_REQ_PATH = "/ota.req";
static const char* OTA_MSG_PATH = "/ota.msg";

// Authority (host[:port]) of a "scheme://host[:port]/path" URL -- good enough
// for the https:// URLs this file deals with.
static void hostFromUrl(const String& url, char* out, size_t n) {
  int start = url.indexOf("://");
  start = (start < 0) ? 0 : start + 3;
  int end = url.indexOf('/', start);
  if (end < 0) end = url.length();
  strlcpy(out, url.substring(start, end).c_str(), n);
}

// GitHub's release-asset URL (browser_download_url, itself on github.com)
// redirects once to a signed, time-limited CDN URL -- historically something
// under githubusercontent.com, but that's changed before and isn't worth
// hardcoding, let alone assuming it behaves like raw.githubusercontent.com
// (a different host doing different traffic). That CDN is where the actual
// multi-hundred-KB firmware transfer happens, so it's the host whose TLS
// record size actually matters for buffer sizing. Resolve it with a request
// small enough to always afford -- github.com's own redirect response has
// next to no body -- then hand the caller the real host to MFLN-probe.
// Returns false only when even this small preliminary request couldn't
// connect at all; the caller's own 16 KB/original-URL fallback covers that.
//
// Chases up to 3 hops rather than assuming exactly one: github.com's own
// redirect chain for a release asset has been just the one hop in practice,
// but that's an observation, not a contract GitHub has made, and this file's
// own history is "it changed CDN once already" (see the comment above). One
// hop was silently assumed here before — if a future chain adds a second
// redirect, the resolved attempt would probe/connect to an intermediate
// host instead of the real download host, ESPhttpUpdate.update() would then
// see a 3xx it's told not to follow (forceRedirect=false), fail, and that
// failure used to get thrown away entirely whenever the 16 KB fallback also
// failed its own heap check — see the per-attempt `errs` accumulation in
// otaBootUpdate() below, added for exactly this kind of silently-swallowed
// failure.
static bool resolveDownloadTarget(const Settings& s, const String& url,
                                   String& outUrl, char* outHost, size_t hostLen) {
  outUrl = url;
  for (uint8_t hop = 0; hop < 3; hop++) {
    char host[80];
    hostFromUrl(outUrl, host, sizeof(host));

    BearSSL::WiFiClientSecure client;
    client.setInsecure();
    client.setBufferSizes(probeMfln(host), 512);

    HTTPClient http;
    http.setTimeout(s.httpTimeout);
    http.setUserAgent(F(FW_NAME));
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);   // chased by hand, one hop per loop
    if (!http.begin(client, outUrl)) return false;
    int code = http.GET();
    bool ok = (code > 0);                                       // a real response, whatever its status
    bool redirected = (code >= 300 && code < 400 && http.getLocation().length() > 0);
    if (redirected) outUrl = http.getLocation();
    http.end();

    if (!ok) return false;
    if (!redirected) { hostFromUrl(outUrl, outHost, hostLen); return true; }
    // else: loop once more against the new outUrl
  }
  return false;   // too many hops — give up; caller's 16 KB fallback still runs
}

bool otaBootRequested() { return LittleFS.exists(OTA_REQ_PATH); }

bool otaRequestBootUpdate(const char* tag) {
  File f = LittleFS.open(OTA_REQ_PATH, "w");
  if (!f) return false;                     // storage full/broken -> caller must not reboot
  f.print(tag ? tag : "");
  f.close();
  return true;
}

static void otaBootResult(const String& msg) {
  File f = LittleFS.open(OTA_MSG_PATH, "w");
  if (f) { f.print(msg); f.close(); }
}

String otaTakeBootResult() {
  if (!LittleFS.exists(OTA_MSG_PATH)) return String();
  File f = LittleFS.open(OTA_MSG_PATH, "r");
  String m = f ? f.readString() : String();
  if (f) f.close();
  LittleFS.remove(OTA_MSG_PATH);
  return m;
}

void otaBootUpdate(const Settings& s) {
  LittleFS.remove(OTA_REQ_PATH);            // consume first: one attempt per request
  if (WiFi.status() != WL_CONNECTED) { otaBootResult(F("no WiFi at boot")); return; }

  OtaLatest r = otaCheckLatest(s);          // re-resolve the asset URL fresh
  if (!r.ok)    { otaBootResult("check failed: " + r.error); return; }
  if (!r.newer) { otaBootResult(F("already up to date (" FW_VERSION ")")); return; }

  ESPhttpUpdate.rebootOnUpdate(true);

  // Resolve the release asset's real download host (the browser_download_url
  // itself just 302s to a signed CDN URL — see resolveDownloadTarget above)
  // and MFLN-probe THAT host live, so the small buffer tried first is a
  // number the server actually acknowledged rather than a guess borrowed from
  // an unrelated host (that's what v2.9.9's 5120-byte guess was, and it
  // didn't hold up against the real CDN). If resolution itself fails for any
  // reason, skip straight to the unconditional fallback below. Either way,
  // the original, proven 16 KB / unresolved-URL / force-redirect path always
  // runs last, so this change can only let a tight-heap device through that
  // the old code would have failed anyway — never make a working device
  // worse.
  struct OtaAttempt { uint32_t rxBuf; String url; bool forceRedirect; const char* tag; };
  OtaAttempt attempts[2];
  uint8_t n = 0;

  // Kept even on success so a failure below can say plainly "resolve itself
  // never worked" instead of silently only ever showing the fallback's error
  // (see the accumulated `errs` below — that ambiguity is exactly what made
  // v2.9.15's real "still fails sometimes" boil down to a bare heap number
  // with no way to tell which of the two attempts, or which failure mode,
  // actually produced it).
  bool resolveOk = false;
  String resolvedUrl;
  char resolvedHost[80] = {0};
  if (resolveDownloadTarget(s, r.url, resolvedUrl, resolvedHost, sizeof(resolvedHost)) &&
      resolvedHost[0]) {
    resolveOk = true;
    attempts[n].rxBuf        = probeMfln(resolvedHost);
    attempts[n].url          = resolvedUrl;
    attempts[n].forceRedirect = false;   // already resolved by hand above
    attempts[n].tag           = "resolved";
    n++;
  }
  attempts[n].rxBuf         = 16384;
  attempts[n].url           = r.url;
  attempts[n].forceRedirect = true;      // let the client itself chase the redirect
  attempts[n].tag           = "fallback";
  n++;

  // Every attempt's outcome, not just the last one — otherwise a fallback
  // heap-check failure (which needs the biggest contiguous block of the two,
  // so it's the one most likely to fail) silently overwrites whatever the
  // small resolved-buffer attempt actually hit, and a boot-only report with
  // no serial console has no other way to see that.
  //
  // Built with snprintf into a fixed stack buffer, NOT chained Arduino
  // String concatenation (`a + b + c + ...`). This function's very first
  // caller of `record()` below can be the "not enough heap" branch — i.e.
  // every one of these messages can be built at the exact moment the heap
  // has just been found critically low/fragmented. Each `+` on a String
  // allocates a new heap block for its temporary result; chaining several
  // of them (as this used to) right when allocation is already failing
  // risks the allocator itself, not just this diagnostic, and a device that
  // was already landing in this exact low-heap branch (see the "not enough
  // heap even at boot" reports this was added to explain) is exactly where
  // that risk stops being theoretical. snprintf only ever writes into a
  // buffer that's already on the stack, so it can't fail the same way.
  char errs[300] = {0};
  size_t errsLen = 0;
  auto record = [&errs, &errsLen](const char* tag, uint32_t rxBuf, const char* err) {
    int avail = (int)sizeof(errs) - (int)errsLen;
    if (avail <= 1) return;
    int n = snprintf(errs + errsLen, avail, "%s%s (%uB): %s",
                      errsLen ? "; " : "", tag, (unsigned)rxBuf, err);
    if (n > 0) errsLen += (n < avail - 1) ? (size_t)n : (size_t)(avail - 1);
  };
  if (!resolveOk) record("resolve", 0, "could not resolve/probe the real download host, skipped");

  for (uint8_t i = 0; i < n; i++) {
    uint32_t rxBuf = attempts[i].rxBuf;
    // rx + tx buffers plus BearSSL engine/stack-thunk overhead.
    const uint32_t need    = rxBuf + 512 + 8000;
    const uint32_t needBlk = rxBuf + 1024;
    // Total free heap can clear `need` while the heap is fragmented enough
    // that no single block is big enough — WiFi association and the check
    // above's own HTTPS request(s) each leave short-lived allocations behind.
    // A few delay()s (which service the WiFi/lwIP stack) give those a moment
    // to be freed and the allocator a moment to coalesce; cheap, and it's one
    // throwaway boot attempt either way if it doesn't help. Was 5*200ms —
    // widened to 12*250ms (3s worst case) since the observed failures were
    // sitting close to the line (largest block a few KB short of `needBlk`),
    // the kind of gap a little more coalescing time plausibly closes, and
    // 3s once at boot, only when already about to fail outright, costs
    // nothing on the normal/no-update-pending path.
    for (int tries = 0; tries < 12; tries++) {
      if (ESP.getFreeHeap() >= need && ESP.getMaxFreeBlockSize() >= needBlk) break;
      delay(250);
    }
    if (ESP.getFreeHeap() < need || ESP.getMaxFreeBlockSize() < needBlk) {
      char msg[110];
      snprintf(msg, sizeof(msg),
               "not enough heap (%u free, %u largest block, need %u free / %u contiguous)",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
               (unsigned)need, (unsigned)needBlk);
      record(attempts[i].tag, rxBuf, msg);
      continue;   // this size didn't even get to try — see if another attempt is left
    }

    BearSSL::WiFiClientSecure client;
    client.setInsecure();
    client.setBufferSizes(rxBuf, 512);
    ESPhttpUpdate.setFollowRedirects(attempts[i].forceRedirect
                                          ? HTTPC_FORCE_FOLLOW_REDIRECTS
                                          : HTTPC_DISABLE_FOLLOW_REDIRECTS);

    t_httpUpdate_return ret = ESPhttpUpdate.update(client, attempts[i].url);
    if (ret == HTTP_UPDATE_OK) return;                     // rebootOnUpdate restarts into the new image
    if (ret == HTTP_UPDATE_NO_UPDATES) { otaBootResult(F("server reported no update")); return; }
    // getLastErrorString() only runs after a real attempt was made (the heap
    // check above passed), so heap is no longer the knife-edge it is in the
    // branch above — a single bounded String copy here is the same risk the
    // rest of this file already accepts (e.g. every other otaBootResult call).
    record(attempts[i].tag, rxBuf, ESPhttpUpdate.getLastErrorString().c_str());
  }
  otaBootResult(String("download failed: ") + errs);
}
#else
bool   otaBootRequested() { return false; }
bool   otaRequestBootUpdate(const char*) { return false; }
void   otaBootUpdate(const Settings&) {}
String otaTakeBootResult() { return String(); }
#endif
