#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace studio
{
class Project;

struct SoloResolution
{
    std::vector<juce::String> audibleTrackIds;
    std::vector<juce::String> processingTrackIds;

    [[nodiscard]] bool isAudible(const juce::String& trackId) const;
    [[nodiscard]] bool isProcessing(const juce::String& trackId) const;
};

class SoloResolver
{
public:
    static std::optional<SoloResolution> resolve(const Project& project,
                                                 juce::String& error);
};
}
