#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace studio
{
inline constexpr int maximumHardwareAudioChannels = 256;

[[nodiscard]] juce::String preferredAsioDeviceName(
    const juce::StringArray& deviceNames);
[[nodiscard]] juce::AudioDeviceManager::AudioDeviceSetup
preferredAsioDeviceSetup(const juce::String& deviceName);
[[nodiscard]] int callbackChannelIndex(
    const juce::BigInteger& activeChannels,
    int physicalChannel) noexcept;

class StudioAudioDeviceManager final : public juce::AudioDeviceManager
{
public:
    StudioAudioDeviceManager();

    [[nodiscard]] juce::Result initialiseStudioAudio();
    [[nodiscard]] juce::Result saveCurrentSetup() const;
    void prepareDeviceTypesForSettings();

private:
    void addWindowsFallbackDeviceTypes();
    void markDeviceTypesScanned() noexcept;

    juce::File settingsFile;
    bool deviceTypesScanned = false;
#if JUCE_WINDOWS
    bool windowsFallbackDeviceTypesAdded = false;
#endif
};
}
