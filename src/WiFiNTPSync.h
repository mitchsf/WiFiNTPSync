// WiFiNTPSync.h
// Shared WiFi + NTP boot/service API for Arduino ESP32 projects.
// Depends on the NTP2 library.
//
// Usage:
//   #include <WiFiNTPSync.h>
//
//   NTP2 ntp(wifiUDP);            // your NTP2 instance
//   WiFiNTPConfig netCfg;
//   WiFiNTPHooks  netHooks;
//
//   void setup() {
//     netCfg.ntpInstance       = &ntp;
//     netCfg.ssid              = &mySsid;       // String*
//     netCfg.password          = &myPassword;
//     netCfg.primaryNtpServer  = &myNtpServer;
//     netCfg.alternateNtpServer= &myNtpServer2; // optional
//     // Override any timing default you want.
//     netHooks.onProgress = [](bool wifi, bool ntp, uint32_t ms) { /* animate */ };
//     bootWiFiAndNTP(netCfg, netHooks);
//   }
//
//   void loop() { serviceWiFiAndNTP(netCfg, netHooks); }

#pragma once

#include <Arduino.h>
#include <NTP2.h>
#include "NtpGuard.h"     // client-side NTP sanity guard (shared by all clock apps)

struct WiFiNTPConfig {
  NTP2*        ntpInstance          = nullptr;   // required
  const String* ssid                = nullptr;   // required
  const String* password            = nullptr;   // empty string = open network
  const String* primaryNtpServer    = nullptr;   // required
  const String* alternateNtpServer  = nullptr;   // nullptr or empty disables rotation
  uint32_t wifiAttemptTimeoutMs     = 15000;
  uint32_t ntpAttemptTimeoutMs      = 15000;
  uint32_t ntpRuntimeIntervalMs     = 14400000UL; // 4 h
  uint32_t ntpRotateAfterMs         = 7200000UL;  // 2 h
  uint32_t wifiRuntimeRetryMs       = 60000;
  uint32_t bootTotalTimeoutMs       = 0;          // 0 = unlimited (default)
  uint32_t progressIntervalMs       = 0;          // 0 = fire on every poll iteration

  // Disable WiFi modem power-save the moment STA mode comes up, BEFORE the
  // boot NTP sync. Default modem sleep wakes the radio only at DTIM beacon
  // intervals and buffers incoming UDP, which delays NTP replies past the
  // response window on a marginal RF link (weak antenna, long range). Mains-
  // powered clocks want this off; leave true. Set false only for a battery
  // device that needs modem sleep and can tolerate slower NTP.
  bool     disableModemSleep        = true;

  // NTP retry cycle (send + wait-for-reply + gap-before-next-send).
  // Boot starts with ntpBootRetryCycleMs for ntpBootFastRetryWindowMs, then
  // backs off to ntpRuntimeRetryCycleMs until the first sync lands. After
  // that, retries on later failures use ntpRuntimeRetryCycleMs.
  // Values are in ms and represent the total cycle time, not just the gap.
  uint32_t ntpBootRetryCycleMs      = 5000;       // 5 s — fast initial discovery
  uint32_t ntpBootFastRetryWindowMs = 30000;      // 30 s — bound the boot burst
  uint32_t ntpRuntimeRetryCycleMs   = 30000;      // 30 s — RFC-friendly, no KoD risk
};

struct WiFiNTPHooks {
  void (*onStart)(bool wifiConnected, bool ntpSynced)                 = nullptr;
  void (*onProgress)(bool wifiConnected, bool ntpSynced, uint32_t ms) = nullptr;
  void (*onSuccess)(bool wifiConnected, bool ntpSynced)               = nullptr;
  void (*onReconnecting)()                                            = nullptr;
  void (*onReconnected)()                                             = nullptr;
  void (*onBootTimeout)(bool wifiConnected, bool ntpSynced)           = nullptr;
};

// Boot-time blocking connect. Returns false only if cfg.bootTotalTimeoutMs
// was set (>0) and expired before both WiFi and NTP succeeded.
bool bootWiFiAndNTP(const WiFiNTPConfig& cfg, const WiFiNTPHooks& hooks);

// Non-blocking runtime service. Call every loop iteration: pumps
// ntp.update(), reconnects WiFi if dropped, rotates to alternate NTP
// server if no sync in cfg.ntpRotateAfterMs.
void serviceWiFiAndNTP(const WiFiNTPConfig& cfg, const WiFiNTPHooks& hooks);

// Scan WiFi networks and return unique, trimmed SSIDs in RSSI order.
// Filters applied:
//   - empty SSID (hidden networks)
//   - SSIDs containing ',' (can't be safely represented in CSV-separated
//     dropdowns like WebPanel's, which splits options on commas)
//   - subsequent occurrences of an SSID already kept (mesh / extender /
//     dual-band routers broadcast one AP entry per radio/node)
// `out` must be a caller-allocated String array of at least `maxCount`
// entries. Returns the count of unique SSIDs actually written.
// `scanTimeMs` is the per-channel passive scan time (default 200 ms).
int scanUniqueSsids(String* out, int maxCount, uint32_t scanTimeMs = 200);
