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
//   // on the live form: liveForm.addHTML(ntpGuard.logPtr());

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
    logAberration(off, ok);
    if (!ok) return false;                                      // reject — hold last good
    adopt(eTime, nowMs); return true;
  }

  String*       logPtr()       { return &log_; }   // for WebPanel addHTML()
  const String& log()    const { return log_; }
  uint32_t      rejects()      const { return rejects_; }

private:
  void adopt(time_t eTime, uint32_t nowMs) { lastTime_ = eTime; lastMs_ = nowMs; pendCnt_ = 0; }

  void logAberration(long off, bool accepted) {
    if (!accepted) rejects_++;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d off=%+lds %s<br>",
             hour(), minute(), second(), off, accepted ? "ACCEPT" : "REJECT");
    log_ = String(buf) + log_;                       // newest first
    int n = 0, idx = 0, p;                            // prune to kLogMax entries
    while (n < kLogMax && (p = log_.indexOf("<br>", idx)) >= 0) { idx = p + 4; n++; }
    if (n >= kLogMax) log_.remove(idx);
  }

  time_t   lastTime_ = 0;     // last accepted UTC epoch; 0 = never synced
  uint32_t lastMs_   = 0;
  time_t   pendCand_ = 0;     // suspected-step candidate awaiting persistence
  uint32_t pendMs_   = 0;
  uint8_t  pendCnt_  = 0;
  uint32_t rejects_  = 0;
  String   log_;             // aberration log (newest first, RAM only)

  static const long    kPanic    = 60;   // reject UTC steps larger than this ...
  static const uint8_t kPersistN = 3;    // ... unless confirmed this many times
  static const int     kLogMax   = 20;   // prune log past this many entries
};
