#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <optional>

namespace studio
{
class ProjectMigration
{
public:
    [[nodiscard]] static std::optional<int> supportedVersion(const juce::var& value);
    static std::optional<juce::var> migrateToCurrent(const juce::var& value,
                                                     juce::String& error);
};
}
