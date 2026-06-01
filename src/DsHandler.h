#pragma once
#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>
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
        uint16_t channels{2};
        bool latm{false};
        std::vector<uint8_t> extraData;
    };

    struct SubtitleStreamInfo {
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        bool hasData{false};
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

    using SubtitleResourceCallback = std::function<void(
        int streamIndex,
        int dataType,
        int subsampleNumber,
        int lastSubsampleNumber,
        long long pts,
        long long dts,
        const uint8_t* data,
        size_t size)>;

    void setVideoCallback(SampleCallback cb) { m_videoCallback = std::move(cb); }
    void setAudioCallback(SampleCallback cb) { m_audioCallback = std::move(cb); }
    void setSubtitleCallback(SampleCallback cb) { m_subtitleCallback = std::move(cb); }
    void setSubtitleResourceCallback(SubtitleResourceCallback cb) { m_subtitleResourceCallback = std::move(cb); }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override;
    void onMpt(const MmtTlv::Mpt& mpt) override;

    void reset() { m_basePts = -1; }
    void resetAudioSelection();
    std::vector<AudioStreamInfo> getAudioStreams() const;
    std::vector<SubtitleStreamInfo> getSubtitleStreams() const;
    void setKnownAudioStreams(const std::vector<AudioStreamInfo>& streams);
    void setKnownSubtitleStreams(const std::vector<SubtitleStreamInfo>& streams);
    std::vector<AudioStreamInfo> getAdtsConvertibleAudioStreams() const;
    std::vector<AudioStreamInfo> getPlayableAudioStreams() const;
    void setRequireAdtsConvertibleAudio(bool require);
    void setAudioStreamListLocked(bool locked);
    int getSelectedAudioStreamIndex() const;
    bool selectAudioStreamByListIndex(size_t listIndex);

private:
    long long toRefTime(int64_t pts, const MmtTlv::MmtStream& stream);
    void rememberAudioStream(const MmtTlv::MmtStream& stream);
    void rememberLatmConfig(int streamIndex, const uint8_t* data, size_t size);
    void rememberAdtsConvertibleAudioStream(int streamIndex);
    void rememberSubtitleStream(const MmtTlv::MmtStream& stream);
    bool shouldProcessAudioStream(int streamIndex) const;

    SampleCallback m_videoCallback;
    SampleCallback m_audioCallback;
    SampleCallback m_subtitleCallback;
    SubtitleResourceCallback m_subtitleResourceCallback;
    ADTSConverter  m_adtsConverter;
    long long m_basePts{-1};  // first valid PTS seen, in 100ns units
    int m_primaryAudioStreamIndex{-1};
    mutable std::mutex m_audioMutex;
    std::vector<AudioStreamInfo> m_audioStreams;
    std::vector<int> m_adtsConvertibleAudioStreams;
    bool m_requireAdtsConvertibleAudio{false};
    bool m_audioStreamListLocked{false};
    mutable std::mutex m_subtitleMutex;
    std::vector<SubtitleStreamInfo> m_subtitleStreams;
};
