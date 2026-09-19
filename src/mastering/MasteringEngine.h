#pragma once

#include "MasteringModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <limits>
#include <optional>

namespace studio
{
struct MasteringMeasurement
{
    std::optional<double> integratedLoudnessLufs;
    std::optional<double> loudnessRangeLu;
    double samplePeakDbfs = -std::numeric_limits<double>::infinity();
    double truePeakDbtp = -std::numeric_limits<double>::infinity();
    double correlation = 1.0;
    double durationSeconds = 0.0;
};

struct MasteringRender
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    std::vector<MasteringTrackPlacement> placements;
};

class MasteringEngine
{
public:
    static MasteringMeasurement analyse(
        const juce::AudioBuffer<float>& audio,
        double sampleRate);
    static std::optional<MasteringRender> renderAlbum(
        const MasteringAlbum& album,
        double sampleRate,
        juce::String& error);
};
}
