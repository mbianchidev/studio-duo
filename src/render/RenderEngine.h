#pragma once

#include "audio/StudioAudioEngine.h"
#include "AudioExport.h"

#include <functional>

namespace studio
{
enum class MixExportRange
{
    entireProject,
    loop,
    markers,
    custom
};

struct MixExportSettings
{
    AudioExportSettings audio;
    MixExportRange range = MixExportRange::entireProject;
    juce::String startMarkerId;
    juce::String endMarkerId;
    double startSeconds = 0.0;
    double endSeconds = 8.0;
    std::optional<double> tailSeconds;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
};

class RenderEngine
{
public:
    using RequestBuilder = std::function<
        std::vector<StudioAudioEngine::PluginRuntimeRequest>(const Project&)>;

    static std::optional<StudioAudioEngine::RenderRange> resolveRange(
        const Project& project,
        const MixExportSettings& settings,
        juce::String& error);
    static juce::Result exportMix(
        StudioAudioEngine& engine,
        const Project& project,
        const juce::File& destination,
        const MixExportSettings& settings,
        std::vector<StudioAudioEngine::PluginRuntimeRequest> pluginRequests = {});
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
