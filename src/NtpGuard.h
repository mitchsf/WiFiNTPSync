// NtpGuard.h
// Client-side NTP sanity guard for Arduino ESP32 clocks.
//
// Rejects implausible time steps (a glitch, or a bad/unsynced NTP server) while
// still letting a *genuine* shift through after it's confirmed. Comparison is in
// UTC, so it never false-trips on a DST boundary (UTC is continuous; only the
// local offset jumps). The first sync after boot is trusted — a fresh clock has
// no prior reference to judge against, same as any NTP client.
//
// Logic verified by a host-side test (one-off glitch -> reject + hold; sustained
// shift -> accept after 3; DST step -> accept, no false trip; backward glitch ->
// reject; bad first sync -> trusted then self-heals).
//
// Usage in an app's TimeLib sync provider:
//   NtpGuard ntpGuard;                       // one per app
//   time_t sTime() {
//     time_t e = (time_t)ntp.epoch();        // candidate UTC
//     if (e != 0 && ntpGuard.accept(e, millis())) return e + rOffset;  // adopt
//     return 0;                              // reject -> TimeLib holds current time
//   }
//   // The on-page aberration log was removed (no project displays it).
//   // logPtr()/log()/rejects() remain as no-ops for source compatibility.

#pragma once
#include <Arduino.h>
#include <TimeLib.h>

class NtpGuard {
public:
  // Candidate UTC epoch + millis(). Returns true to adopt, false to reject (hold).
  bool accept(time_t eTime, uint32_t nowMs) {
    if (lastTime_ == 0) { adopt(eTime, nowMs); return true; }   // first sync — trust it
    uint32_t elapsed = (nowMs - lastMs_) / 1000UL;
    long off = (long)(eTime - (lastTime_ + (time_t)elapsed));   // discrepancy, in UTC
    if (off >= -kPanic && off <= kPanic) { pendCnt_ = 0; adopt(eTime, nowMs); return true; }
    // aberration: only accept if the SAME large offset persists kPersistN times
    uint32_t pe = (nowMs - pendMs_) / 1000UL;
    long pd = (long)(eTime - (pendCand_ + (time_t)pe));
    if (pendCnt_ > 0 && pd >= -2 && pd <= 2) pendCnt_++;
    else { pendCand_ = eTime; pendMs_ = nowMs; pendCnt_ = 1; }
    bool ok = (pendCnt_ >= kPersistN);
    if (!ok) return false;                                      // reject — hold last good
    adopt(eTime, nowMs); return true;
  }

  // Logging was removed (no project displays it). These are retained as no-ops
  // so existing callers — liveForm.addHTML(ntpGuard.logPtr()) in several
  // projects — keep compiling and simply render nothing. Drop the addHTML call
  // from each project as it is next updated.
  String*       logPtr()       { static String empty; return &empty; }
  const String& log()    const { static const String empty; return empty; }
  uint32_t      rejects()      const { return 0; }

private:
  void adopt(time_t eTime, uint32_t nowMs) { lastTime_ = eTime; lastMs_ = nowMs; pendCnt_ = 0; }

  time_t   lastTime_ = 0;     // last accepted UTC epoch; 0 = never synced
  uint32_t lastMs_   = 0;
  time_t   pendCand_ = 0;     // suspected-step candidate awaiting persistence
  uint32_t pendMs_   = 0;
  uint8_t  pendCnt_  = 0;

  static const long    kPanic    = 60;   // reject UTC steps larger than this ...
  static const uint8_t kPersistN = 3;    // ... unless confirmed this many times
};
