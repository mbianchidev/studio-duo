#pragma once

#include "UtilityDeviceProcessor.h"

#include <memory>
#include <vector>

namespace studio
{
struct DeviceDescriptor
{
    juce::String identifier;
    juce::String name;
    UtilityDeviceType type = UtilityDeviceType::gain;
};

class DeviceRegistry
{
public:
    static const std::vector<DeviceDescriptor>& descriptors();
    static std::unique_ptr<juce::AudioProcessor> create(
        const juce::String& identifier);
    static bool isDeviceIdentifier(const juce::String& identifier);
    static double meterValue(const juce::AudioProcessor& processor,
                             const juce::String& meter);
};
}
