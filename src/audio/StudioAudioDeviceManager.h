#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <vector>

namespace studio
{
inline constexpr int maximumHardwareAudioChannels = 256;

struct AudioDeviceTypeAvailability
{
    juce::String typeName;
    bool hasInputDevices = false;
    bool hasOutputDevices = false;
};

[[nodiscard]] juce::String preferredAsioDeviceName(
    const juce::StringArray& deviceNames);
[[nodiscard]] juce::StringArray orderedAsioDeviceNames(
    const juce::StringArray& deviceNames,
    juce::String deviceNameToTryLast = {});
[[nodiscard]] juce::AudioDeviceManager::AudioDeviceSetup
preferredAsioDeviceSetup(const juce::String& deviceName);
[[nodiscard]] juce::String preferredAvailableAudioDeviceType(
    const juce::String& currentType,
    const std::vector<AudioDeviceTypeAvailability>& deviceTypes);
[[nodiscard]] int callbackChannelIndex(
    const juce::BigInteger& activeChannels,
    int physicalChannel) noexcept;

class StudioAudioDeviceManager final : public juce::AudioDeviceManager
{
public:
    StudioAudioDeviceManager();

    [[nodiscard]] juce::Result initialiseStudioAudio();
    [[nodiscard]] juce::Result saveCurrentSetup() const;
    [[nodiscard]] juce::String takeStartupNotice();
    void prepareDeviceTypesForSettings();

private:
#if JUCE_WINDOWS
    [[nodiscard]] juce::Result initialiseAsioDevices(
        const juce::StringArray& deviceNames,
        const juce::String& deviceNameToTryLast);
    [[nodiscard]] juce::Result initialiseWindowsAudioFallback(
        const juce::String& asioFailure);
#endif
    void addWindowsFallbackDeviceTypes();
    void markDeviceTypesScanned() noexcept;

    juce::File settingsFile;
    juce::String startupNotice;
    bool deviceTypesScanned = false;
#if JUCE_WINDOWS
    bool windowsFallbackDeviceTypesAdded = false;
#endif
};
}
