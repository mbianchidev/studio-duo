#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace studio
{
class AudioAnalysis
{
public:
    static std::vector<double> detectTransients(
        const juce::AudioBuffer<float>& buffer,
        double sampleRate,
        double sensitivity = 0.5);
};
}
