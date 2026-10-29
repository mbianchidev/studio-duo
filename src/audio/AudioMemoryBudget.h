#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <algorithm>
#include <optional>

namespace studio
{
inline constexpr std::size_t maximumAudioBufferBytes = 512 * 1024 * 1024;

inline std::optional<int> boundedAudioSampleCount(double requestedSamples, int channels, juce::String& error)
{
    if (channels <= 0 || static_cast<std::size_t>(channels) > maximumAudioBufferBytes / sizeof(float)
        || !std::isfinite(requestedSamples) || requestedSamples <= 0.0
        || std::round(requestedSamples) > static_cast<double>(maximumAudioBufferBytes
            / (static_cast<std::size_t>(channels) * sizeof(float))))
    {
        error = "An audio buffer exceeds the 512 MiB memory limit or has invalid dimensions. "
                "Shorten the source/render range or deactivate unused tracks.";
        return std::nullopt;
    }
    return std::max(1, static_cast<int>(std::llround(requestedSamples)));
}

inline bool reservePreparedAudio(std::size_t bytes, std::size_t& reserved, juce::String& error)
{
    if (reserved > maximumAudioBufferBytes || bytes > maximumAudioBufferBytes - reserved)
    {
        error = "Prepared audio exceeds the 512 MiB project memory budget. "
                "Deactivate unused tracks, shorten source clips, or consolidate repeated media.";
        return false;
    }
    reserved += bytes;
    return true;
}
}
