#include "OtaUpdate.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
#include <HTTPUpdate.h>
#endif

#if defined(SMALLTV_ESP8266)
// Platform.h pulls in ESP8266httpUpdate.h for the ESPhttpUpdate global used
// below, but that header doesn't itself declare the Update global (only its
// own .cpp does) -- otaDownloadRanged() further down calls Update.begin()/
// write()/end() directly, so it needs this include explicitly rather than
// relying on it arriving transitively.
#include <Updater.h>
// Prefer MFLN so BearSSL can run with the smallest buffer the server actually
// agreed to. 512/1024/4096 are the only fragment lengths the MFLN extension
// (RFC 6066) defines, so all three get a real probe -- unlike the old version
// of this function, which tried 512 and 1024 and then just ASSUMED 4096 would
// work if neither did, without ever confirming the server would honor it.
// Used for the small JSON GET in otaCheckLatest below (response is tiny
// either way, so getting the probe "wrong" would be harmless -- this just
// avoids allocating a bigger buffer than needed), and reused by
// otaBootUpdate() below to size its own small-buffer attempt against the
// CDN host specifically -- see the big comment above that function.
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
// v2.9.22: an earlier design hand-resolved the redirect first (its own GET
// to github.com, chased by hand) and then MFLN-probed the resolved CDN host
// directly, so the small-buffer attempt connected straight to that
// pre-resolved URL. On a live device that hand-resolved attempt failed the
// same way -- "Stream Read Timeout" or "connection lost", never a heap or
// buffer-size complaint -- six times running, surviving three different
// fixes in turn (verifying the negotiated size, retrying same-URL, even
// re-resolving a fresh URL immediately before each retry). Six identical
// failures across four different theories point at the one thing all of
// them shared and none of them tested: connecting directly to a pre-resolved
// CDN URL, bypassing github.com's own redirect entirely. v2.9.22 stopped
// doing that and let ESPhttpUpdate/HTTPClient chase the redirect itself for
// BOTH attempts, the same as the always-worked 16 KB path always has.
//
// v2.9.24 (deferred mDNS/SNTP, see main.cpp) tested that heap theory on top
// of the v2.9.22 fix and, over two live runs, found: the 16 KB attempt's
// heap precheck still never passed (11640-14328 B largest block seen, never
// the 17408 B needed -- deferring mDNS/SNTP didn't move that number, so
// something else pins the heap that low the moment STA WiFi is up) -- AND,
// more importantly, the 4 KB attempt's heap precheck had *never once*
// failed, in any test, this whole session. The 4 KB path was never
// heap-blocked; freeing heap could never have fixed it. Its real failure
// both times was still a live-connection fault ("connection lost" once,
// "Update error: ERROR[6]" / Stream Read Timeout the other) *during the
// download itself*, after headers were already exchanged -- the same two
// symptoms v2.9.22's fix was tested against, just with the hand-resolution
// bug gone. That points at the one thing v2.9.22 removed along with the
// hand-resolution: v2.9.21 and earlier MFLN-probed the resolved CDN host
// directly before connecting to it, so the buffer size and the server's
// actual TLS record size were confirmed to match. v2.9.22 onward stopped
// resolving the CDN host at all (on purpose, to stop connecting to it
// directly) but never replaced that probe -- it just guesses 4096 blind.
// BearSSL's own setBufferSizes() doesn't negotiate anything by itself; if
// the CDN host doesn't honor a 4096-byte MFLN request, the server is free to
// send full-size (~16 KB) records the 4096+overhead buffer can't hold, and
// that surfaces later, mid-download, as exactly this kind of stall/drop --
// not as an obvious "buffer too small" error. otaResolveRedirect()/
// otaUrlHost() below restore the probe (learning the CDN host, then probing
// *it*, not github.com) without restoring the bug: it's a separate,
// short-lived connection that's fully closed before the small/large
// attempts, which still go through ESPhttpUpdate's own
// HTTPC_FORCE_FOLLOW_REDIRECTS on the original github.com URL, exactly as
// v2.9.22 established. If the CDN turns out not to honor MFLN at any size,
// the 4 KB attempt is skipped outright (a mismatched small buffer is worse
// than not trying it) and only the 16 KB attempt runs, gated on heap as
// before -- and, confirmed live on v2.9.26 (see below), that CDN really
// doesn't honor MFLN at all, so v2.9.28 adds a third, structurally
// different attempt (otaDownloadRanged(), also below) that sidesteps the
// whole MFLN/heap question via small HTTP Range requests instead of one
// streamed response.
//
// The web UI queues the request in LittleFS and reboots; this runs early in
// setup() with the heap still free. The request is consumed BEFORE the
// attempt, so a crash or failure can never boot-loop.
//
// Testing note: this code only ever runs as part of the CURRENTLY INSTALLED
// firmware. As long as the automatic download keeps failing, the device
// never advances, so every fix pushed here keeps getting "tested" by
// re-running whatever fetch logic was already on the device -- not the new
// code just pushed. Verifying a fix for real requires landing it on the
// device first (manual upload, System tab) and then testing whether THAT
// build can auto-update itself to a subsequent release. v2.9.22 hit exactly
// this: the error strings the device kept reporting ("resolved (4096B)"/
// "fallback (16384B)") were still v2.9.21's tags long after v2.9.22 was
// pushed and its automatic OTA "succeeded" at nothing -- proof the fetch
// code never actually changed until it was flashed manually. v2.9.24 hit
// the same trap for the heap-fragmentation fix in main.cpp/Net.cpp (deferred
// mDNS/SNTP past a queued update): it had to be flashed manually too, since
// otherwise the OLD, already-installed fetch code -- with its own already-
// fragmented heap -- would be the one "testing" it. v2.9.26 (this file's
// MFLN-probe fix) hit it again identically, and was flashed manually and
// confirmed booting fine; testing it against v2.9.27 (a docs-only bump)
// gave the conclusive live result this comment opens with: the CDN honors
// no MFLN size at all, and the 16 KB path's heap precheck still never
// passes. v2.9.28 (otaDownloadRanged(), below) will need the same manual
// flash + one more release before it can be genuinely tested in turn.
#if defined(SMALLTV_ESP8266)
static const char* OTA_REQ_PATH = "/ota.req";
static const char* OTA_MSG_PATH = "/ota.msg";

// Pulls the "host[:port]" component out of an "https://host[:port]/path..."
// URL. Used to get a bare hostname to hand to probeMaxFragmentLength()/
// probeMfln(), which take a host, not a URL.
static String otaUrlHost(const String& url) {
  int start = url.indexOf("://");
  if (start < 0) return String();
  start += 3;
  int end = url.indexOf('/', start);
  String host = (end > start) ? url.substring(start, end) : url.substring(start);
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  return host;
}

// Learns the URL github.com's redirect points the release asset at (the
// signed, time-limited CDN URL) -- purely to *look at* it (probeMfln() on
// its host below, or reusing the URL itself for otaDownloadRanged()'s
// keep-alive chunk requests further down); this connection is intentionally
// separate from, and fully closed before, either use. Connecting straight
// to a pre-resolved CDN URL for a *single, full-size* download attempt was
// the blamed cause of six identical failures earlier this session (see the
// file-level comment above) -- but that was about the download itself, not
// about looking up where it points, and otaDownloadRanged()'s requests are
// each individually Range-bounded and small regardless of this URL's
// origin, so reusing it there doesn't reintroduce that bug. A small, fixed
// 4 KB buffer is used for this lookup -- github.com's own host has reliably
// supported that size all session (see otaCheckLatest above) -- so a
// failure here just means "couldn't learn the URL", not a download fault;
// callers fall back to their own pre-v2.9.26 behavior in that case.
static String otaResolveRedirect(const String& url) {
  SecureClient client;
  client.setInsecure();
  client.setBufferSizes(4096, 512);

  HTTPClient http;
  http.setTimeout(8000);
  http.setReuse(false);
  http.setUserAgent(F(FW_NAME));
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* hdrKeys[] = { "Location" };
  http.collectHeaders(hdrKeys, 1);

  String loc;
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code >= 300 && code < 400) loc = http.header("Location");
    http.end();
  }
  return loc;
}

// Downloads and flashes the firmware in small, HTTP Range-bounded chunks
// over ONE reused HTTPS connection, instead of asking the server to hold to
// a small TLS record size for one continuous streamed response. Directly
// probing this CDN (see otaResolveRedirect/otaUrlHost above and the
// file-level comment) confirmed it honors no MFLN fragment size under
// 16384 bytes -- so it always frames a plain streamed response in
// full-size (~16 KB) TLS records -- while this device has never once
// freed more than ~14 KB of *contiguous* heap while WiFi is up, even with
// mDNS/SNTP deferred past the download (v2.9.24). Those two numbers don't
// meet: 16 KB records need a buffer this chip can't reliably clear. A
// Range-bounded response can't legally arrive wrapped in a TLS record
// bigger than the response itself, though, so keeping each request's
// response small (RANGE_CHUNK bytes of body, comfortably under
// RANGE_BUF's capacity once headers and TLS framing are accounted for)
// sidesteps the ceiling entirely instead of trying to raise it further.
// One TLS connection is opened and reused (HTTPClient's setReuse) across
// every chunk: partly to avoid ~100 separate handshakes, partly because
// reusing one already-allocated set of BearSSL buffers can't progressively
// fragment the heap the way repeated per-chunk alloc/free cycles could.
static const uint32_t RANGE_BUF   = 8192;  // TLS RX buffer; needs ~9216B contiguous --
                                            // comfortably under the ~14 KB ceiling above
static const uint32_t RANGE_CHUNK = 7168;  // body bytes/request; leaves ~1KB of RANGE_BUF
                                            // for response headers + TLS record overhead

static bool otaDownloadRanged(const String& url, String& err) {
  const uint32_t need    = RANGE_BUF + 512 + 8000;
  const uint32_t needBlk = RANGE_BUF + 1024;
  for (int tries = 0; tries < 12; tries++) {
    if (ESP.getFreeHeap() >= need && ESP.getMaxFreeBlockSize() >= needBlk) break;
    delay(250);
  }
  if (ESP.getFreeHeap() < need || ESP.getMaxFreeBlockSize() < needBlk) {
    char msg[100];
    snprintf(msg, sizeof(msg), "not enough heap (%u free, %u largest block, need %u / %u)",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
             (unsigned)need, (unsigned)needBlk);
    err = msg;
    return false;
  }

  SecureClient client;
  client.setInsecure();
  client.setBufferSizes(RANGE_BUF, 512);

  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(true);              // keep the TLS session alive across chunk requests
  http.setUserAgent(F(FW_NAME));
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // this IS the resolved URL already
  const char* hdrKeys[] = { "Content-Range" };
  http.collectHeaders(hdrKeys, 1);

  uint32_t total  = 0;     // learned from the first response's Content-Range
  uint32_t offset = 0;
  bool     began  = false; // Update.begin() has run
  uint8_t  buf[512];

  while (!began || offset < total) {
    uint32_t last = began ? (((offset + RANGE_CHUNK < total) ? offset + RANGE_CHUNK : total) - 1)
                           : (offset + RANGE_CHUNK - 1);
    char rangeHdr[48];
    snprintf(rangeHdr, sizeof(rangeHdr), "bytes=%u-%u", (unsigned)offset, (unsigned)last);

    int code = -1;
    for (uint8_t retry = 0; retry < 3; retry++) {
      if (retry) delay(300);
      if (!http.begin(client, url)) continue;
      http.addHeader("Range", rangeHdr);
      code = http.GET();
      if (code == 206) break;
      http.end();
      code = -1;
    }
    if (code != 206) {
      err = "range GET failed at offset " + String(offset) +
            (code == -1 ? String(" (no 206 after retries)") : (": HTTP " + String(code)));
      if (began) Update.end(false);
      return false;
    }

    if (!began) {
      String cr = http.header("Content-Range");           // "bytes 0-7167/695504"
      int slash = cr.lastIndexOf('/');
      total = (slash >= 0) ? (uint32_t)cr.substring(slash + 1).toInt() : 0;
      if (!total || !Update.begin(total)) {
        err = !total ? "no Content-Range in response" : "Update.begin failed";
        http.end();
        return false;
      }
      began = true;
    }

    int chunkLen = http.getSize();       // this response's body length, not the total
    if (chunkLen < 0) chunkLen = 0;
    WiFiClient* stream = http.getStreamPtr();
    int remaining = chunkLen;
    bool chunkOk = (chunkLen > 0);
    while (remaining > 0) {
      int toRead = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
      int got = stream->readBytes(buf, toRead);
      if (got <= 0 || Update.write(buf, got) != (size_t)got) { chunkOk = false; break; }
      remaining -= got;
    }
    http.end();
    if (!chunkOk) {
      err = "stream read failed at offset " + String(offset);
      Update.end(false);
      return false;
    }
    offset += (uint32_t)chunkLen;
  }

  if (!Update.end(true) || !Update.isFinished()) {
    err = "Update.end failed, code " + String(Update.getError());
    return false;
  }
  return true;   // caller reboots
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

  // Probe the CDN's actual MFLN support before picking the small attempt's
  // buffer size (see the file-level comment above for the full reasoning: a
  // guessed size the CDN doesn't honor is indistinguishable, until well into
  // the download, from the "connection lost" / Stream Read Timeout failures
  // this file has been chasing all session). This resolve is a separate,
  // already-closed connection by this point -- the small/large attempts
  // below are unaffected and still go through ESPhttpUpdate's own redirect
  // handling on the original r.url.
  String cdnHost = otaUrlHost(otaResolveRedirect(r.url));
  uint16_t smallBuf = 4096;              // pre-v2.9.26 fallback if the lookup above failed
  bool     haveMfln = false;
  if (cdnHost.length()) {
    smallBuf = probeMfln(cdnHost.c_str());
    haveMfln = smallBuf < 16384;         // probeMfln returns 16384 itself when nothing smaller matched
  }

  // Both attempts below let ESPhttpUpdate/HTTPClient chase github.com's own
  // redirect to the real, signed CDN URL (see the file-level comment above
  // for why: six straight live failures on a hand-resolved direct-to-CDN
  // connection, across four falsified theories, pointed at that hand
  // resolution itself rather than at buffer size, heap, retries, or URL
  // freshness). "small" (now MFLN-probed, not guessed) runs first, since
  // this device's heap is fragmented enough that the largest contiguous
  // block has never once reached the 16 KB path's requirement in testing;
  // it's skipped outright when the CDN host was confirmed to support no
  // fragment size smaller than 16 KB, since a mismatched small buffer is
  // worse than not trying it. "large" always runs last, unconditionally, as
  // the original proven size in case a less-fragmented boot allows it.
  // Either way this can only let a tight-heap or MFLN-mismatched device
  // through that used to fail outright — never make a working device worse.
  struct OtaAttempt { uint32_t rxBuf; const char* tag; };
  OtaAttempt attempts[2];
  uint8_t n = 0;
  if (haveMfln || cdnHost.length() == 0) {
    attempts[n++] = { smallBuf, haveMfln ? "small(mfln)" : "small" };
  }
  attempts[n++] = { 16384, "large" };

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
  char errs[420] = {0};   // 3 possible entries now (small/large/ranged) -- was 300 for 2
  size_t errsLen = 0;
  auto record = [&errs, &errsLen](const char* tag, uint32_t rxBuf, const char* err) {
    int avail = (int)sizeof(errs) - (int)errsLen;
    if (avail <= 1) return;
    int n = snprintf(errs + errsLen, avail, "%s%s (%uB): %s",
                      errsLen ? "; " : "", tag, (unsigned)rxBuf, err);
    if (n > 0) errsLen += (n < avail - 1) ? (size_t)n : (size_t)(avail - 1);
  };
  // Record the skip itself when it happens — otherwise a report with only a
  // "large" entry looks identical to a boot where the small attempt was
  // never coded at all, and the whole point of probing first is to know
  // *why* it didn't run.
  if (cdnHost.length() && !haveMfln) {
    record("small", 0, "skipped: CDN honors no MFLN size <16384 (checked 512/1024/4096)");
  }
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

  // Both fixed-buffer attempts above are structurally doomed on a CDN that
  // won't shrink its TLS records (see the big comment above
  // otaDownloadRanged()): "small" either mismatches or gets skipped, and
  // "large" needs more contiguous heap than this device has ever produced.
  // Falls through to the Range-chunked approach as a last resort, on a
  // freshly re-resolved URL (rather than reusing cdnHost's resolve from
  // above) so a slow small/large loop above can't have let the signed URL's
  // validity window run out before the real download even starts.
  String rangedUrl = otaResolveRedirect(r.url);
  if (rangedUrl.length()) {
    String rangedErr;
    if (otaDownloadRanged(rangedUrl, rangedErr)) {
      ESP.restart();   // otaDownloadRanged() doesn't reboot itself -- ESPhttpUpdate.rebootOnUpdate does that for the other two attempts
      return;          // never reached; defensive
    }
    record("ranged", RANGE_BUF, rangedErr.c_str());
  } else {
    record("ranged", 0, "couldn't re-resolve the CDN URL");
  }

  otaBootResult(String("download failed: ") + errs);
}
#else
bool   otaBootRequested() { return false; }
bool   otaRequestBootUpdate(const char*) { return false; }
void   otaBootUpdate(const Settings&) {}
String otaTakeBootResult() { return String(); }
#endif
