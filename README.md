# WiFiNTPSync

An Arduino library for ESP32 that wraps WiFi connection and NTP time sync into two clean entry points — one blocking (for `setup()`), one non-blocking (for `loop()`) — with all display and side-effect behavior delegated to the caller via optional hooks.

Built on top of the [NTP2](https://github.com/feigmd/NTP2) NTP client library. The library itself has no opinions about displays, LEDs, buttons, or logging — every project plugs in its own behavior through a small set of function pointers.

## Why this exists

Every ESP32 clock project ends up re-implementing the same logic: connect to WiFi, wait for DHCP, pre-warm DNS, sync NTP (with alternate-server failover), handle runtime reconnects, and periodically re-sync. The details are painful to get right — NTP2's default timing is too tight for real internet RTT over a fresh WiFi link, DNS has to be re-resolved on every sync attempt to rotate anycast IPs, and so on. WiFiNTPSync captures that dialed-in logic in one place so fixes propagate to every project that uses it.

## Installation

### Arduino IDE (manual)

1. Clone or download this repository.
2. Copy the `WiFiNTPSync/` folder into your Arduino sketchbook's `libraries/` folder.
3. Restart the Arduino IDE.

### arduino-cli

```bash
arduino-cli lib install --git-url https://github.com/<user>/WiFiNTPSync.git
```

### Dependency

Requires the [NTP2](https://github.com/feigmd/NTP2) library. Install it the same way.

## Quick start

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTP2.h>
#include <WiFiNTPSync.h>

String mySsid         = "YourSSID";
String myPassword     = "YourPassword";
String myNtpServer    = "time.cloudflare.com";
String myNtpServer2   = "time.google.com";

WiFiUDP wifiUDP;
NTP2    ntp(wifiUDP);

WiFiNTPConfig netCfg;
WiFiNTPHooks  netHooks;

void setup() {
  Serial.begin(115200);

  netCfg.ntpInstance        = &ntp;
  netCfg.ssid               = &mySsid;
  netCfg.password           = &myPassword;
  netCfg.primaryNtpServer   = &myNtpServer;
  netCfg.alternateNtpServer = &myNtpServer2;

  netHooks.onProgress = [](bool wifi, bool, uint32_t ms) {
    Serial.print(wifi ? "NTP..." : "WiFi...");
    Serial.println(ms);
  };

  bootWiFiAndNTP(netCfg, netHooks);   // blocks until connected and synced
}

void loop() {
  serviceWiFiAndNTP(netCfg, netHooks);
  // ... your app logic ...
}
```

## API reference

### `bool bootWiFiAndNTP(const WiFiNTPConfig& cfg, const WiFiNTPHooks& hooks)`

Blocking. Call once from `setup()` after NVS/prefs have populated the strings that `cfg.ssid`, `cfg.password`, etc. point at.

Flow:
1. `WiFi.mode(WIFI_STA)`, then retry WiFi connection loops forever (unless `bootTotalTimeoutMs` is set) with per-attempt re-calls to `WiFi.begin()` — a changed BSSID or stuck state machine can't strand you permanently.
2. DHCP settle — waits up to 5 s for a real IP (not `0.0.0.0`) after authentication.
3. DNS pre-warm — three lookups of the primary NTP server (300 ms between) populates ARP/DNS caches so the first outgoing UDP packet isn't dropped waiting for the router MAC.
4. NTP sync loop — alternates primary/secondary on each failed attempt. Each attempt re-calls `ntp.begin()` to re-resolve DNS (rotates anycast IPs) and applies a 1.6 s response window and 1 s retry delay (the library's defaults are too tight for boot-time internet sync).
5. On success, switches NTP's update interval to `ntpRuntimeIntervalMs` with ±5 min jitter.

Returns `true` on full success. Returns `false` **only** if `cfg.bootTotalTimeoutMs > 0` and that budget expires before both phases complete.

### `void serviceWiFiAndNTP(const WiFiNTPConfig& cfg, const WiFiNTPHooks& hooks)`

Non-blocking. Call every iteration from `loop()`.

Each call:
1. **`ntp.update()`** — cheap UDP poll; pumps any pending NTP response.
2. **WiFi monitor** — if disconnected, calls `WiFi.begin()` at most once per `cfg.wifiRuntimeRetryMs`. Fires `onReconnecting` on the first disconnect, `onReconnected` on reconnect.
3. **NTP server rotation** — once per 60 s, checks `millis() - ntp.timestamp()`. If the gap exceeds `cfg.ntpRotateAfterMs`, flips to the alternate server and issues a `forceUpdate()`. Next flip can go back.

Safe to call every loop iteration — the expensive bits are self-throttled.

### `struct WiFiNTPConfig`

All fields have sensible defaults; set only what you need.

| Field | Type | Default | Purpose |
|---|---|---|---|
| `ntpInstance` | `NTP2*` | `nullptr` | **Required.** Pointer to the caller's NTP2 instance. |
| `ssid` | `const String*` | `nullptr` | **Required.** Pointer to caller's SSID string (mutable state works — see notes). |
| `password` | `const String*` | `nullptr` | Pointer to password. Empty string = open network. |
| `primaryNtpServer` | `const String*` | `nullptr` | **Required.** Pointer to primary NTP server string (e.g. `time.cloudflare.com`). |
| `alternateNtpServer` | `const String*` | `nullptr` | Optional. Pointer to alternate server; `nullptr` or empty disables rotation. |
| `wifiAttemptTimeoutMs` | `uint32_t` | `15000` | Per-attempt WiFi connection timeout before re-calling `WiFi.begin()`. |
| `ntpAttemptTimeoutMs` | `uint32_t` | `15000` | Per-attempt NTP poll window. |
| `ntpRuntimeIntervalMs` | `uint32_t` | `14400000` (4 h) | Applied to NTP2 after first successful sync. Jitter of ±5 min added. |
| `ntpRotateAfterMs` | `uint32_t` | `7200000` (2 h) | Runtime failover threshold: if last successful sync is older than this, swap servers. |
| `wifiRuntimeRetryMs` | `uint32_t` | `60000` | Minimum gap between runtime reconnect attempts. |
| `bootTotalTimeoutMs` | `uint32_t` | `0` | `0` = unlimited (default). Set `>0` to return `false` after this total boot budget. |
| `progressIntervalMs` | `uint32_t` | `0` | `0` = fire `onProgress` on every 50 ms poll iteration. Set `>0` to throttle. |

**Why pointers, not values:** the `String*` fields point into the caller's NVS-backed settings. When the user updates the SSID or NTP server through a live form, the next `serviceWiFiAndNTP()` call reads the new value with zero reconfiguration — no struct rebuild, no tear-down.

### `struct WiFiNTPHooks`

All hooks are optional (`nullptr` default) and null-checked before call.

| Hook | Signature | When it fires |
|---|---|---|
| `onStart` | `void(bool wifiConnected, bool ntpSynced)` | Once per phase at phase entry. Good for kicking off an animation or playing a tone. |
| `onProgress` | `void(bool wifiConnected, bool ntpSynced, uint32_t elapsedMs)` | During poll loops. Throttled by `cfg.progressIntervalMs`. `elapsedMs` is time since boot start. |
| `onSuccess` | `void(bool wifiConnected, bool ntpSynced)` | Phase complete. Fires twice during boot: once after WiFi+DHCP (`true,false`), once after NTP (`true,true`). |
| `onReconnecting` | `void()` | Runtime only. On transition from connected→disconnected. |
| `onReconnected` | `void()` | Runtime only. On transition from disconnected→connected. |
| `onBootTimeout` | `void(bool wifiConnected, bool ntpSynced)` | Once, immediately before `bootWiFiAndNTP()` returns `false`. State flags indicate how far boot got. |

Typical use: the display code lives entirely in `onProgress`. The library never touches pixels, LEDs, tubes, OLED buffers, etc. — if you want a spinner during connect, a blinking indicator, a digit animation, a scrolling message, they all plug in here.

## Recipes

### Animate a display during boot

```cpp
netHooks.onProgress = [](bool wifi, bool ntpOk, uint32_t elapsedMs) {
  if (!wifi)      showMessage("WiFi...");       // connecting
  else if (!ntpOk) showMessage("NTP...");       // connected, syncing
};
```

### Throttle progress to 2 Hz (for a slow display)

```cpp
netCfg.progressIntervalMs = 500;
```

### Boot-time timeout with fallback to AP mode

```cpp
netCfg.bootTotalTimeoutMs = 120000;   // 2 minutes total budget
netHooks.onBootTimeout = [](bool wifi, bool ntpOk) {
  Serial.println("Giving up; entering AP setup");
};

if (!bootWiFiAndNTP(netCfg, netHooks)) {
  startAccessPointMode();   // caller's fallback
}
```

### Restart HTTP server / mDNS on reconnect

```cpp
netHooks.onReconnecting = []() { Serial.println("WiFi dropped"); };
netHooks.onReconnected  = []() {
  server.begin();
  MDNS.begin("my-clock");
  ntp.forceUpdate();   // re-sync immediately rather than waiting
};
```

### Feed a software watchdog during the boot wait

```cpp
netHooks.onProgress = [](bool, bool, uint32_t) {
  feedWatchdog();        // application-specific watchdog feed
  button1.tick();        // keep button handler responsive
  button2.tick();
};
```

### Faster sync on a short-uptime device

```cpp
netCfg.ntpRuntimeIntervalMs = 1800000;   // re-sync every 30 min instead of every 4 h
netCfg.ntpRotateAfterMs     =  900000;   // flip servers after 15 min without sync
```

## Behavior details

**WiFi never gives up by default.** With `bootTotalTimeoutMs = 0` (the default), `bootWiFiAndNTP()` loops forever on WiFi failure — the intent is that a configured device should keep trying rather than drop into a fallback UI. Opt in to timeout by setting `bootTotalTimeoutMs > 0`.

**Runtime reconnect uses the same SSID/password pointers.** If the user changes credentials via a live form, the next runtime reconnect attempt uses the new values automatically. No reconfigure step.

**NTP timing overrides are load-bearing.** The library applies `responseDelay(1600)` and `retryDelay(1000)` to NTP2. The library defaults (250 ms / 30 s) are too tight for real internet RTT over a fresh WiFi link. These values are the result of field tuning — don't change them without testing against real networks.

**The 2-hour rotation is separate from the 4-hour sync.** Normal operation re-syncs every ~4 h (`ntpRuntimeIntervalMs`). The 2-hour rotation (`ntpRotateAfterMs`) is a *failover trigger* — if the normal sync hasn't happened in 2 h, we assume the current server is bad and flip.

**No locking.** The service call keeps internal `static` state for connection tracking and throttling. One service instance per program. Do not call `serviceWiFiAndNTP()` concurrently from multiple FreeRTOS tasks without your own mutex.

**Failure modes.**
- `cfg.ntpInstance == nullptr` — both functions return immediately (`false` / void) as a fail-safe.
- Both NTP servers empty/unset — the NTP phase of boot exits without success; `bootWiFiAndNTP()` returns `true` only if `ntpStat()` happened to already be set. Callers that require time should validate `ntp.ntpStat()` after boot.

## Design notes

- **No globals, no assumed symbols.** The caller owns the NTP2 instance. The library works with multiple NTP instances or unusual naming.
- **Hooks are the entire side-effect surface.** The library has no `Serial.print`, no LED code, no button handling. If you want logging, add it in a hook.
- **All config fields have defaults.** Callers only override what they need; missing fields preserve current behavior.

## Supported platforms

ESP32 only (`architectures=esp32`). Uses the ESP32 Arduino core's WiFi API. Porting to other WiFi-capable platforms (ESP8266, RP2040-W) would require adjusting `WiFi.h` calls but is otherwise straightforward.

## License

MIT — see `LICENSE`.
