#pragma once

#include "plugin_host/PluginCompatibilityValidator.h"

juce::Result exerciseVendorWorkflows(
    const juce::PluginDescription&, const juce::File& ownedDirectory,
    std::vector<studio::PluginValidationCheck>& checks, juce::var& measurements,
    const juce::var& profile = {});
