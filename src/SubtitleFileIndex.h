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
    SubtitleFileIndex(const std::wstring& filename, long long initialTime, std::function<bool()> cancelled);
    bool Resolve(long long offset, uint16_t packetId, uint64_t hash, Record& record);
private:
    bool ReadPacket();
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
    std::function<bool()> m_cancelled;
    CFilterDemuxerHandler m_handler;
    MmtTlv::MmtTlvDemuxer m_demuxer;
    std::vector<Record> m_records;
    std::vector<CFilterDemuxerHandler::SubtitleStreamInfo> m_controls;
};
