#pragma once
#include "additionalAribSubtitleInfo.h"
#include <optional>
#include <tuple>
#include <cstdint>

namespace DsSubtitle {
using Control = std::optional<MmtTlv::AdditionalAribSubtitleInfo>;

inline bool SameControl(const Control& a, const Control& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    auto fields = [](const auto& c) {
        return std::make_tuple(c.subtitleTag, c.subtitle_info_version,
            c.start_mpu_sequence_number_flag, c.languageCode, c.type,
            c.subtitleFormat, c.opm, c.tmd, c.dmf, c.resolution,
            c.compressionType, c.startMpuSequenceNumber,
            c.referenceStartTime.seconds, c.referenceStartTime.fraction,
            c.referenceStartTimeLeapIndicator);
    };
    return fields(*a) == fields(*b);
}

inline bool Synchronized(const Control& control)
{
    return control && control->tmd == 0b0010;
}

// Video MPU timestamps and reference_start_time use the same Unix/NTP clock.
// Convert the fraction directly, without losing precision through milliseconds.
inline std::optional<int64_t> ReferenceTime(const Control& control)
{
    if (!Synchronized(control) || control->referenceStartTime.seconds == 0)
        return std::nullopt;
    return (static_cast<int64_t>(control->referenceStartTime.seconds) - 2208988800LL) * 10000000LL
        + ((static_cast<uint64_t>(control->referenceStartTime.fraction) * 10000000ULL) >> 32);
}

inline uint64_t Hash(const uint8_t* data, size_t size)
{
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ULL;
    return hash;
}
}
