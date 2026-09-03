#pragma once
// TTML subtitle timeline -> media-clock anchoring, extracted from
// CMmtTlvSplitter so the exact same per-program calibration/resync logic can
// be replayed by offline diagnostic tools (see tools/) without pulling in
// DirectShow/COM.
//
// Time values are 100ns units (DirectShow REFERENCE_TIME convention), passed
// as plain int64_t so this header has no DirectShow dependency.
#include <atomic>
#include <cstdint>

class SubtitleTimingResolver {
public:
    // awaitProgramStart mirrors the caller's "we already know we'll need a
    // fresh EIT before trusting cues" decision (e.g. seeking to pos > 0).
    void Reset(bool awaitProgramStart = false);

    // Returns the previous program-start value (-1 if none was known), so the
    // caller can log a change the same way CMmtTlvSplitter did.
    int64_t OnProgramStart(int64_t programStartRt);
    void OnNtpAnchor(int64_t ntpRt, int64_t ntpMediaRt);

    bool AwaitingProgramStart() const { return m_awaitProgramStart.load(std::memory_order_acquire); }
    void SetAwaitingProgramStart(bool wait) { m_awaitProgramStart.store(wait, std::memory_order_release); }
    bool AwaitingNtp() const { return m_awaitNtp.load(std::memory_order_acquire); }
    void SetAwaitingNtp(bool wait) { m_awaitNtp.store(wait, std::memory_order_release); }

    int64_t ProgramStart() const { return m_programStartRt.load(std::memory_order_acquire); }

    // programStart + ttmlTime (or ttmlTime alone if programStart isn't known yet).
    int64_t SourceTime(int64_t ttmlTime) const;

    // Resolves (and, at most once per program/resync, recalibrates) the
    // offset mapping subtitle source time -> normalized media time. This was
    // modelled on dantto4k's one-shot-per-program PCR calibration, which
    // dantto4k no longer has: its TTML/B24 rewrite dropped the EIT-derived
    // calibration, and it now anchors each TTML document on the
    // reference_start_time carried in the subtitle descriptor. The two timing
    // models have diverged - this one is ours to reason about on its own.
    // `anchor` is the current media-time anchor (video DTS/PTS elapsed). The
    // *ForLog params only feed the diagnostic log line and have no effect on
    // the result.
    int64_t ResolveOffset(int64_t ttmlBegin, int64_t sourceBegin, int64_t anchor,
                          int64_t currentPtsForLog, int64_t currentDtsForLog,
                          int64_t segmentStartForLog, int64_t segmentTimeOffsetForLog);

private:
    static constexpr int64_t kResyncBackToleranceRt = 5 * 10000000LL; // 5s, 100ns units

    std::atomic<int64_t> m_programStartRt{-1};
    std::atomic<bool> m_awaitProgramStart{false};
    std::atomic<int64_t> m_ntpRt{-1};
    std::atomic<int64_t> m_ntpMediaRt{-1};
    std::atomic<bool> m_awaitNtp{false};
    std::atomic<int64_t> m_timeOffset{0};
    std::atomic<int64_t> m_lastTtmlBegin{-1};
    std::atomic<int64_t> m_offsetProgramStartRt{-1};
    std::atomic<bool> m_offsetUsesNtp{false};
    // Whether m_timeOffset holds a calibrated value. A separate flag is
    // required because a legitimate offset can be negative (TTML begin <
    // media PTS), so a "< 0 means unset" sentinel would mis-fire on every cue.
    std::atomic<bool> m_offsetValid{false};
};
