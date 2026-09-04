#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <span>

namespace studio
{
class SampleAccurateAutomationTarget
{
public:
    virtual ~SampleAccurateAutomationTarget() = default;

    [[nodiscard]] virtual bool supportsSampleAccurateAutomation(
        std::span<const PluginBridgeParameterEvent> events,
        int numSamples) const noexcept = 0;
    virtual void processBlockWithAutomation(
        juce::AudioBuffer<float>& audio,
        juce::MidiBuffer& midi,
        std::span<const PluginBridgeParameterEvent> events) noexcept = 0;
    virtual bool processBlockWithAutomationOrFallback(
        juce::AudioBuffer<float>& audio,
        juce::MidiBuffer& midi,
        std::span<const PluginBridgeParameterEvent> events) noexcept
    {
        if (!supportsSampleAccurateAutomation(
                events,
                audio.getNumSamples()))
            return false;
        processBlockWithAutomation(audio, midi, events);
        return true;
    }
};
}
