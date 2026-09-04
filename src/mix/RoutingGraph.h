#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace studio
{
class Project;

class RoutingGraph
{
public:
    static bool validate(const Project& project, juce::String& error);
    static std::optional<std::vector<juce::String>> order(
        const Project& project,
        juce::String& error);
};
}
