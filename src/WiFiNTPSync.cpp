// WiFiNTPSync.cpp
// Implementation of bootWiFiAndNTP() and serviceWiFiAndNTP().
// All NTP state lives on the caller's NTP2 instance (cfg.ntpInstance) —
// no globals, no hidden assumptions.

#include "WiFiNTPSync.h"
#include <WiFi.h>

static inline bool _wnWifiUp() { return WiFi.status() == WL_CONNECTED; }

static void _wnDoWifiBegin(const WiFiNTPConfig& cfg) {
  if (!cfg.ssid) return;
  WiFi.disconnect();
  if (cfg.password && cfg.password->length() > 0)
    WiFi.begin(cfg.ssid->c_str(), cfg.password->c_str());
  else
    WiFi.begin(cfg.ssid->c_str());
}

// Re-begin every attempt so DNS gets re-resolved. If the first resolve
// happened before the WiFi stack was fully ready, the cached server IP
// would be stale (or 0.0.0.0) and every forceUpdate would fail.
// Configure NTP2 timings. responseDelay is the wait-for-reply window; the
// effective retry cycle is responseDelay + retryDelay. cycleMs is the total
// cycle time the caller wants; we derive retryDelay from it.
// updateInterval() is intentionally NOT set here — each caller owns the
// post-sync polling cadence (otherwise a stale 10 s value would persist
// past first sync and clobber the runtime interval on rotation).
static void _wnBeginNtp(NTP2* ntp, const char* server, uint32_t cycleMs) {
  const uint32_t responseMs = 1600;
  uint32_t retryMs = (cycleMs > responseMs) ? (cycleMs - responseMs) : 100;
  ntp->begin(server);
  ntp->responseDelay(responseMs);
  ntp->retryDelay(retryMs);
}

int scanUniqueSsids(String* out, int maxCount, uint32_t scanTimeMs) {
  if (!out || maxCount <= 0) return 0;
  int n = WiFi.scanNetworks(false, false, false, scanTimeMs);
  if (n < 0) n = 0;
  int count = 0;
  for (int i = 0; i < n && count < maxCount; i++) {
    String s = WiFi.SSID(i);
    s.trim();
    if (s.length() == 0) continue;          // hidden network
    if (s.indexOf(',') >= 0) continue;      // CSV-unsafe for WebPanel dropdowns
    bool dup = false;
    for (int j = 0; j < count; j++) {
      if (out[j] == s) { dup = true; break; }
    }
    if (dup) continue;
    out[count++] = s;
  }
  WiFi.scanDelete();  // free the WiFi stack's internal scan result table
  return count;
}

bool bootWiFiAndNTP(const WiFiNTPConfig& cfg, const WiFiNTPHooks& hooks) {
  if (!cfg.ntpInstance) return false;   // guaranteed misuse; bail rather than UB

  NTP2* ntp = cfg.ntpInstance;
  unsigned long bootStart    = millis();
  unsigned long lastProgress = 0;

  auto expired = [&]() -> bool {
    return cfg.bootTotalTimeoutMs > 0 &&
           (millis() - bootStart) > cfg.bootTotalTimeoutMs;
  };
  auto fireProgress = [&](bool wifi, bool ntpOk) {
    if (!hooks.onProgress) return;
    unsigned long now = millis();
    if (cfg.progressIntervalMs > 0 && now - lastProgress < cfg.progressIntervalMs) return;
    lastProgress = now;
    hooks.onProgress(wifi, ntpOk, now - bootStart);
  };
  auto timeoutAndReturn = [&](bool wifi, bool ntpOk) -> bool {
    if (hooks.onBootTimeout) hooks.onBootTimeout(wifi, ntpOk);
    return false;
  };

  // ----- WiFi phase -----
  if (hooks.onStart) hooks.onStart(false, false);
  WiFi.mode(WIFI_STA);
  // Kill modem power-save NOW, before the boot NTP sync — not after boot.
  // Default DTIM buffering delays incoming NTP UDP replies past the response
  // window on a marginal link, turning a recoverable sync into a failed one.
  if (cfg.disableModemSleep) WiFi.setSleep(false);

  while (!_wnWifiUp()) {
    if (expired()) return timeoutAndReturn(false, false);
    _wnDoWifiBegin(cfg);
    unsigned long attemptStart = millis();
    while (!_wnWifiUp() && millis() - attemptStart < cfg.wifiAttemptTimeoutMs) {
      if (expired()) return timeoutAndReturn(false, false);
      fireProgress(false, false);
      delay(50);
    }
  }

  // DHCP settle — WL_CONNECTED fires at auth, but localIP() can still be 0.0.0.0.
  unsigned long dhcpStart = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - dhcpStart < 5000) {
    if (expired()) return timeoutAndReturn(true, false);
    fireProgress(true, false);
    delay(50);
  }

  if (hooks.onSuccess) hooks.onSuccess(true, false);

  // ----- NTP phase -----
  if (hooks.onStart) hooks.onStart(true, false);

  // Pre-warm DNS + ARP so the first outgoing NTP UDP packet isn't dropped
  // while ARP resolves the router's MAC.
  if (cfg.primaryNtpServer) {
    String s = *cfg.primaryNtpServer;
    s.trim();
    if (s.length() > 0) {
      IPAddress ip;
      for (byte i = 0; i < 3; i++) {
        if (WiFi.hostByName(s.c_str(), ip) && ip != IPAddress(0, 0, 0, 0)) break;
        delay(300);
      }
    }
  }

  // NTP sync loop — alternate primary/secondary on each failed attempt.
  bool useAlt = false;
  while (!ntp->ntpStat()) {
    if (expired()) return timeoutAndReturn(true, false);

    // WiFi may have dropped mid-sync — reconnect before next attempt.
    if (!_wnWifiUp()) {
      while (!_wnWifiUp()) {
        if (expired()) return timeoutAndReturn(false, false);
        _wnDoWifiBegin(cfg);
        unsigned long a = millis();
        while (!_wnWifiUp() && millis() - a < cfg.wifiAttemptTimeoutMs) {
          if (expired()) return timeoutAndReturn(false, false);
          fireProgress(false, false);
          delay(50);
        }
      }
    }

    String primary   = cfg.primaryNtpServer   ? *cfg.primaryNtpServer   : String();
    String alternate = cfg.alternateNtpServer ? *cfg.alternateNtpServer : String();
    primary.trim();
    alternate.trim();
    const String& srv = (useAlt && alternate.length() > 0) ? alternate : primary;
    if (srv.length() == 0) break;   // nothing to try; leave ntpStat false

    // First ntpBootFastRetryWindowMs of boot uses the fast cycle; after that,
    // back off to the runtime cycle so we don't flood the server if the NTP
    // path stays broken for a long boot stretch.
    //
    // Exception: when the caller bounds the boot with bootTotalTimeoutMs, stay
    // on the fast cycle for the WHOLE budget. The backoff was meant to avoid
    // flooding a forever-retrying boot — but a bounded boot reboots at the
    // deadline anyway, so a 5 s cycle for <=2 min isn't flooding, and backing
    // off to a 30 s cycle (longer than the 15 s per-attempt window → 1 packet
    // per attempt) starves the back half of the budget and turns a marginal
    // network into a reboot loop. Bounded boot: max robustness, deadline caps it.
    uint32_t fastWindow = cfg.ntpBootFastRetryWindowMs;
    if (cfg.bootTotalTimeoutMs > fastWindow) fastWindow = cfg.bootTotalTimeoutMs;
    uint32_t cycleMs = (millis() - bootStart < fastWindow)
                       ? cfg.ntpBootRetryCycleMs
                       : cfg.ntpRuntimeRetryCycleMs;
    _wnBeginNtp(ntp, srv.c_str(), cycleMs);
    ntp->forceUpdate();

    unsigned long ntpAttemptStart = millis();
    while (!ntp->ntpStat() && millis() - ntpAttemptStart < cfg.ntpAttemptTimeoutMs) {
      if (expired()) return timeoutAndReturn(true, false);
      fireProgress(true, false);
      ntp->update();
      delay(50);
    }
    if (!ntp->ntpStat()) {
      if (alternate.length() > 0) useAlt = !useAlt;
      delay(500);
    }
  }

  // Sync achieved — move to the caller's configured runtime interval.
  ntp->updateInterval(cfg.ntpRuntimeIntervalMs);
  if (hooks.onSuccess) hooks.onSuccess(true, true);
  return true;
}

void serviceWiFiAndNTP(const WiFiNTPConfig& cfg, const WiFiNTPHooks& hooks) {
  if (!cfg.ntpInstance) return;
  NTP2* ntp = cfg.ntpInstance;

  // 1) Pump NTP — cheap, safe to call every loop iteration.
  ntp->update();

  // 2) WiFi connection-state tracking + runtime reconnect.
  static bool          wasConnected = true;
  static unsigned long lastRetry    = 0;
  bool connected = _wnWifiUp();

  if (connected) {
    if (!wasConnected && hooks.onReconnected) hooks.onReconnected();
    wasConnected = true;
  } else {
    if (wasConnected) {
      wasConnected = false;
      if (hooks.onReconnecting) hooks.onReconnecting();
    }
    if (millis() - lastRetry >= cfg.wifiRuntimeRetryMs) {
      lastRetry = millis();
      _wnDoWifiBegin(cfg);
    }
  }

  // 3) NTP server rotation — if no successful sync in ntpRotateAfterMs,
  //    flip primary/alternate. Checked once per minute.
  //    Caller-owned Strings are read by reference (no copies, no trim) to
  //    avoid a per-minute heap alloc/free pair. Caller is expected to trim
  //    server names once at config time.
  static unsigned long lastRotCheck = 0;
  static bool          useAlt       = false;
  // Gate on a valid DHCP lease: _wnBeginNtp() below does a BLOCKING DNS resolve.
  // On a half-up link (WL_CONNECTED flickers true with no lease) that resolve
  // stalls the caller's loop for seconds; requiring localIP != 0 skips it until
  // the link can actually carry DNS.
  if (connected && WiFi.localIP() != IPAddress(0, 0, 0, 0) && millis() - lastRotCheck >= 60000) {
    lastRotCheck = millis();
    if (cfg.primaryNtpServer   && cfg.primaryNtpServer->length()   > 0 &&
        cfg.alternateNtpServer && cfg.alternateNtpServer->length() > 0) {
      uint32_t lastSync = ntp->timestamp();
      if (lastSync > 0 && (millis() - lastSync) > cfg.ntpRotateAfterMs) {
        useAlt = !useAlt;
        _wnBeginNtp(ntp,
          useAlt ? cfg.alternateNtpServer->c_str()
                 : cfg.primaryNtpServer->c_str(),
          cfg.ntpRuntimeRetryCycleMs);
        // _wnBeginNtp no longer touches updateInterval; reassert the runtime
        // cadence so future polls fire on cfg.ntpRuntimeIntervalMs rather than
        // whatever activeInterval got reset to after the rotation forceUpdate.
        ntp->updateInterval(cfg.ntpRuntimeIntervalMs);
        ntp->forceUpdate();
      }
    }
  }
}
