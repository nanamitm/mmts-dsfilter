#include "SubtitleFileIndex.h"
#include "DebugLog.h"
#include <algorithm>

SubtitleFileIndex::SubtitleFileIndex(const std::wstring& filename, long long initialTime,
                                    std::function<bool()> cancelled, long long readThrough,
                                    long long restartLead)
    : m_file(filename, std::ios::binary), m_arrival(initialTime),
      m_readThrough((std::max)(readThrough, 1LL)), m_restartLead((std::max)(restartLead, 0LL)),
      m_cancelled(std::move(cancelled))
{
    m_handler.setVideoCallback([this](int, bool, long long pts, long long dts, bool, bool, const uint8_t*, size_t) {
        if (dts >= 0 || pts >= 0) {
            m_arrival = dts >= 0 ? dts : pts;
            m_hasVideoClock = true;
        }
        if (pts >= 0) m_lastPts = (std::max)(m_lastPts, pts);
    });
    m_handler.setNtpCallback([this](long long ntp) { if (!m_hasVideoClock) m_arrival = ntp; });
    m_handler.setSubtitleControlCallback([this](const auto& streams) { Controls(streams); });
    m_handler.setSubtitleDocumentCallback([this](const auto& info, long long, long long, const uint8_t* data, size_t size) {
        try {
            const auto doc = DsTtml::Parse(std::string(reinterpret_cast<const char*>(data), size), DsSubtitle::Synchronized(info.control));
            Record record;
            record.offset = m_packetOffset;
            record.packetId = info.packetId;
            record.hash = DsSubtitle::Hash(data, size);
            record.control = info.control;
            record.arrival = m_arrival;
            const auto reference = DsSubtitle::ReferenceTime(info.control);
            record.base = reference.value_or(m_arrival);
            record.start = m_arrival;
            if (!doc.divTags.empty()) {
                const auto& div = doc.divTags.front();
                if (div.begin) record.start = record.base + static_cast<long long>(*div.begin) * 10000;
                if (div.end) record.stop = record.base + static_cast<long long>(*div.end) * 10000;
            }
            record.clear = doc.divTags.empty() || std::all_of(doc.divTags.begin(), doc.divTags.end(), [](const auto& div) {
                return std::all_of(div.pTags.begin(), div.pTags.end(), [](const auto& p) {
                    return std::all_of(p.spanTags.begin(), p.spanTags.end(), [](const auto& span) { return span.text.empty(); });
                });
            });
            if (record.clear) record.start = m_arrival;
            m_records.push_back(std::move(record));
        } catch (const std::exception& e) {
            MmtTlvLogInfo(L"SUBTITLE index rejected document at byte=%I64d: %S\n", m_packetOffset, e.what());
        }
    });
    m_demuxer.setDemuxerHandler(m_handler);
}

void SubtitleFileIndex::Controls(const std::vector<CFilterDemuxerHandler::SubtitleStreamInfo>& streams)
{
    for (const auto& old : m_controls) {
        const auto current = std::find_if(streams.begin(), streams.end(), [&](const auto& s) { return s.packetId == old.packetId; });
        if (current != streams.end() && current->componentTag == old.componentTag && DsSubtitle::SameControl(current->control, old.control))
            continue;
        Record record;
        record.offset = m_packetOffset;
        record.packetId = old.packetId;
        record.controlUpdate = true;
        record.start = record.arrival = m_arrival;
        m_records.push_back(record);
    }
    m_controls = streams;
}

// Start a new window shortly before offset, as the splitter does on a seek:
// the demuxer keeps its stream registration but drops partial MFUs.
void SubtitleFileIndex::Restart(long long offset)
{
    const long long start = (std::max)(0LL, offset - m_restartLead);
    MmtTlvLogInfo(L"SUBTITLE index restarts at byte=%I64d for byte=%I64d (window was %I64d-%I64d)\n",
                  start, offset, m_windowStart, m_bufferOffset + static_cast<long long>(m_consumed));
    m_file.clear();
    m_file.seekg(start);
    m_buffer.clear();
    m_consumed = 0;
    m_bufferOffset = start;
    m_packetOffset = start;
    m_eof = false;
    m_records.clear();
    m_demuxer.resetStreams();
    m_arrival = -1;
    m_lastPts = -1;
    m_hasVideoClock = false;
    m_windowStart = start;
}

bool SubtitleFileIndex::ReadPacket()
{
    constexpr size_t chunk = 1024 * 1024;
    if (!m_file.is_open() || m_cancelled()) return false;
    if (m_consumed > 0 && (m_consumed >= chunk || m_consumed == m_buffer.size())) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + m_consumed);
        m_bufferOffset += static_cast<long long>(m_consumed);
        m_consumed = 0;
    }
    for (;;) {
        if (m_cancelled()) return false;
        if (m_buffer.size() == m_consumed && m_eof) return false;
        MmtTlv::Common::ReadStream bytes(m_buffer);
        bytes.skip(m_consumed);
        m_packetOffset = m_bufferOffset + static_cast<long long>(m_consumed);
        const auto status = m_demuxer.demux(bytes);
        m_consumed = m_buffer.size() - bytes.leftBytes();
        if (status != MmtTlv::DemuxStatus::NotEnoughBuffer) return true;
        if (m_eof) return false;
        const size_t old = m_buffer.size();
        m_buffer.resize(old + chunk);
        m_file.read(reinterpret_cast<char*>(m_buffer.data() + old), chunk);
        const auto got = m_file.gcount();
        m_bytesRead += got;
        m_buffer.resize(old + static_cast<size_t>(got));
        m_eof = got == 0;
    }
}

bool SubtitleFileIndex::Resolve(long long offset, uint16_t packetId, uint64_t hash, Record& record)
{
    const long long position = m_bufferOffset + static_cast<long long>(m_consumed);
    if (offset < m_windowStart || offset > position + m_readThrough) {
        Restart(offset);
    }
    size_t cursor = 0;
    std::optional<size_t> found;
    std::optional<long long> boundary;
    for (;;) {
        while (cursor < m_records.size()) {
            const auto& next = m_records[cursor];
            if (!found) {
                if (!next.controlUpdate && next.offset == offset && next.packetId == packetId && next.hash == hash)
                {
                    found = cursor;
                    boundary = next.stop;
                }
            } else if (next.packetId == packetId) {
                // A clear/control update acts at arrival; a new timed document
                // replaces the old one at its begin, even when begin is zero.
                const long long update = (std::max)(next.start, next.arrival);
                boundary = boundary ? (std::min)(*boundary, update) : update;
            }
            ++cursor;
        }
        // Read through the tentative end: a subsequent clear can supersede a
        // future replacement received earlier in the file. Finite documents
        // only need lookahead over their own display interval.
        if (found && boundary && m_arrival >= *boundary) {
            record = m_records[*found];
            record.stop = boundary;
            return true;
        }
        if (!ReadPacket()) {
            if (m_cancelled() || !found) return false;
            record = m_records[*found];
            // No next document: hold to the last observed media timestamp.
            const long long end = (std::max)(m_arrival, m_lastPts);
            record.stop = boundary ? (std::min)(*boundary, end) : end;
            return true;
        }
    }
}
