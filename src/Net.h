// Net.h — WiFi station / fallback AP / captive portal / mDNS
#pragma once
#include <Arduino.h>
#include "Settings.h"

enum NetMode { NET_STA, NET_AP };

// Connects to the configured station; falls back to AP if that fails or no
// credentials are stored. `onProgress` (optional) is called with short status
// strings so the display can show what's happening during the boot connect.
// `deferMdns`: skip starting mDNS here even on a successful STA connect: the
// caller must call netStartMdns() itself later. mDNS.begin() is, like SNTP, a
// permanent mid-arena heap allocation (several service/TXT records) that can
// fragment the heap below what a boot-time OTA download's TLS handshake
// needs -- pass true when a GitHub update is queued for this boot and call
// netStartMdns() after that attempt instead (see main.cpp).
void netBegin(const Settings& s, void (*onProgress)(const char*) = nullptr, bool deferMdns = false);
void netStartMdns();      // start mDNS + service adverts now; no-op if not STA or already done
void netLoop();           // pump DNS (AP) / mDNS (STA) / reconnect

NetMode  netMode();
bool     netConnected();  // STA associated with an IP
String   netIP();         // current IP (STA or AP)
String   netSSID();       // joined SSID (STA) or AP SSID
int      netRSSI();       // STA signal, 0 in AP mode
