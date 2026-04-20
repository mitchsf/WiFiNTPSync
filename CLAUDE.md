# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`WiFiNTPSync` is an Arduino library for ESP32 that gives clock/IoT sketches two network-lifecycle entry points:

- `bootWiFiAndNTP(cfg, hooks)` — blocking, called once from `setup()`. Connects WiFi, settles DHCP, pre-warms DNS, syncs NTP with primary/alternate rotation. Loops forever by default; returns `false` only if the optional `bootTotalTimeoutMs` expires.
- `serviceWiFiAndNTP(cfg, hooks)` — non-blocking, called every iteration from `loop()`. Pumps `ntp.update()`, reconnects WiFi if dropped, rotates to the alternate NTP server if no successful sync within `ntpRotateAfterMs`.

Both functions take the same `const WiFiNTPConfig&` and `const WiFiNTPHooks&` — a deliberate symmetric shape. See `examples/Basic/Basic.ino` for the minimal call-site pattern.

## Build / test

There is no standalone build or test harness — this is a consumable Arduino library. Validation happens by compiling a consumer sketch against it:

```bash
ACLI="/c/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
FQBN="esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=min_spiffs"
"$ACLI" compile --fqbn "$FQBN" --libraries "C:/Users/Mitch/OneDrive/Projects/libraries" <sketch-path>
```

`architectures=esp32` in `library.properties` — the implementation uses the ESP32 core's WiFi API, Arduino `String`, and the `NTP2` library. It will not compile for AVR/ESP8266 without porting.

## Architecture (what requires reading across files to see)

**No globals, no assumed symbols.** The caller owns the `NTP2` instance and passes it via `cfg.ntpInstance`. The library uses `cfg.ntpInstance->update()` / `->forceUpdate()` / `->ntpStat()` everywhere — never a bare `ntp.X()`. If you add code, do not introduce file-scope `ntp` references. This is what allows the library to coexist with any caller's naming or multiple instances.

**Config uses `const String*`, not `String`.** `cfg.ssid`, `cfg.password`, `cfg.primaryNtpServer`, `cfg.alternateNtpServer` are all pointers to the caller's NVS-backed `String` variables. This is intentional: when the user updates SSID or server via a live form, the next `serviceWiFiAndNTP()` call reads the new value with no reconfigure step. Don't change these to by-value or you break live-update semantics.

**Hooks struct is the entire side-effect surface.** The library has no opinion on displays, buttons, watchdogs, LEDs, or logging. Every project-specific effect goes through one of the `WiFiNTPHooks` function pointers:
- `onStart(wifi, ntp)` — fires at phase transition (WiFi→NTP→done)
- `onProgress(wifi, ntp, elapsedMs)` — fires during polling; `cfg.progressIntervalMs` throttles it (0 = every iteration)
- `onSuccess(wifi, ntp)` — fires at phase completion
- `onReconnecting` / `onReconnected` — runtime-only
- `onBootTimeout(wifi, ntp)` — fires once if `cfg.bootTotalTimeoutMs > 0` and expires; paired with `false` return

All are `nullptr` by default and null-checked before call.

**NTP2 timing overrides are load-bearing.** In `_wnBeginNtp()`:
- `responseDelay(1600)` — NTP2's 250 ms default is too tight for real internet RTT over a fresh WiFi association; first responses often arrive at 400–1500 ms.
- `retryDelay(1000)` — NTP2's 30 s default means only one packet gets sent per attempt window; 1 s lets multiple send/check cycles fit.

Do not "simplify" these values without reading the `NTP2 library timing values` section of the user's global `CLAUDE.md` — they are the result of significant field-tuning. Same for the 2-second DHCP-settle and 3×300ms DNS pre-warm in `bootWiFiAndNTP()`.

**`ntp->begin()` is called on every sync attempt, not once.** Each call re-resolves DNS, which rotates to a fresh anycast IP from pools like `time.cloudflare.com`. A bad first resolve (or a stale cached IP from a pre-WiFi resolve) would otherwise poison every subsequent `forceUpdate()`. If you refactor the boot loop, preserve the per-attempt `_wnBeginNtp()` call.

**Boot loops forever by default.** `bootTotalTimeoutMs = 0` means "never give up." This matches the intent that a configured clock should keep trying rather than drop into a fallback UI. AP mode / error display is the *caller's* decision — opt in by setting `bootTotalTimeoutMs > 0` and providing `onBootTimeout`.

**Service call internal state.** `serviceWiFiAndNTP()` uses three function-local `static` variables (`wasConnected`, `lastRetry`, `lastRotCheck`, `useAlt`). This is fine for a single service instance, which is the only supported use case. Don't call it concurrently from multiple tasks — there is no locking.

**Runtime cadences.**
- `ntp->update()` — every call (cheap, non-blocking, safe)
- WiFi reconnect attempt — once per `cfg.wifiRuntimeRetryMs` (default 60s) while disconnected
- NTP rotation check — once per 60s (hardcoded; checks `millis() - ntp->timestamp() > cfg.ntpRotateAfterMs`)

**Two failure modes worth knowing.**
1. `cfg.ntpInstance == nullptr` — both functions return immediately (`false` / void). Fail-safe against misuse.
2. `cfg.primaryNtpServer` is nullptr or empty AND alternate is also empty/nullptr — the NTP loop `break`s without success. Returns `true` from boot only if `ntpStat()` happened to be set before; otherwise the behavior is "WiFi up, clock unsynced." Callers that need time should validate `ntpInstance->ntpStat()` after boot.

## Conventions for new code

- Internal helpers: `static` and `_wn` prefix (`_wnWifiUp`, `_wnDoWifiBegin`, `_wnBeginNtp`) to keep them confined to the translation unit.
- Every new `WiFiNTPConfig` field needs an in-struct default that preserves current behavior — callers never have to initialize fields they don't care about.
- Every new hook is optional (`nullptr` default) and called through a null check.
- Do not add logging (`Serial.print`) in the library. Callers can log from hooks.
