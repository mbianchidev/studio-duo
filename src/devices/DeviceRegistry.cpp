#include "DeviceRegistry.h"

#include <algorithm>

namespace studio
{
const std::vector<DeviceDescriptor>& DeviceRegistry::descriptors()
{
    static const std::vector<DeviceDescriptor> values {
        { "studio.device.eq", "Parametric EQ", UtilityDeviceType::equalizer },
        { "studio.device.compressor", "Compressor", UtilityDeviceType::compressor },
        { "studio.device.limiter", "True-Peak Limiter", UtilityDeviceType::limiter },
        { "studio.device.reverb", "Algorithmic Reverb", UtilityDeviceType::reverb },
        { "studio.device.gate", "Noise Gate", UtilityDeviceType::gate },
        { "studio.device.gain", "Gain", UtilityDeviceType::gain },
        { "studio.device.polarity", "Polarity", UtilityDeviceType::polarity },
        { "studio.device.delay", "Delay", UtilityDeviceType::delay },
        { "studio.device.tuner", "Tuner", UtilityDeviceType::tuner },
        { "studio.device.generator", "Signal Generator", UtilityDeviceType::generator }
    };
    return values;
}

std::unique_ptr<juce::AudioProcessor> DeviceRegistry::create(
    const juce::String& identifier)
{
    const auto descriptor = std::find_if(
        descriptors().cbegin(),
        descriptors().cend(),
        [&identifier](const auto& value)
        {
            return value.identifier == identifier;
        });
    return descriptor == descriptors().cend()
        ? std::unique_ptr<juce::AudioProcessor> {}
        : std::make_unique<UtilityDeviceProcessor>(descriptor->type);
}

bool DeviceRegistry::isDeviceIdentifier(const juce::String& identifier)
{
    return std::any_of(
        descriptors().cbegin(),
        descriptors().cend(),
        [&identifier](const auto& value)
        {
            return value.identifier == identifier;
        });
}

double DeviceRegistry::meterValue(const juce::AudioProcessor& processor,
                                  const juce::String& meter)
{
    const auto* utility =
        dynamic_cast<const UtilityDeviceProcessor*>(&processor);
    return utility != nullptr ? utility->meterValue(meter) : 0.0;
}
}
