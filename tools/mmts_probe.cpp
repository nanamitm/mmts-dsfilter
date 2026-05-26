// mmts_probe.cpp - MMTS file structure diagnostic tool
// Compile with the same include paths as the main project
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

// ---- Minimal TLV/MMTP parser ----
// TLV sync byte
static const uint8_t TLV_SYNC = 0x7F;

struct TlvPacket {
    uint8_t  type;
    uint16_t length;
    std::vector<uint8_t> data;
};

// Try to read one TLV packet; returns false if not enough data or no sync
static bool readTlv(const uint8_t* buf, size_t sz, size_t& pos, TlvPacket& pkt)
{
    // Find sync byte
    while (pos < sz && buf[pos] != TLV_SYNC) ++pos;
    if (pos + 4 > sz) return false;
    // pos is on 0x7F
    uint8_t type = buf[pos + 1];
    uint16_t len = (uint16_t)((buf[pos + 2] << 8) | buf[pos + 3]);
    if (pos + 4 + len > sz) return false;
    pkt.type = type;
    pkt.length = len;
    pkt.data.assign(buf + pos + 4, buf + pos + 4 + len);
    pos += 4 + len;
    return true;
}

// MMTP packet parsing (minimal)
// Header Compressed IP packet:
//   [2] context_id(12) + seq_num(4)
//   [1] header_type
//   if header_type == 0x20: IPv6(40) + UDP(8) then MMTP payload
//   otherwise: MMTP payload directly
struct MmtpInfo {
    uint16_t packetId;
    uint8_t  payloadType; // 0x00=MPU, 0x02=signaling, else other
    bool     rapFlag;
    uint32_t mpuSeqNum;
    bool     hasMpuSeq;
    int64_t  pts;         // in 90kHz (from MPU timestamp), -1 if unknown
    bool     isFirstMfu;
};

// Read big-endian u32
static uint32_t rbe32(const uint8_t* p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint16_t rbe16(const uint8_t* p)
{
    return (uint16_t)((p[0]<<8)|p[1]);
}

// Parse MMTP packet from raw MMTP bytes; return false if too short
static bool parseMmtp(const uint8_t* d, size_t sz, MmtpInfo& info)
{
    if (sz < 8) return false;
    // Byte 0: V(2) + C(1) + FEC(2) + r(1) + X(1) + RAP(1)
    info.rapFlag   = (d[0] & 0x01) != 0;
    // Bytes 2-3: packet_id
    info.packetId  = rbe16(d + 2);
    // Byte 1: packet_counter_flag(1) + fec_type(2) + r(1) + X(1) + ...
    // payload_type is bits [4:6] of byte 4? Let's look at byte 4
    // Actually MMTP header:
    //   Byte0: version(2) packet_counter_flag(1) fec_type(2) reserved(1) extension_header_flag(1) RAP_flag(1)
    //   Byte1: reserved(4) payload_type(4)
    //   Byte2-3: packet_id
    //   Byte4-7: timestamp
    //   [if packet_counter_flag] byte8-11: packet_counter
    //   [if extension_header_flag] extension headers
    //   Then payload
    uint8_t  flags       = d[0];
    bool     hasPktCtr   = (flags >> 5) & 1;
    bool     hasExtHdr   = (flags >> 1) & 1;
    info.rapFlag         = (flags & 1) != 0;
    info.payloadType     = d[1] & 0x0F;
    // skip fixed header (8 bytes)
    size_t off = 8;
    // skip packet counter (4 bytes) if present
    if (hasPktCtr) { if (off + 4 > sz) return false; off += 4; }
    // skip extension headers
    if (hasExtHdr) {
        // extension header: type(16) + length(16) + data
        while (off + 4 <= sz) {
            uint16_t ehType = rbe16(d + off);
            uint16_t ehLen  = rbe16(d + off + 2);
            off += 4 + ehLen;
            if (ehType == 0) break; // null terminator? (spec varies)
            // just skip all ext headers up to a reasonable limit
            if (off > sz) break;
        }
    }
    // Now parse payload
    info.hasMpuSeq = false;
    info.mpuSeqNum = 0;
    info.pts       = -1;
    info.isFirstMfu = false;

    if (info.payloadType == 0x00) {
        // MPU payload
        // MPU payload header:
        //   length(16) + payload_type(4) + fragmentation_indicator(2) + timed_flag(1) + ...
        if (off + 5 > sz) return false;
        uint16_t mpuLen   = rbe16(d + off); (void)mpuLen;
        uint8_t  mpuFType = (d[off+2] >> 4) & 0x0F;
        uint8_t  fragInd  = (d[off+2] >> 1) & 0x03;  // 0=not fragmented,1=first,2=middle,3=last
        bool     timedFlag = (d[off+2]) & 0x01;
        off += 3;
        if (off + 4 > sz) return false;
        info.mpuSeqNum = rbe32(d + off);
        info.hasMpuSeq = true;
        info.isFirstMfu = (fragInd == 0 || fragInd == 1); // not fragmented or first fragment
        off += 4;
        // if timed_flag, parse MFU payload with timestamps
        if (timedFlag && mpuFType == 2) {
            // MFU header: aggregate_flag(1) + ...
            // Actually for video: item_length(32) + DU_header ... complex
            // Just report the MPU sequence number
        }
    }
    return true;
}

// Parse compressed IP TLV to get MMTP
static bool extractMmtpFromCompIp(const uint8_t* d, size_t sz, MmtpInfo& info)
{
    if (sz < 3) return false;
    // context_id(12) + seq_num(4) = 2 bytes
    uint8_t headerType = d[2];
    size_t off = 3;
    // header type 0x20 = partial IPv6 + partial UDP
    if (headerType == 0x20) {
        // IPv6 source address (16 bytes) + destination address (16 bytes) = 32 bytes
        // Actually: src_addr(16) + dst_addr(16) + src_port(2) + dst_port(2) = 36 bytes
        // But partial means only the fields that changed
        // For simplicity assume full IPv6+UDP: 40 + 8 = 48 bytes
        // The spec says: for 0x20, contextIdPartialIpv6AndPartialUdp,
        // it includes ipv6 (20 bytes of src addr portion) + udp (4 bytes)
        // This is implementation-specific; let's just skip a fixed amount
        // and look for MMTP sync pattern
        // Try skipping 24 bytes (typical for dantto4k's format)
        off += 24;
    }
    if (off >= sz) return false;
    return parseMmtp(d + off, sz - off, info);
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        printf("Usage: mmts_probe <input.mmts>\n");
        return 1;
    }
    const char* filename = argv[1];

    printf("=== MMTS File Probe ===\n");
    printf("File: %s\n\n", filename);

    std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        printf("ERROR: Cannot open file\n");
        return 1;
    }
    std::streamsize fileSize = ifs.tellg();
    printf("File size: %.1f MB (%lld bytes)\n\n", fileSize / 1048576.0, (long long)fileSize);

    // Scan structure
    struct StreamStats {
        uint64_t packetCount = 0;
        uint32_t firstMpuSeq = 0;
        uint32_t lastMpuSeq  = 0;
        bool     hasFirstSeq = false;
        uint8_t  payloadType = 0;
        bool     seenRap     = false;
    };
    std::map<uint16_t, StreamStats> streams;

    // Read in chunks and parse TLV packets
    constexpr size_t kChunk = 1024 * 1024;
    std::vector<uint8_t> buf;
    buf.reserve(kChunk * 2);
    size_t totalRead = 0;
    uint64_t tlvCount = 0;
    constexpr size_t kMaxScan = 30ULL * 1024 * 1024; // scan first 30MB

    ifs.seekg(0);

    while (totalRead < kMaxScan) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = (size_t)ifs.gcount();
        buf.resize(oldSz + got);
        if (got == 0) break;
        totalRead += got;

        size_t pos = 0;
        TlvPacket pkt;
        while (readTlv(buf.data(), buf.size(), pos, pkt)) {
            tlvCount++;
            // type 0xFE = header compressed IP
            if (pkt.type == 0xFE && !pkt.data.empty()) {
                MmtpInfo info;
                if (extractMmtpFromCompIp(pkt.data.data(), pkt.data.size(), info)) {
                    auto& st = streams[info.packetId];
                    st.packetCount++;
                    st.payloadType = info.payloadType;
                    if (info.hasMpuSeq) {
                        if (!st.hasFirstSeq) {
                            st.firstMpuSeq = info.mpuSeqNum;
                            st.hasFirstSeq = true;
                        }
                        st.lastMpuSeq = info.mpuSeqNum;
                    }
                    if (info.rapFlag) st.seenRap = true;
                }
            }
        }
        // keep unconsumed bytes
        if (pos > 0 && pos <= buf.size())
            buf.erase(buf.begin(), buf.begin() + pos);
        else
            buf.clear();
    }

    printf("=== First %zu MB scan ===\n", kMaxScan / 1048576);
    printf("TLV packets parsed: %llu\n", (unsigned long long)tlvCount);
    printf("Unique packet IDs found: %zu\n\n", streams.size());
    printf("%-10s %-12s %-12s %-12s %-12s %-10s\n",
           "PacketID", "PayloadType", "Packets", "FirstMpuSeq", "LastMpuSeq", "RapSeen");
    printf("%-10s %-12s %-12s %-12s %-12s %-10s\n",
           "--------", "-----------", "-------", "-----------", "----------", "-------");
    for (auto& [pid, st] : streams) {
        const char* ptName =
            (st.payloadType == 0x00) ? "MPU" :
            (st.payloadType == 0x02) ? "Signaling" : "Other";
        printf("0x%04X    %-12s %-12llu %-12u %-12u %-10s\n",
               pid, ptName,
               (unsigned long long)st.packetCount,
               st.firstMpuSeq, st.lastMpuSeq,
               st.seenRap ? "YES" : "no");
    }

    // Now scan tail
    printf("\n=== Last 20MB tail scan ===\n");
    std::streamsize tailOffset = fileSize - 20LL * 1024 * 1024;
    if (tailOffset < 0) tailOffset = 0;
    printf("Scanning from offset %.1f MB\n\n", tailOffset / 1048576.0);

    std::map<uint16_t, StreamStats> tailStreams;
    buf.clear();
    ifs.clear();
    ifs.seekg(tailOffset);
    totalRead = 0;
    tlvCount  = 0;

    while (true) {
        size_t oldSz = buf.size();
        buf.resize(oldSz + kChunk);
        ifs.read(reinterpret_cast<char*>(buf.data() + oldSz), kChunk);
        size_t got = (size_t)ifs.gcount();
        buf.resize(oldSz + got);
        if (got == 0) break;
        totalRead += got;

        size_t pos = 0;
        TlvPacket pkt;
        while (readTlv(buf.data(), buf.size(), pos, pkt)) {
            tlvCount++;
            if (pkt.type == 0xFE && !pkt.data.empty()) {
                MmtpInfo info;
                if (extractMmtpFromCompIp(pkt.data.data(), pkt.data.size(), info)) {
                    auto& st = tailStreams[info.packetId];
                    st.packetCount++;
                    st.payloadType = info.payloadType;
                    if (info.hasMpuSeq) {
                        if (!st.hasFirstSeq) {
                            st.firstMpuSeq = info.mpuSeqNum;
                            st.hasFirstSeq = true;
                        }
                        st.lastMpuSeq = info.mpuSeqNum;
                    }
                    if (info.rapFlag) st.seenRap = true;
                }
            }
        }
        if (pos > 0 && pos <= buf.size())
            buf.erase(buf.begin(), buf.begin() + pos);
        else
            buf.clear();
    }

    printf("TLV packets parsed: %llu\n", (unsigned long long)tlvCount);
    printf("Unique packet IDs found: %zu\n\n", tailStreams.size());
    printf("%-10s %-12s %-12s %-12s %-12s %-10s\n",
           "PacketID", "PayloadType", "Packets", "FirstMpuSeq", "LastMpuSeq", "RapSeen");
    printf("%-10s %-12s %-12s %-12s %-12s %-10s\n",
           "--------", "-----------", "-------", "-----------", "----------", "-------");
    for (auto& [pid, st] : tailStreams) {
        const char* ptName =
            (st.payloadType == 0x00) ? "MPU" :
            (st.payloadType == 0x02) ? "Signaling" : "Other";
        printf("0x%04X    %-12s %-12llu %-12u %-12u %-10s\n",
               pid, ptName,
               (unsigned long long)st.packetCount,
               st.firstMpuSeq, st.lastMpuSeq,
               st.seenRap ? "YES" : "no");
    }

    printf("\nDone.\n");
    return 0;
}
