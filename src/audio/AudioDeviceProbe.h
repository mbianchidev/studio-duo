#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <atomic>
#include <memory>
#include <optional>

namespace studio
{
inline constexpr auto audioDeviceProbeArgument = "--audio-device-probe";
inline constexpr auto midiDeviceDiscoveryProbeType = "MIDI discovery";
inline constexpr auto studioAudioStartupProbeType = "Studio audio startup";

enum class AudioDeviceProbeFailure
{
    none, invalidRequest, storage, launch, timeout, cancelled, crash, deviceUnavailable, protocol
};

struct AudioDeviceProbeResult
{
    juce::Result result = juce::Result::ok();
    AudioDeviceProbeFailure failure = AudioDeviceProbeFailure::none;
};

struct AudioDeviceProbeOptions
{
    juce::File executable = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    int timeoutMs = 10000;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

[[nodiscard]] juce::String audioDeviceSetupName(const juce::XmlElement& setup);
[[nodiscard]] juce::Result probeAudioDeviceSetup(
    const juce::XmlElement& setup,
    const AudioDeviceProbeOptions& options = {});
[[nodiscard]] AudioDeviceProbeResult probeAudioDeviceSetupDetailed(
    const juce::XmlElement& setup,
    const AudioDeviceProbeOptions& options = {});
[[nodiscard]] std::optional<int> runAudioDeviceProbeWorker(
    const juce::StringArray& arguments,
    const std::function<juce::Result(const juce::XmlElement&)>& openDevice);
}
