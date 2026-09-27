#include "Net.h"
#include "Platform.h"
#include <DNSServer.h>

static NetMode     g_mode = NET_AP;
static DNSServer   g_dns;
static String      g_hostname;
static String      g_apSsid;
static uint32_t    g_lastReconnect = 0;
static const Settings* g_cfg = nullptr;  // for runtime failover between saved networks
static int8_t      g_curNet = -1;        // settings index of the joined network
static uint32_t    g_downSince = 0;      // 0 = connected; else millis() the drop began
static bool        g_mdnsStarted = false;

// ---- link diagnostics -------------------------------------------------------
// The crashes of September 2026 all died inside the WiFi SDK's beacon handling
// (ieee80211_setup_ratetable <- cnx_update_bss_more <- scan_parse_beacon), and
// only ever with the features running, never in safe mode. One suspect is
// the link dropping (e.g. beacons missed while a long TLS handshake holds the
// CPU) and the SDK tearing down / rebuilding the connection while beacons are
// still being parsed -- so count what the link actually does.
static volatile uint16_t g_discCount = 0;
static volatile int      g_discReason = 0;
static volatile uint32_t g_discAtMs = 0;
static bool              g_discAny = false;
static uint16_t          g_forcedReconnects = 0;
#if defined(SMALLTV_ESP8266)
static WiFiEventHandler  g_onDisc;   // must stay alive for the handler to stay registered
#endif

uint16_t netDisconnects()          { return g_discCount; }
int      netLastDisconnectReason() { return g_discReason; }
uint32_t netMsSinceDisconnect()    { return g_discAny ? (millis() - g_discAtMs) : 0xFFFFFFFFUL; }
uint16_t netForcedReconnects()     { return g_forcedReconnects; }

static void startAP(const Settings& s) {
  g_mode = NET_AP;
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  if (s.apPass.length() >= 8) {
    WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str());
  } else {
    WiFi.softAP(s.apSsid.c_str());           // open AP (WPA2 needs >=8 chars)
  }
  g_apSsid = s.apSsid;
  // Captive portal: answer every DNS query with our own IP.
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", apIP);
}

void netStartMdns() {
  if (g_mdnsStarted || g_mode != NET_STA) return;   // once per boot, STA only
  g_mdnsStarted = true;
  if (MDNS.begin(g_hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
#if WITH_USAGE
    // Discoverable usage-push service so the clawdmeter daemon can find and
    // push to every SmallTV on the LAN (no hardcoded host). TXT carries the
    // device id, firmware version, and the push path.
    MDNS.addService("clawdmeter", "tcp", 80);
    MDNS.addServiceTxt("clawdmeter", "tcp", "id",   g_hostname.c_str());
    MDNS.addServiceTxt("clawdmeter", "tcp", "ver",  FW_VERSION);
    MDNS.addServiceTxt("clawdmeter", "tcp", "path", "/api/usage");
#endif
  }
}

void netBegin(const Settings& s, void (*onProgress)(const char*), bool deferMdns) {
  g_cfg = &s;
  g_hostname = s.hostname.length() ? s.hostname : String(DEFAULT_HOSTNAME);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
#if defined(SMALLTV_ESP8266)
  g_onDisc = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
    if (g_discCount < 0xFFFF) g_discCount++;
    g_discReason = (int)e.reason;
    g_discAtMs = millis();
    g_discAny = true;
  });
#endif
  platformSetHostname(g_hostname.c_str());

  if (s.wifiCount == 0) {
    if (onProgress) onProgress("No WiFi saved");
    startAP(s);
    return;
  }

  WiFi.mode(WIFI_STA);

  // Try order: scan once (blocking is fine here, only the boot screen is up)
  // and put the networks the scan can see first, strongest first. Unseen ones
  // (hidden SSIDs or currently out of range) go last, in config order, with a
  // shorter timeout each.
  uint8_t order[MAX_WIFI_NETS];
  bool    seen[MAX_WIFI_NETS];
  if (s.wifiCount == 1) {
    order[0] = 0;
    seen[0] = true;
  } else {
    int32_t rssi[MAX_WIFI_NETS];
    for (uint8_t i = 0; i < s.wifiCount; i++) { rssi[i] = -32768; seen[i] = false; }
    if (onProgress) onProgress("Scanning...");
    int found = WiFi.scanNetworks();
    for (int a = 0; a < found; a++)
      for (uint8_t i = 0; i < s.wifiCount; i++)
        if (WiFi.SSID(a) == s.wifi[i].ssid && WiFi.RSSI(a) > rssi[i]) {
          rssi[i] = WiFi.RSSI(a);
          seen[i] = true;
        }
    WiFi.scanDelete();

    bool used[MAX_WIFI_NETS] = {false};
    for (uint8_t k = 0; k < s.wifiCount; k++) {
      int best = -1;
      for (uint8_t i = 0; i < s.wifiCount; i++) {
        if (used[i]) continue;
        if (best < 0 ||
            (seen[i] && !seen[best]) ||
            (seen[i] == seen[best] && rssi[i] > rssi[best])) best = i;
      }
      used[best] = true;
      order[k] = (uint8_t)best;
    }
  }

  for (uint8_t k = 0; k < s.wifiCount; k++) {
    const WifiCred& n = s.wifi[order[k]];
    if (onProgress) {
      char msg[48];
      snprintf(msg, sizeof(msg), "WiFi: %s", n.ssid.c_str());
      onProgress(msg);
    }
    WiFi.begin(n.ssid.c_str(), n.pass.c_str());

    uint32_t budget = seen[order[k]] ? 15000 : 8000;
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < budget) {
      delay(200);
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      g_curNet = (int8_t)order[k];
      g_mode = NET_STA;
      if (!deferMdns) netStartMdns();
      if (onProgress) onProgress(WiFi.localIP().toString().c_str());
      return;
    }
    WiFi.disconnect();
    delay(100);
  }

  if (onProgress) onProgress("WiFi failed -> AP");
  startAP(s);
}

void netLoop() {
  if (g_mode == NET_AP) {
    g_dns.processNextRequest();
    return;
  }
  // STA: keep mDNS alive, nudge reconnect if we dropped. After a long outage
  // rotate through the other saved networks. Never scan here — it would block
  // the display loop and the web server; WiFi.begin is fire-and-forget and its
  // status is picked up on later passes.
  platformMdnsUpdate();
  if (WiFi.status() == WL_CONNECTED) {
    g_downSince = 0;
    return;
  }
  if (!g_downSince) g_downSince = millis();
  // The SDK's own auto-reconnect (setAutoReconnect above) is already retrying
  // the moment the link drops. This used to force WiFi.reconnect() -- a hard
  // disconnect + connect -- every 10 s on top of it, i.e. tearing down the
  // SDK's connection attempt mid-flight while it may still be parsing that
  // router's beacons, which is where every crash of September 2026 died (see
  // the diagnostics note above). Now it leaves the first 60 s of an outage to
  // the SDK and only then nudges, every 30 s, as a fallback.
  uint32_t down = millis() - g_downSince;
  if (down > 60000 && millis() - g_lastReconnect > 30000) {
    g_lastReconnect = millis();
    if (g_forcedReconnects < 0xFFFF) g_forcedReconnects++;
    if (g_cfg && g_cfg->wifiCount > 1) {
      g_curNet = (int8_t)((g_curNet + 1) % g_cfg->wifiCount);
      WiFi.begin(g_cfg->wifi[g_curNet].ssid.c_str(), g_cfg->wifi[g_curNet].pass.c_str());
      g_downSince = millis() - 30000;   // this candidate gets 30 s before rotating on
    } else {
      WiFi.reconnect();
    }
  }
}

NetMode netMode()      { return g_mode; }
bool    netConnected() { return g_mode == NET_STA && WiFi.status() == WL_CONNECTED; }

String netIP() {
  return (g_mode == NET_AP) ? WiFi.softAPIP().toString()
                            : WiFi.localIP().toString();
}

String netSSID() {
  return (g_mode == NET_AP) ? g_apSsid : WiFi.SSID();
}

int netRSSI() {
  return (g_mode == NET_STA) ? WiFi.RSSI() : 0;
}
