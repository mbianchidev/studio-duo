#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace studio
{
struct PluginStateReference
{
    juce::String relativePath;
    juce::String hash;
};

class PluginStateStore
{
public:
    static std::optional<PluginStateReference> store(
        const juce::File& package,
        const juce::MemoryBlock& state,
        juce::String& error);
    static bool load(const juce::File& package,
                     const PluginStateReference& reference,
                     juce::MemoryBlock& state,
                     juce::String& error);
};
}
