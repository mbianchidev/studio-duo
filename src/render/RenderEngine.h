#pragma once

#include "audio/StudioAudioEngine.h"

#include <functional>

namespace studio
{
class RenderEngine
{
public:
    using RequestBuilder = std::function<
        std::vector<StudioAudioEngine::PluginRuntimeRequest>(const Project&)>;

    static double levelMatchGainDecibels(
        const juce::AudioBuffer<float>& reference,
        const juce::AudioBuffer<float>& candidate);
    static std::optional<double> levelMatchGainDecibels(
        const juce::File& reference,
        const juce::File& candidate,
        juce::String& error);
    static std::vector<RenderReport> batchToneSnapshots(
        StudioAudioEngine& engine,
        const Project& project,
        const std::vector<ToneSnapshot>& snapshots,
        const juce::File& outputDirectory,
        const RequestBuilder& requestBuilder);
};
}
