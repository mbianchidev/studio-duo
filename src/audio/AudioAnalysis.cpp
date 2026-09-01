#include "AudioAnalysis.h"

#include <algorithm>
#include <cmath>

namespace studio
{
std::vector<double> AudioAnalysis::detectTransients(
    const juce::AudioBuffer<float>& buffer,
    double sampleRate,
    double sensitivity)
{
    if (sampleRate <= 0.0
        || buffer.getNumChannels() <= 0
        || buffer.getNumSamples() <= 0)
        return {};

    auto peak = 0.0f;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        peak = std::max(peak,
                        buffer.getMagnitude(channel,
                                            0,
                                            buffer.getNumSamples()));
    if (peak < 0.0001f)
        return {};

    const auto threshold = std::max(
        0.005f,
        peak * static_cast<float>(juce::jmap(juce::jlimit(0.0,
                                                          1.0,
                                                          sensitivity),
                                             0.35,
                                             0.08)));
    const auto releaseThreshold = threshold * 0.35f;
    const auto minimumDistanceSamples = std::max(
        1,
        static_cast<int>(std::round(sampleRate * 0.02)));
    auto lastTransient = -minimumDistanceSamples;
    auto armed = true;
    std::vector<double> transients;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        auto level = 0.0f;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            level = std::max(level, std::abs(buffer.getSample(channel, sample)));

        if (armed
            && level >= threshold
            && sample - lastTransient >= minimumDistanceSamples)
        {
            transients.push_back(static_cast<double>(sample) / sampleRate);
            lastTransient = sample;
            armed = false;
        }
        else if (!armed && level <= releaseThreshold)
        {
            armed = true;
        }
    }
    return transients;
}
}
