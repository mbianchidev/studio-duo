#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace studio
{
struct PluginOutputBus
{
    juce::String name;
    int channels = 0;
    int bufferChannelOffset = -1;
};

inline juce::Result configurePluginAudioBuses(juce::AudioProcessor& processor, int sidechainChannels, int lastOutputBus)
{
    const auto count = processor.getBusCount(false);
    if (count > 128 || lastOutputBus < 0 || (lastOutputBus > 0 && lastOutputBus >= count))
        return juce::Result::fail("The requested processor output bus is unavailable.");
    if (sidechainChannels < 0 || sidechainChannels > 2
        || (sidechainChannels > 0 && processor.getBusCount(true) <= 1))
        return juce::Result::fail("The requested mono/stereo processor sidechain is unavailable.");
    auto layout = processor.getBusesLayout();
    auto changed = false;
    if (sidechainChannels > 0)
    {
        const auto channels = sidechainChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        changed = layout.inputBuses[1] != channels;
        layout.inputBuses.set(1, channels);
    }
    for (int bus = 1; bus <= lastOutputBus; ++bus)
    {
        if (!layout.outputBuses[bus].isDisabled())
            continue;
        const auto channels = processor.getBus(false, bus)->getDefaultLayout();
        if (channels.isDisabled())
            return juce::Result::fail("The requested processor output bus has no supported default layout.");
        layout.outputBuses.set(bus, channels);
        changed = true;
    }
    if (changed && !processor.setBusesLayout(layout))
        return juce::Result::fail("The processor rejected the requested sidechain/auxiliary output layout.");
    return juce::Result::ok();
}

inline std::vector<PluginOutputBus> pluginOutputBuses(const juce::AudioProcessor& processor)
{
    std::vector<PluginOutputBus> result;
    for (int index = 0; index < processor.getBusCount(false); ++index)
    {
        const auto* bus = processor.getBus(false, index);
        auto name = bus->getName().substring(0, 256);
        if (name.trim().isEmpty())
            name = "Output " + juce::String(index + 1);
        const auto channels = processor.getChannelCountOfBus(false, index);
        result.push_back({ name, channels > 0 ? channels : bus->getDefaultLayout().size(),
            channels > 0 ? processor.getChannelIndexInProcessBlockBuffer(false, index, 0) : -1 });
    }
    return result;
}

inline juce::StringArray pluginOutputBusNames(const std::vector<PluginOutputBus>& buses)
{
    juce::StringArray result;
    for (const auto& bus : buses)
        result.add(bus.name);
    return result;
}

inline juce::String encodePluginOutputBuses(const std::vector<PluginOutputBus>& buses)
{
    juce::Array<juce::var> values;
    for (const auto& bus : buses)
    {
        auto value = std::make_unique<juce::DynamicObject>();
        value->setProperty("name", bus.name);
        value->setProperty("channels", bus.channels);
        value->setProperty("offset", bus.bufferChannelOffset);
        values.add(juce::var(value.release()));
    }
    const auto json = juce::JSON::toString(juce::var(values), false);
    return juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8()).toBase64Encoding();
}

inline juce::Result decodePluginOutputBuses(const juce::String& encoded, std::vector<PluginOutputBus>& output)
{
    juce::MemoryBlock data;
    if (encoded.length() > 65536 || !data.fromBase64Encoding(encoded))
        return juce::Result::fail("The worker returned invalid output-bus metadata.");
    const auto value = juce::JSON::parse(juce::String::fromUTF8(
        static_cast<const char*>(data.getData()), static_cast<int>(data.getSize())));
    const auto* values = value.getArray();
    if (values == nullptr || values->size() > 128)
        return juce::Result::fail("The worker returned an invalid output-bus list.");
    std::vector<PluginOutputBus> buses;
    auto offset = 0;
    for (const auto& entry : *values)
    {
        if (entry.getDynamicObject() == nullptr || !entry["name"].isString()
            || entry["name"].toString().trim().isEmpty() || entry["name"].toString().length() > 256
            || !entry["channels"].isInt() || !entry["offset"].isInt())
            return juce::Result::fail("The worker returned an invalid output-bus descriptor.");
        const auto channels = static_cast<int>(entry["channels"]);
        const auto channelOffset = static_cast<int>(entry["offset"]);
        if (channels < 0 || channels > 1024 || channelOffset < -1
            || (channelOffset >= 0 && (channels == 0 || channelOffset != offset
                || channels > PluginBridgeSharedState::maxChannels - offset)))
            return juce::Result::fail("The worker returned an unsupported output-bus channel map.");
        if (channelOffset >= 0)
            offset += channels;
        buses.push_back({ entry["name"].toString(), channels, channelOffset });
    }
    output = std::move(buses);
    return juce::Result::ok();
}
}
