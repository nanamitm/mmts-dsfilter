#pragma once
#include <functional>
#include <vector>
#include <mutex>
#include "demuxerHandler.h"
#include "adtsConverter.h"

namespace MmtTlv { class MmtStream; struct MfuData; }

// Bridges dantto4k demuxer callbacks to DirectShow sample delivery.
// Uses long long (= REFERENCE_TIME = 100ns units) to avoid windows.h dependency here.
class CFilterDemuxerHandler : public MmtTlv::DemuxerHandler {
public:
    struct AudioStreamInfo {
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        uint32_t samplingRate{0};
    };

    using SampleCallback = std::function<void(
        int streamIndex,
        bool keyframe,
        long long pts,   // DirectShow REFERENCE_TIME (100ns units), -1 = unknown
        long long dts,
        bool isFirstFragment,
        bool isLastFragment,
        const uint8_t* data,
        size_t size)>;

    void setVideoCallback(SampleCallback cb) { m_videoCallback = std::move(cb); }
    void setAudioCallback(SampleCallback cb) { m_audioCallback = std::move(cb); }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onMpt(const MmtTlv::Mpt& mpt) override;

    void reset() { m_basePts = -1; }
    void resetAudioSelection();
    std::vector<AudioStreamInfo> getAudioStreams() const;
    void setKnownAudioStreams(const std::vector<AudioStreamInfo>& streams);
    int getSelectedAudioStreamIndex() const;
    bool selectAudioStreamByListIndex(size_t listIndex);

private:
    long long toRefTime(int64_t pts, const MmtTlv::MmtStream& stream);
    void rememberAudioStream(const MmtTlv::MmtStream& stream);

    SampleCallback m_videoCallback;
    SampleCallback m_audioCallback;
    ADTSConverter  m_adtsConverter;
    long long m_basePts{-1};  // first valid PTS seen, in 100ns units
    int m_primaryAudioStreamIndex{-1};
    mutable std::mutex m_audioMutex;
    std::vector<AudioStreamInfo> m_audioStreams;
};
