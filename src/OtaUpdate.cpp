#include "OtaUpdate.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
#include <HTTPUpdate.h>
#endif

#if defined(SMALLTV_ESP8266)
// Prefer MFLN so BearSSL can run with the smallest buffer the server actually
// agreed to. 512/1024/4096 are the only fragment lengths the MFLN extension
// (RFC 6066) defines, so all three get a real probe -- unlike the old version
// of this function, which tried 512 and 1024 and then just ASSUMED 4096 would
// work if neither did, without ever confirming the server would honor it.
// Used only for the small JSON GET in otaCheckLatest below, where the
// response is tiny either way, so getting the probe "wrong" would be
// harmless -- this just avoids allocating a bigger buffer than needed. The
// firmware download itself (otaBootUpdate) doesn't probe MFLN on the CDN
// host at all; see the comment above that function for why.
static uint16_t probeMfln(const char* host) {
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 512))  return 512;
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 1024)) return 1024;
  if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, 443, 4096)) return 4096;
  return 16384;
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
// Both attempts below go through github.com's own redirect
// (HTTPC_FORCE_FOLLOW_REDIRECTS -- HTTPClient follows the 3xx to the real,
// signed CDN URL itself) and differ only in RX buffer size. That's new as of
// this version: an earlier design hand-resolved the redirect first (its own
// GET to github.com, chased by hand) and then MFLN-probed the resolved CDN
// host directly, so the small-buffer attempt connected straight to that
// pre-resolved URL. On a live device that hand-resolved attempt failed the
// same way -- "Stream Read Timeout" or "connection lost", never a heap or
// buffer-size complaint -- six times running, surviving three different
// fixes in turn (verifying the negotiated size, retrying same-URL, even
// re-resolving a fresh URL immediately before each retry). Six identical
// failures across four different theories point at the one thing all of
// them shared and none of them tested: connecting directly to a pre-resolved
// CDN URL, bypassing github.com's own redirect entirely. This version stops
// doing that and lets ESPhttpUpdate/HTTPClient chase the redirect itself for
// BOTH attempts, the same as the always-worked 16 KB path always has --
// just with a smaller buffer tried first to fit this device's fragmented
// heap, instead of assuming the small buffer also means a hand-resolved
// direct connection. The web UI queues the request in LittleFS and reboots;
// this runs early in setup() with the heap still free. The request is
// consumed BEFORE the attempt, so a crash or failure can never boot-loop.
#if defined(SMALLTV_ESP8266)
static const char* OTA_REQ_PATH = "/ota.req";
static const char* OTA_MSG_PATH = "/ota.msg";

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

  // Both attempts below let ESPhttpUpdate/HTTPClient chase github.com's own
  // redirect to the real, signed CDN URL (see the file-level comment above
  // for why: six straight live failures on a hand-resolved direct-to-CDN
  // connection, across four falsified theories, pointed at that hand
  // resolution itself rather than at buffer size, heap, retries, or URL
  // freshness). They differ only in RX buffer size: "small" first, since
  // this device's heap is fragmented enough that the largest contiguous
  // block has never once reached the 16 KB path's requirement in testing;
  // "large" last, unconditionally, as the original proven size in case a
  // less-fragmented boot allows it. Either way this can only let a
  // tight-heap device through that used to fail outright — never make a
  // working device worse.
  struct OtaAttempt { uint32_t rxBuf; const char* tag; };
  OtaAttempt attempts[2] = {
    { 4096,  "small" },
    { 16384, "large" },
  };
  const uint8_t n = 2;

  // Every attempt's outcome, not just the last one — otherwise the "large"
  // attempt's heap-check failure (which needs the biggest contiguous block
  // of the two, so it's the one most likely to fail) silently overwrites
  // whatever the "small" attempt actually hit, and a boot-only report with
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

    // Retried once on a transient-looking error. Both tries hit the same
    // r.url and let ESPhttpUpdate follow github.com's redirect itself.
    String lastErr;
    for (uint8_t retry = 0; retry < 2; retry++) {
      if (retry) delay(300);

      BearSSL::WiFiClientSecure client;
      client.setInsecure();
      client.setBufferSizes(rxBuf, 512);
      ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

      t_httpUpdate_return ret = ESPhttpUpdate.update(client, r.url);
      if (ret == HTTP_UPDATE_OK) return;                   // rebootOnUpdate restarts into the new image
      if (ret == HTTP_UPDATE_NO_UPDATES) { otaBootResult(F("server reported no update")); return; }
      lastErr = ESPhttpUpdate.getLastErrorString();
      if (lastErr != "connection lost" &&
          !lastErr.startsWith("Update error: ERROR[6]")) {
        break;   // not the transient pattern above — a retry won't help, don't wait 300ms for nothing
      }
    }
    // getLastErrorString() only runs after a real attempt was made (the heap
    // check above passed), so heap is no longer the knife-edge it is in the
    // branch above — a single bounded String copy here is the same risk the
    // rest of this file already accepts (e.g. every other otaBootResult call).
    record(attempts[i].tag, rxBuf, lastErr.c_str());
  }
  otaBootResult(String("download failed: ") + errs);
}
#else
bool   otaBootRequested() { return false; }
bool   otaRequestBootUpdate(const char*) { return false; }
void   otaBootUpdate(const Settings&) {}
String otaTakeBootResult() { return String(); }
#endif
