#pragma once

#include "UtilityDeviceProcessor.h"

#include <memory>
#include <vector>

namespace studio
{
enum class BundledDeviceKind
{
    utility,
    drumInstrument,
    guitarAmp,
    bassAmp
};

struct DeviceDescriptor
{
    juce::String identifier;
    juce::String name;
    juce::String category;
    BundledDeviceKind kind = BundledDeviceKind::utility;
    UtilityDeviceType utilityType = UtilityDeviceType::gain;
    int inputChannels = 2;
    int outputChannels = 2;
    bool instrument = false;
    std::vector<juce::String> outputBuses { "Main" };
};

class DeviceRegistry
{
public:
    static const std::vector<DeviceDescriptor>& descriptors();
    static const DeviceDescriptor* descriptor(
        const juce::String& identifier);
    static std::unique_ptr<juce::AudioProcessor> create(
        const juce::String& identifier);
    static bool isDeviceIdentifier(const juce::String& identifier);
    static double meterValue(const juce::AudioProcessor& processor,
                             const juce::String& meter);
};
}
