#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace studio
{
[[nodiscard]] juce::String preferredAsioDeviceName(
    const juce::StringArray& deviceNames);

class StudioAudioDeviceManager final : public juce::AudioDeviceManager
{
public:
    void createAudioDeviceTypes(
        juce::OwnedArray<juce::AudioIODeviceType>& types) override;
};
}
