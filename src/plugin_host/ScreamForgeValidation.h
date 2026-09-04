#pragma once

#include "PluginCompatibilityValidator.h"

namespace studio
{
struct ScreamForgeValidationResult
{
    juce::String status;
    std::vector<PluginValidationReport> reports;

    [[nodiscard]] juce::var toVar() const;
};

class ScreamForgeValidation
{
public:
    static bool matches(const juce::PluginDescription& description);
    static ScreamForgeValidationResult validateInstalled(
        const std::vector<juce::PluginDescription>& descriptions);
};
}
