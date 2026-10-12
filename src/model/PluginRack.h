#pragma once

#include "ProjectModel.h"

#include <functional>
#include <map>

namespace studio
{
class PluginRack
{
public:
    static constexpr int maximumDepth = 32;
    struct Timing
    {
        std::int64_t latencySamples = 0;
        double tailSeconds = 0.0;
    };
    using TimingLookup = std::function<Timing(const PluginInsert&)>;

    [[nodiscard]] static bool validate(
        const std::vector<PluginInsert>& inserts, juce::String& error);
    [[nodiscard]] static const PluginInsert* find(
        const std::vector<PluginInsert>& inserts, const juce::String& id);
    [[nodiscard]] static bool isBypassed(
        const std::vector<PluginInsert>& inserts, const PluginInsert& insert);
    [[nodiscard]] static bool contains(
        const std::vector<PluginInsert>& inserts, const juce::String& containerId,
        const PluginInsert& insert);
    [[nodiscard]] static Timing measure(
        const std::vector<PluginInsert>& inserts, const TimingLookup& lookup = {},
        std::map<juce::String, std::int64_t>* inputLatencies = nullptr,
        const juce::String& containerId = {});
};
}
