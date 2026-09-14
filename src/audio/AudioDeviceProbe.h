#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>

namespace studio
{
inline constexpr auto audioDeviceProbeArgument = "--audio-device-probe";
inline constexpr auto midiDeviceDiscoveryProbeType = "MIDI discovery";

struct AudioDeviceProbeOptions
{
    juce::File executable = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    int timeoutMs = 10000;
};

[[nodiscard]] juce::String audioDeviceSetupName(const juce::XmlElement& setup);
[[nodiscard]] juce::Result probeAudioDeviceSetup(
    const juce::XmlElement& setup,
    const AudioDeviceProbeOptions& options = {});
[[nodiscard]] std::optional<int> runAudioDeviceProbeWorker(
    const juce::StringArray& arguments,
    const std::function<juce::Result(const juce::XmlElement&)>& openDevice);
}
