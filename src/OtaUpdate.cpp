#include "OtaUpdate.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "Gfx.h"
#include <memory>
#include <new>

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
// The web UI (the Update now button, or the automatic checker) queues the
// request in LittleFS and reboots; otaBootUpdate() runs early in setup(),
// before the features claim the heap, and reboots again into the new image
// on success. The request is consumed BEFORE the attempt, so a crash or
// failure can never boot-loop by itself -- but see OtaUpdate.h for the
// automatic checker's loop guard, which that alone turned out not to cover.
//
// How the download got here, in short (full reasoning next to each function
// below):
//  - Up to v2.9.27: one streamed ESPhttpUpdate download, trying a small
//    (MFLN-probed) and a 16 KB TLS buffer. Live tests established that the
//    release CDN honors no MFLN fragment size under 16 KB, and that this
//    chip never frees the ~17 KB of contiguous heap the 16 KB buffer needs
//    while WiFi is up. Both attempts are structurally doomed on this hardware;
//    they remain only as a fallback.
//  - v2.9.28: otaDownloadRanged() -- small HTTP Range requests, each of whose
//    responses fits an 8 KB buffer. First live run crashed on a null stream
//    pointer after a 206 (fixed in v2.9.30), and every automatic attempt after
//    that died on the soft watchdog ~20 s into the boot, with no message.
//  - The same live test exposed a bigger problem: after each failed attempt the
//    device rebooted, the automatic checker fired again 5 minutes into the new
//    boot, and the cycle repeated every ~5.5 minutes for ~2 days.
//  - v2.9.31: the ranged download runs first, over static-RSA TLS (no
//    elliptic-curve math in the handshake), in 4 KB chunks, checking every
//    chunk's Content-Range and the whole image's MD5, refreshing an expired
//    signed link and resuming stalled chunks; RTC-memory breadcrumbs report
//    where an attempt died even when a reset killed it; and the automatic
//    checker stops retrying a release it has already failed to install twice.
//
// Testing note: this code only ever runs as part of the CURRENTLY INSTALLED
// firmware. As long as the automatic download keeps failing, the device never
// advances, so a fix pushed here is only "tested" by whatever fetch code was
// already on the device. Verifying a fix for real means flashing it manually
// (System tab) and then letting THAT build update itself to a later release.
// v2.9.22, v2.9.24, v2.9.26, v2.9.28 and v2.9.30 all went through exactly
// that, and so did v2.9.31: v2.9.30's own attempt to install it (Update now)
// died on the soft watchdog ~15 s into the boot, same as v2.9.28's -- so the
// null-stream fix alone never touched the watchdog -- and v2.9.31 was then
// flashed manually and booted cleanly. v2.9.32 is a docs-only release whose
// only job is to be installed BY v2.9.31's updater.

// ---- automatic-update loop guard (all targets) ------------------------------
// "<tag> <count>\n" for the most recent release the automatic checker tried to
// install. A different tag starts the count over, so every new release gets
// its own fresh tries. See OtaUpdate.h for why this exists.
static const char* OTA_AUTO_PATH = "/ota.auto";

static uint8_t otaAutoCount(const String& tag) {
  File f = LittleFS.open(OTA_AUTO_PATH, "r");
  if (!f) return 0;
  String line = f.readStringUntil('\n');
  f.close();
  int sp = line.indexOf(' ');
  if (sp <= 0 || line.substring(0, sp) != tag) return 0;
  long n = line.substring(sp + 1).toInt();
  return (uint8_t)(n < 0 ? 0 : (n > 255 ? 255 : n));
}

bool otaAutoAttemptAllowed(const String& tag) {
  return otaAutoCount(tag) < OTA_AUTO_MAX_TRIES;
}

void otaNoteAutoAttempt(const String& tag) {
  uint8_t n = otaAutoCount(tag);
  File f = LittleFS.open(OTA_AUTO_PATH, "w");
  if (!f) return;
  f.print(tag);
  f.print(' ');
  f.print((unsigned)(n < 255 ? n + 1 : 255));
  f.print('\n');
  f.close();
}

#if defined(SMALLTV_ESP8266)
static const char* OTA_REQ_PATH = "/ota.req";
static const char* OTA_MSG_PATH = "/ota.msg";

static void otaBootResult(const String& msg) {
  File f = LittleFS.open(OTA_MSG_PATH, "w");
  if (f) { f.print(msg); f.close(); }
}

// ---- progress breadcrumbs (RTC memory) --------------------------------------
// A watchdog reset or an exception in the middle of the boot-time download
// leaves no error message behind -- the code that would have written one
// never runs -- and this device has no serial console. That is exactly how
// v2.9.28's automatic update failed for ~2 days with nothing to show for it
// but "Last reset: Software Watchdog", every few minutes, without ever saying
// where. So otaBootUpdate() now drops a breadcrumb into RTC user memory at
// every step. RTC memory survives watchdog and exception resets (not a power
// cut, and power-on garbage is caught by the magic + check word), one write
// is a few microseconds, and on the next boot otaReportInterrupted() turns it
// into the "Last update:" line the System tab already shows. Blocks 0-31 of
// RTC user memory hold eboot's command (the one that copies a freshly
// downloaded image into place), so the breadcrumb sits well above them.
static const uint32_t CRUMB_BLOCK = 96;            // 4-byte blocks; 96 = byte 384 of 512
static const uint32_t CRUMB_MAGIC = 0x4F544332UL;  // "OTC2"
static const uint32_t CRUMB_RSA   = 1;             // detail bit: static-RSA TLS was in use

struct OtaCrumb { uint32_t magic, phase, offset, total, detail, check; };

enum : uint32_t {
  PH_NONE = 0,   // nothing in flight
  PH_CHECK,      // asking the GitHub API for the latest release
  PH_RESOLVE,    // asking github.com where the signed download link points
  PH_HEAP,       // waiting for enough contiguous heap for the TLS buffers
  PH_REQUEST,    // TLS connect + Range request for one chunk
  PH_READ,       // reading one chunk's body into flash
  PH_FINISH,     // Update.end(): MD5 + image header check
  PH_LEGACY,     // the older single-stream ESPhttpUpdate attempts
  PH_REBOOT,     // image written and verified; rebooting into it
};

static uint32_t crumbCheck(const OtaCrumb& c) {
  return c.magic ^ c.phase ^ c.offset ^ c.total ^ c.detail ^ 0xA5A5A5A5UL;
}

static void crumb(uint32_t phase, uint32_t offset = 0, uint32_t total = 0, uint32_t detail = 0) {
  OtaCrumb c = { CRUMB_MAGIC, phase, offset, total, detail, 0 };
  c.check = crumbCheck(c);
  ESP.rtcUserMemoryWrite(CRUMB_BLOCK, reinterpret_cast<uint32_t*>(&c), sizeof(c));
}

static const char* phaseText(uint32_t p) {
  switch (p) {
    case PH_CHECK:   return "checking GitHub for the latest release";
    case PH_RESOLVE: return "looking up the download link";
    case PH_HEAP:    return "waiting for free memory";
    case PH_REQUEST: return "connecting/requesting a chunk";
    case PH_READ:    return "downloading a chunk";
    case PH_FINISH:  return "verifying the new image";
    case PH_LEGACY:  return "trying the older single-stream download";
    default:         return "updating";
  }
}

void otaReportInterrupted(const char* resetReason) {
  OtaCrumb c;
  if (!ESP.rtcUserMemoryRead(CRUMB_BLOCK, reinterpret_cast<uint32_t*>(&c), sizeof(c))) return;
  if (c.magic != CRUMB_MAGIC || c.check != crumbCheck(c)) return;   // never written / power-on noise
  crumb(PH_NONE);                                                   // report each breadcrumb once
  if (c.phase == PH_NONE) return;

  char msg[200];
  if (c.phase == PH_REBOOT) {
    // offset carries verNum() of the firmware that did the download.
    long from = (long)c.offset;
    if (from == verNum(FW_VERSION)) {
      snprintf(msg, sizeof(msg), "new image was written, but the device came back on " FW_VERSION
               " (the copy into place did not take)");
    } else {
      snprintf(msg, sizeof(msg), "updated from %ld.%ld.%ld to " FW_VERSION,
               from / 10000, (from / 100) % 100, from % 100);
    }
  } else if (c.total) {
    snprintf(msg, sizeof(msg), "update cut off by a reset (%s) while %s, at byte %u of %u [%s TLS]",
             resetReason, phaseText(c.phase), (unsigned)c.offset, (unsigned)c.total,
             (c.detail & CRUMB_RSA) ? "rsa" : "ecdhe");
  } else {
    snprintf(msg, sizeof(msg), "update cut off by a reset (%s) while %s",
             resetReason, phaseText(c.phase));
  }
  otaBootResult(msg);
}

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

// ---- Range-chunked download ------------------------------------------------
// Downloads and flashes the firmware in small, HTTP Range-bounded chunks
// instead of asking the server to hold to a small TLS record size for one
// continuous streamed response. Directly probing this CDN (see
// otaResolveRedirect/otaUrlHost above and the file-level comment) confirmed
// it honors no MFLN fragment size under 16384 bytes -- so it frames a plain
// streamed response in full-size (~16 KB) TLS records -- while this device
// has never once freed more than ~14 KB of *contiguous* heap while WiFi is
// up. Those two numbers don't meet. A Range-bounded response can't arrive
// wrapped in a TLS record bigger than the response itself, though, so keeping
// each response small sidesteps that ceiling instead of trying to raise it.
//
// What v2.9.31 changed here, after v2.9.28's version (first to run live)
// crashed once on a null stream and then died on the soft watchdog on every
// automatic attempt afterwards:
//  - Static-RSA TLS first. An SSL Labs scan of release-assets.githubusercontent.com
//    shows an RSA-2048 certificate and exactly one non-ECDHE suite,
//    TLS_RSA_WITH_AES_128_GCM_SHA256. Offering only that suite takes every
//    bit of elliptic-curve math out of the handshake (the RSA public-key
//    operation is a few ms), and a long, uninterruptible EC step inside a
//    BearSSL handshake is the leading suspect for the watchdog: nothing in
//    this download loop can yield while BearSSL computes. BearSSL's own
//    setCiphersLessSecure() is no use here -- its list is CBC-only, and this
//    CDN offers no RSA-CBC suite at all. If that connection fails outright,
//    the rest of the download falls back to BearSSL's default list (what
//    v2.9.28-v2.9.30 used).
//  - 4 KB chunks, not 7 KB. Measured response headers run ~950 bytes; if the
//    server packs headers and body into one TLS record, 7168 + ~950 bytes
//    was within ~100 bytes of what an 8 KB BearSSL buffer can take (BearSSL
//    needs ~325 bytes of it for record overhead). 4096 leaves ~3.7 KB spare.
//  - Every chunk's Content-Range is checked against what was asked for before
//    a byte of it reaches flash, and the whole image is checked against the
//    CDN's own MD5 (x-ms-blob-content-md5, the Azure blob's stored hash --
//    verified equal to the release file's MD5) in Update.end(). A wrong or
//    shuffled chunk can never be committed as the new firmware.
//  - Dead links and stalls are recoverable. The signed CDN link's token
//    expires 5 minutes after it is issued, so a 4xx gets a fresh link from
//    github.com and the same chunk is asked for again; a stream that stalls
//    mid-chunk keeps what already reached flash, closes the connection (never
//    reused with unread bytes in flight) and resumes from exactly that byte.
//  - Progress on screen ("updating 40%") and in the RTC breadcrumb.
static const uint32_t RANGE_BUF      = 8192;  // TLS RX buffer; needs ~9216B contiguous
static const uint32_t RANGE_CHUNK    = 4096;  // body bytes per request (see above)
static const uint8_t  RANGE_RESOLVES = 4;     // fresh-link lookups allowed per download
static const uint8_t  RANGE_STALLS   = 8;     // mid-chunk stalls (resume points) allowed

static const uint16_t kRsaGcmOnly[] PROGMEM = { BR_TLS_RSA_WITH_AES_128_GCM_SHA256 };
static const char* HDR_RANGE = "Content-Range";
static const char* HDR_MD5   = "x-ms-blob-content-md5";

// "bytes 4096-8191/697584" -> 4096, 8191, 697584
static bool parseContentRange(const String& cr, uint32_t& start, uint32_t& end, uint32_t& total) {
  unsigned long s = 0, e = 0, t = 0;
  if (sscanf(cr.c_str(), "bytes %lu-%lu/%lu", &s, &e, &t) != 3) return false;
  if (e < s || t <= e) return false;
  start = (uint32_t)s; end = (uint32_t)e; total = (uint32_t)t;
  return true;
}

// Base64 MD5 ("2GPYZC2+0hVbS6iiYsBCSQ==") -> 32 lowercase hex chars + NUL,
// the form Update.setMD5() wants. False on anything that isn't exactly 16 bytes.
static bool b64Md5ToHex(const String& b64, char out[33]) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t bin[16];
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < b64.length(); i++) {
    char ch = b64[i];
    if (ch == '=') break;
    const char* p = ch ? strchr(tbl, ch) : nullptr;
    if (!p) return false;
    acc = (acc << 6) | (uint32_t)(p - tbl);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= sizeof(bin)) return false;
      bin[n++] = (uint8_t)(acc >> bits);
    }
  }
  if (n != sizeof(bin)) return false;
  for (size_t i = 0; i < sizeof(bin); i++) snprintf(out + i * 2, 3, "%02x", bin[i]);
  return true;
}

static bool otaDownloadRanged(const String& ghUrl, String& err) {
  crumb(PH_RESOLVE);
  String url = otaResolveRedirect(ghUrl);
  if (!url.length()) { err = F("couldn't look up the download link"); return false; }

  crumb(PH_HEAP);
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

  // Two client objects, never connected at the same time (the other one is
  // always stopped first, so only one set of 8 KB buffers is ever allocated).
  SecureClient rsaClient;                 // static RSA: no elliptic-curve math at all
  rsaClient.setInsecure();
  rsaClient.setBufferSizes(RANGE_BUF, 512);
  rsaClient.setCiphers(kRsaGcmOnly, 1);
  SecureClient fullClient;                // BearSSL's default list, as used up to v2.9.30
  fullClient.setInsecure();
  fullClient.setBufferSizes(RANGE_BUF, 512);
  bool useRsa = true;

  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(true);                    // keep the connection across chunk requests
  http.setUserAgent(F(FW_NAME));
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);   // this IS the resolved URL already
  const char* hdrKeys[] = { HDR_RANGE, HDR_MD5 };
  http.collectHeaders(hdrKeys, 2);

  uint32_t total    = 0;       // learned from the first response's Content-Range
  uint32_t offset   = 0;       // bytes already written to flash
  bool     began    = false;   // Update.begin() has run
  bool     md5Set   = false;
  uint8_t  resolves = 0;
  uint8_t  stalls   = 0;
  int      shownPct = -10;
  // Read buffer on the heap, not the 4 KB cont stack: /api/status has shown
  // that stack's high-water mark within ~64 bytes of full in normal running.
  const size_t kBufLen = 512;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kBufLen]);
  if (!buf) { err = F("out of memory for the read buffer"); return false; }

  auto dropConnection = [&]() {   // never reuse a connection that may have bytes in flight
    http.end();
    rsaClient.stop();
    fullClient.stop();
  };
  auto abortWith = [&](const String& why) {
    err = why;
    dropConnection();
    if (began) Update.end(false);
    return false;
  };

  while (!began || offset < total) {
    uint32_t want = began ? ((total - offset < RANGE_CHUNK) ? total - offset : RANGE_CHUNK)
                          : RANGE_CHUNK;
    uint32_t last = offset + want - 1;
    char rangeHdr[40];
    snprintf(rangeHdr, sizeof(rangeHdr), "bytes=%u-%u", (unsigned)offset, (unsigned)last);

    // Up to three tries per chunk. A 206 only counts once getStreamPtr() is
    // non-null and the body is non-empty -- v2.9.28 crashed trusting the
    // status alone (a null stream after a 206 on a reused connection).
    int code = -1;
    int chunkLen = 0;
    WiFiClient* stream = nullptr;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
      if (attempt) delay(300);
      crumb(PH_REQUEST, offset, total, useRsa ? CRUMB_RSA : 0);
      if (!http.begin(useRsa ? static_cast<WiFiClient&>(rsaClient) : static_cast<WiFiClient&>(fullClient), url)) {
        code = -1;
        continue;
      }
      http.addHeader("Range", rangeHdr);
      code = http.GET();
      if (code == 206) {
        chunkLen = http.getSize();
        stream = http.getStreamPtr();
        if (stream && chunkLen > 0) break;
        stream = nullptr;
      }
      dropConnection();
      if (code == HTTPC_ERROR_CONNECTION_FAILED && useRsa && !began) {
        useRsa = false;    // static-RSA offer refused (or connect failed): default suites from here on
      }
      if (code >= 400 && code < 500) break;   // link rejected/expired: the same URL won't do better
    }

    if (!stream) {
      if (resolves < RANGE_RESOLVES) {
        // A fresh signed link from github.com (the old one's token may simply
        // have expired), then the same chunk again on a brand-new connection.
        resolves++;
        dropConnection();
        crumb(PH_RESOLVE, offset, total, useRsa ? CRUMB_RSA : 0);
        String fresh = otaResolveRedirect(ghUrl);
        if (fresh.length()) { url = fresh; continue; }
      }
      char msg[120];
      snprintf(msg, sizeof(msg), "no usable response for %s (last code %d, %s TLS, %u link refreshes)",
               rangeHdr, code, useRsa ? "rsa" : "ecdhe", (unsigned)resolves);
      return abortWith(msg);
    }

    // Only the exact bytes asked for may go near flash.
    uint32_t cs = 0, ce = 0, ct = 0;
    String cr = http.header(HDR_RANGE);
    if (!parseContentRange(cr, cs, ce, ct) || cs != offset ||
        (ce - cs + 1) != (uint32_t)chunkLen || (began && ct != total)) {
      return abortWith("unexpected Content-Range '" + cr + "' (asked for " + rangeHdr +
                       ", length " + String(chunkLen) + ")");
    }

    if (!began) {
      total = ct;
      if (!Update.begin(total)) {
        return abortWith("Update.begin(" + String(total) + ") failed: " + Update.getErrorString());
      }
      began = true;
      char md5hex[33];
      md5Set = b64Md5ToHex(http.header(HDR_MD5), md5hex) && Update.setMD5(md5hex);
    }

    crumb(PH_READ, offset, total, useRsa ? CRUMB_RSA : 0);
    uint32_t written = 0;
    bool flashErr = false;
    while (written < (uint32_t)chunkLen) {
      size_t toRead = ((uint32_t)chunkLen - written < kBufLen) ? (size_t)((uint32_t)chunkLen - written)
                                                               : kBufLen;
      int got = (int)stream->readBytes(buf.get(), toRead);
      if (got <= 0) break;                                   // stalled/dropped: resume below
      if (Update.write(buf.get(), (size_t)got) != (size_t)got) { flashErr = true; break; }
      written += (uint32_t)got;
    }
    offset += written;

    if (flashErr) {
      return abortWith("flash write failed at byte " + String(offset) + ": " + Update.getErrorString());
    }
    if (written < (uint32_t)chunkLen) {
      // Whatever arrived is already in flash; carry on from exactly there on a
      // fresh connection rather than reusing one with unread bytes in flight.
      if (++stalls > RANGE_STALLS) {
        char msg[100];
        snprintf(msg, sizeof(msg), "stream kept stalling (%u times), last at byte %u of %u",
                 (unsigned)stalls, (unsigned)offset, (unsigned)total);
        return abortWith(msg);
      }
      dropConnection();
      continue;
    }
    http.end();   // whole body read: the connection is clean and stays open for the next chunk

    int pct = (int)((uint64_t)offset * 100 / total);
    if (pct / 10 != shownPct / 10) {
      shownPct = pct;
      char line[20];
      snprintf(line, sizeof(line), "updating %d%%", pct);
      gfxBoot("SmallTV", line);
    }
    yield();
  }
  dropConnection();

  crumb(PH_FINISH, offset, total, useRsa ? CRUMB_RSA : 0);
  if (!Update.end()) {   // strict: every byte written, MD5 (when the CDN sent one) must match
    err = String("image check failed") + (md5Set ? " (MD5 checked)" : " (no MD5 from CDN)") +
          ": " + Update.getErrorString();
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

  crumb(PH_CHECK);
  OtaLatest r = otaCheckLatest(s);          // re-resolve the asset URL fresh
  if (!r.ok)    { crumb(PH_NONE); otaBootResult("check failed: " + r.error); return; }
  if (!r.newer) { crumb(PH_NONE); otaBootResult(F("already up to date (" FW_VERSION ")")); return; }

  // Every attempt's outcome, not just the last one, built with snprintf into
  // a fixed stack buffer rather than chained String concatenation: these
  // messages can be built at the exact moment the heap is critically low.
  char errs[420] = {0};
  size_t errsLen = 0;
  auto record = [&errs, &errsLen](const char* tag, uint32_t rxBuf, const char* err) {
    int avail = (int)sizeof(errs) - (int)errsLen;
    if (avail <= 1) return;
    int n = snprintf(errs + errsLen, avail, "%s%s (%uB): %s",
                      errsLen ? "; " : "", tag, (unsigned)rxBuf, err);
    if (n > 0) errsLen += (n < avail - 1) ? (size_t)n : (size_t)(avail - 1);
  };

  // 1) The Range-chunked download: the only approach that fits this chip's
  //    heap against this CDN (see otaDownloadRanged() above). It runs first,
  //    on the freshest heap this boot will have.
  {
    String rangedErr;
    if (otaDownloadRanged(r.url, rangedErr)) {
      crumb(PH_REBOOT, (uint32_t)verNum(FW_VERSION));
      ESP.restart();
      return;          // never reached
    }
    record("ranged", RANGE_BUF, rangedErr.c_str());
  }

  // 2) Fallback: the older single-stream attempts, unchanged in substance.
  //    Both let ESPhttpUpdate/HTTPClient chase github.com's own redirect to
  //    the signed CDN URL itself, and differ only in RX buffer size. "small"
  //    is MFLN-probed against the CDN host and skipped when that host honors
  //    no fragment size under 16 KB (which, as of v2.9.26's live test, it
  //    doesn't); "large" needs more contiguous heap than this device has ever
  //    freed. Neither has succeeded on this hardware -- they stay only because
  //    trying them costs a few seconds of a boot that is failing anyway.
  crumb(PH_LEGACY);
  ESPhttpUpdate.rebootOnUpdate(false);      // reboot below, after the breadcrumb
  String cdnHost = otaUrlHost(otaResolveRedirect(r.url));
  uint16_t smallBuf = 4096;                 // pre-v2.9.26 fallback if the lookup above failed
  bool     haveMfln = false;
  if (cdnHost.length()) {
    smallBuf = probeMfln(cdnHost.c_str());
    haveMfln = smallBuf < 16384;            // probeMfln returns 16384 itself when nothing smaller matched
  }

  struct OtaAttempt { uint32_t rxBuf; const char* tag; };
  OtaAttempt attempts[2];
  uint8_t n = 0;
  if (haveMfln || cdnHost.length() == 0) {
    attempts[n++] = { smallBuf, haveMfln ? "small(mfln)" : "small" };
  } else {
    record("small", 0, "skipped: CDN honors no MFLN size <16384 (checked 512/1024/4096)");
  }
  attempts[n++] = { 16384, "large" };

  for (uint8_t i = 0; i < n; i++) {
    uint32_t rxBuf = attempts[i].rxBuf;
    // rx + tx buffers plus BearSSL engine/stack-thunk overhead; total free can
    // clear this while no single block is big enough, so wait briefly (the
    // delay()s service WiFi/lwIP and let freed blocks coalesce) before giving up.
    const uint32_t need    = rxBuf + 512 + 8000;
    const uint32_t needBlk = rxBuf + 1024;
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
      continue;
    }

    // Retried once on the transient-looking errors only.
    String lastErr;
    for (uint8_t retry = 0; retry < 2; retry++) {
      if (retry) delay(300);

      BearSSL::WiFiClientSecure client;
      client.setInsecure();
      client.setBufferSizes(rxBuf, 512);
      ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

      t_httpUpdate_return ret = ESPhttpUpdate.update(client, r.url);
      if (ret == HTTP_UPDATE_OK) {
        crumb(PH_REBOOT, (uint32_t)verNum(FW_VERSION));
        ESP.restart();
        return;        // never reached
      }
      if (ret == HTTP_UPDATE_NO_UPDATES) { crumb(PH_NONE); otaBootResult(F("server reported no update")); return; }
      lastErr = ESPhttpUpdate.getLastErrorString();
      if (lastErr != "connection lost" &&
          !lastErr.startsWith("Update error: ERROR[6]")) {
        break;         // not the transient pattern -- a retry won't help
      }
    }
    record(attempts[i].tag, rxBuf, lastErr.c_str());
  }

  crumb(PH_NONE);
  otaBootResult(String("download failed: ") + errs);
}
#else
bool   otaBootRequested() { return false; }
bool   otaRequestBootUpdate(const char*) { return false; }
void   otaBootUpdate(const Settings&) {}
String otaTakeBootResult() { return String(); }
void   otaReportInterrupted(const char*) {}
#endif
