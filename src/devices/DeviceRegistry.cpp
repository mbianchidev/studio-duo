#include "DeviceRegistry.h"

#include "AmpDeviceProcessor.h"
#include "DrumDeviceProcessor.h"

#include <algorithm>

namespace studio
{
const std::vector<DeviceDescriptor>& DeviceRegistry::descriptors()
{
    static const std::vector<DeviceDescriptor> values {
        { "studio.device.eq", "Parametric EQ", "Utility",
          BundledDeviceKind::utility, UtilityDeviceType::equalizer },
        { "studio.device.compressor", "Compressor", "Dynamics",
          BundledDeviceKind::utility, UtilityDeviceType::compressor },
        { "studio.device.limiter", "True-Peak Limiter", "Dynamics",
          BundledDeviceKind::utility, UtilityDeviceType::limiter },
        { "studio.device.reverb", "Algorithmic Reverb", "Reverb",
          BundledDeviceKind::utility, UtilityDeviceType::reverb },
        { "studio.device.gate", "Noise Gate", "Dynamics",
          BundledDeviceKind::utility, UtilityDeviceType::gate },
        { "studio.device.gain", "Gain", "Utility",
          BundledDeviceKind::utility, UtilityDeviceType::gain },
        { "studio.device.polarity", "Polarity", "Utility",
          BundledDeviceKind::utility, UtilityDeviceType::polarity },
        { "studio.device.delay", "Delay", "Utility",
          BundledDeviceKind::utility, UtilityDeviceType::delay },
        { "studio.device.tuner", "Tuner", "Utility",
          BundledDeviceKind::utility, UtilityDeviceType::tuner },
        { "studio.device.generator", "Signal Generator", "Utility",
          BundledDeviceKind::utility, UtilityDeviceType::generator },
        { "studio.device.drum-composer", "Metal Drum Composer",
          "Instrument", BundledDeviceKind::drumInstrument,
          UtilityDeviceType::gain, 0, 10, true,
          { "Main", "Kick", "Snare", "Toms", "Cymbals" } },
        { "studio.device.guitar-amp", "Guitar Amp", "Amp",
          BundledDeviceKind::guitarAmp, UtilityDeviceType::gain },
        { "studio.device.bass-amp", "Bass Amp", "Amp",
          BundledDeviceKind::bassAmp, UtilityDeviceType::gain }
    };
    return values;
}

const DeviceDescriptor* DeviceRegistry::descriptor(
    const juce::String& identifier)
{
    const auto match = std::find_if(
        descriptors().cbegin(),
        descriptors().cend(),
        [&identifier](const auto& value)
        {
            return value.identifier == identifier;
        });
    return match == descriptors().cend() ? nullptr : &*match;
}

std::unique_ptr<juce::AudioProcessor> DeviceRegistry::create(
    const juce::String& identifier)
{
    const auto* value = descriptor(identifier);
    if (value == nullptr)
        return {};
    switch (value->kind)
    {
        case BundledDeviceKind::utility:
            return std::make_unique<UtilityDeviceProcessor>(
                value->utilityType);
        case BundledDeviceKind::drumInstrument:
            return std::make_unique<DrumDeviceProcessor>();
        case BundledDeviceKind::guitarAmp:
            return std::make_unique<AmpDeviceProcessor>(
                AmpDeviceType::guitar);
        case BundledDeviceKind::bassAmp:
            return std::make_unique<AmpDeviceProcessor>(
                AmpDeviceType::bass);
    }
    return {};
}

bool DeviceRegistry::isDeviceIdentifier(const juce::String& identifier)
{
    return descriptor(identifier) != nullptr;
}

double DeviceRegistry::meterValue(const juce::AudioProcessor& processor,
                                  const juce::String& meter)
{
    const auto* utility =
        dynamic_cast<const UtilityDeviceProcessor*>(&processor);
    return utility != nullptr ? utility->meterValue(meter) : 0.0;
}
}
