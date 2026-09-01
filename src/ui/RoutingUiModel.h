#pragma once

#include "model/ProjectModel.h"

#include <vector>

namespace studio
{
struct RoutingDestinationItem
{
    juce::String trackId;
    juce::String insertId;
    juce::String label;
};

class RoutingUiModel
{
public:
    static std::vector<RoutingDestinationItem> sendDestinations(
        const Project& project,
        const juce::String& sourceTrackId);
    static std::vector<RoutingDestinationItem> sidechainDestinations(
        const Project& project,
        const juce::String& sourceTrackId);
    static juce::String summary(const Project& project,
                                const RoutingConnection& connection);
};
}
