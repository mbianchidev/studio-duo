#pragma once

#include "RenderEngine.h"
#include "model/ArrangementEditing.h"

namespace studio
{
struct ArrangementRenderPlan
{
    Project project;
    StudioAudioEngine::RenderRange range;
    Track renderedTrack;
    std::vector<juce::String> clipIds;
    juce::String destinationTrackId;
    juce::String freezeSourceTrackId;
    bool replaceSources = false;
};

struct ClipEffectsSettings
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double tailSeconds = 0.5;
    Track effects;
    std::vector<StudioAudioEngine::PluginRuntimeRequest> requests;
};

class ArrangementRender
{
public:
    static std::optional<ArrangementRenderPlan> prepareBounce(
        const Project&, const std::vector<juce::String>& clipIds, bool inPlace, juce::String& error);
    static std::optional<ArrangementRenderPlan> prepareFreeze(
        const Project&, const juce::String& trackId, juce::String& error);
    static juce::Result render(
        const ArrangementRenderPlan&, const juce::File&, double sampleRate,
        std::vector<StudioAudioEngine::PluginRuntimeRequest>, AudioClip& result);
    static std::unique_ptr<ProjectCommand> commit(
        const Project&, const ArrangementRenderPlan&, const AudioClip&, juce::String& error);
    static std::unique_ptr<ProjectCommand> unfreeze(
        const Project&, const juce::String& trackId, juce::String& error);
    static juce::Result applyClipEffects(
        const Project&, const AudioClip&, const ClipEffectsSettings&,
        const juce::File&, double sampleRate, AudioClip& result);
};
}
