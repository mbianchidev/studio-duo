#pragma once

#include "PluginFormats.h"

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

namespace studio
{
struct PluginValidationCheck
{
    juce::String name;
    juce::String status;
    juce::String message;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<PluginValidationCheck> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct PluginValidationReport
{
    juce::String pluginIdentifier;
    juce::String name;
    juce::String format;
    juce::String version;
    juce::String status;
    juce::String createdAt;
    bool araCapable = false;
    std::vector<PluginValidationCheck> checks;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<PluginValidationReport> fromVar(
        const juce::var& value,
        juce::String& error);
};

class PluginCompatibilityValidator
{
public:
    static PluginValidationReport validate(
        const juce::PluginDescription& description);
};
}
