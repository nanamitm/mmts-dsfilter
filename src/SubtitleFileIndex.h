#pragma once
#include "DsHandler.h"
#include "TtmlModel.h"
#include "SubtitleDocumentState.h"
#include "mmtTlvDemuxer.h"
#include <fstream>
#include <functional>

// Incremental second reader: read each byte at most once to learn the next
// document/clear/control update before submitting irrevocable ASS samples.
// Keep only timing records, not video, XML documents, or glyph resources.
//
// It indexes one contiguous window of the file. A document inside the window,
// or a short way past its end, is resolved from there; one far ahead of it or
// before it (a seek) restarts the window just before the document, so a seek
// costs a little more than the document's own lookahead instead of reading
// everything in between.
class SubtitleFileIndex {
public:
    struct Record {
        long long offset{};
        uint16_t packetId{};
        uint64_t hash{};
        bool controlUpdate{};
        bool clear{};
        long long arrival{-1}; // absolute Unix media clock, 100ns
        long long base{-1};
        long long start{-1};
        std::optional<long long> stop;
        DsSubtitle::Control control;
    };
    // readThrough: how far past the window a document may be and still be read
    // up to rather than jumped to. restartLead: how far before the document a
    // restart begins, so that an MPT and a video timestamp have come by then
    // (the MPT repeats about every 100 ms; 8 MiB is a few seconds of 4K).
    static constexpr long long kDefaultReadThrough = 64LL * 1024 * 1024;
    static constexpr long long kDefaultRestartLead = 8LL * 1024 * 1024;
    SubtitleFileIndex(const std::wstring& filename, long long initialTime, std::function<bool()> cancelled,
                      long long readThrough = kDefaultReadThrough, long long restartLead = kDefaultRestartLead);
    bool Resolve(long long offset, uint16_t packetId, uint64_t hash, Record& record);
    // Bytes read from the file so far, for tests.
    long long BytesRead() const { return m_bytesRead; }
private:
    bool ReadPacket();
    void Restart(long long offset);
    void Controls(const std::vector<CFilterDemuxerHandler::SubtitleStreamInfo>& streams);
    std::ifstream m_file;
    std::vector<uint8_t> m_buffer;
    size_t m_consumed{};
    long long m_bufferOffset{};
    long long m_packetOffset{};
    long long m_arrival{-1};
    long long m_lastPts{-1};
    bool m_hasVideoClock{};
    bool m_eof{};
    long long m_windowStart{};
    long long m_readThrough{};
    long long m_restartLead{};
    long long m_bytesRead{};
    std::function<bool()> m_cancelled;
    CFilterDemuxerHandler m_handler;
    MmtTlv::MmtTlvDemuxer m_demuxer;
    std::vector<Record> m_records;
    std::vector<CFilterDemuxerHandler::SubtitleStreamInfo> m_controls;
};
