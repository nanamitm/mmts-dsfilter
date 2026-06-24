#include "SubtitleTimingResolver.h"
#include "DebugLog.h"

#define LogMsg MmtTlvLogInfo
#define LogDetail MmtTlvLogDebug

void SubtitleTimingResolver::Reset(bool awaitProgramStart)
{
    m_programStartRt.store(-1, std::memory_order_release);
    m_awaitProgramStart.store(awaitProgramStart, std::memory_order_release);
    m_ntpRt.store(-1, std::memory_order_release);
    m_ntpMediaRt.store(-1, std::memory_order_release);
    m_awaitNtp.store(false, std::memory_order_release);
    m_timeOffset.store(0, std::memory_order_release);
    m_lastTtmlBegin.store(-1, std::memory_order_release);
    m_offsetProgramStartRt.store(-1, std::memory_order_release);
    m_offsetUsesNtp.store(false, std::memory_order_release);
    m_offsetValid.store(false, std::memory_order_release);
}

int64_t SubtitleTimingResolver::OnProgramStart(int64_t programStartRt)
{
    const int64_t oldStart = m_programStartRt.exchange(programStartRt, std::memory_order_acq_rel);
    m_awaitProgramStart.store(false, std::memory_order_release);
    return oldStart;
}

void SubtitleTimingResolver::OnNtpAnchor(int64_t ntpRt, int64_t ntpMediaRt)
{
    m_ntpRt.store(ntpRt, std::memory_order_release);
    m_ntpMediaRt.store(ntpMediaRt, std::memory_order_release);
    m_awaitNtp.store(false, std::memory_order_release);
}

int64_t SubtitleTimingResolver::SourceTime(int64_t ttmlTime) const
{
    const int64_t start = m_programStartRt.load(std::memory_order_acquire);
    if (start >= 0)
        return start + ttmlTime;
    return ttmlTime;
}

int64_t SubtitleTimingResolver::ResolveOffset(int64_t ttmlBegin, int64_t sourceBegin, int64_t anchor,
                                              int64_t currentPtsForLog, int64_t currentDtsForLog,
                                              int64_t segmentStartForLog, int64_t segmentTimeOffsetForLog)
{
    const int64_t programStartRt = m_programStartRt.load(std::memory_order_acquire);
    const int64_t ntpRt = m_ntpRt.load(std::memory_order_acquire);
    const int64_t ntpMediaRt = m_ntpMediaRt.load(std::memory_order_acquire);
    const bool canUseNtp = (programStartRt >= 0 && ntpRt >= 0 && ntpMediaRt >= 0);

    int64_t offset = m_timeOffset.load(std::memory_order_acquire);
    bool valid = m_offsetValid.load(std::memory_order_acquire);
    bool usesNtp = m_offsetUsesNtp.load(std::memory_order_acquire);

    if (valid) {
        // Detect a TTML timeline reset (e.g. a program boundary). With the
        // cached offset, normal cue times can be far from the current delivered
        // video PTS, so only a clear TTML begin rollback is a resync signal.
        const int64_t candidate = sourceBegin - offset;
        const int64_t lastTtmlBegin = m_lastTtmlBegin.load(std::memory_order_acquire);
        const int64_t calibratedProgramStart = m_offsetProgramStartRt.load(std::memory_order_acquire);
        if (programStartRt >= 0 && calibratedProgramStart != programStartRt) {
            LogMsg(L"SUBTITLE timing resync: programStart changed old=%I64d ms, new=%I64d ms, candidateStart=%I64d ms, anchor=%I64d ms, ttmlBegin=%I64d ms\n",
                   calibratedProgramStart / 10000,
                   programStartRt / 10000,
                   candidate / 10000,
                   anchor / 10000,
                   ttmlBegin / 10000);
            valid = false;
        }
        if (canUseNtp && !usesNtp) {
            LogMsg(L"SUBTITLE timing resync: switching to NTP anchor oldOffset=%I64d ms, ntp=%I64d ms, ntpMedia=%I64d ms, ttmlBegin=%I64d ms\n",
                   offset / 10000,
                   ntpRt / 10000,
                   ntpMediaRt / 10000,
                   ttmlBegin / 10000);
            valid = false;
        }
        if (lastTtmlBegin >= 0 && ttmlBegin + kResyncBackToleranceRt < lastTtmlBegin) {
            LogMsg(L"SUBTITLE timing resync: oldOffset=%I64d ms, candidateStart=%I64d ms, anchor=%I64d ms, current=%I64d ms, programStart=%I64d ms, lastTtmlBegin=%I64d ms, ttmlBegin=%I64d ms\n",
                   offset / 10000,
                   candidate / 10000,
                   anchor / 10000,
                   currentPtsForLog / 10000,
                   programStartRt / 10000,
                   lastTtmlBegin / 10000,
                   ttmlBegin / 10000);
            valid = false;
        }
        // Note: a large *forward* gap between candidate and anchor is not by
        // itself a resync signal. Broadcast TTML cues are routinely delivered
        // well ahead of their display time (and a cut/edit point can widen
        // that gap further), so candidate >> anchor is expected and must not
        // be "corrected" -- doing so previously caused the offset to latch
        // onto a too-early cue, after which every later (correctly paced) cue
        // computed a start time in the past and was silently dropped.
    }

    if (!valid) {
        const int64_t baseAnchor = canUseNtp ? ntpMediaRt : anchor;
        offset = sourceBegin - baseAnchor;
        m_timeOffset.store(offset, std::memory_order_release);
        m_offsetProgramStartRt.store(programStartRt, std::memory_order_release);
        m_offsetUsesNtp.store(canUseNtp, std::memory_order_release);
        m_offsetValid.store(true, std::memory_order_release);
        LogMsg(L"SUBTITLE timing base set: sourceOffset=%I64d ms, anchor=%I64d ms, current=%I64d ms, currentDts=%I64d ms, segmentStart=%I64d ms, rapOffset=%I64d ms, programStart=%I64d ms, ntp=%I64d ms, ntpMedia=%I64d ms, useNtp=%d, ttmlBegin=%I64d ms, sourceBegin=%I64d ms\n",
               offset / 10000,
               baseAnchor / 10000,
               currentPtsForLog / 10000,
               currentDtsForLog / 10000,
               segmentStartForLog / 10000,
               segmentTimeOffsetForLog / 10000,
               programStartRt / 10000,
               ntpRt / 10000,
               ntpMediaRt / 10000,
               canUseNtp ? 1 : 0,
               ttmlBegin / 10000,
               sourceBegin / 10000);
    }

    m_lastTtmlBegin.store(ttmlBegin, std::memory_order_release);
    return offset;
}
