#pragma once

#include <juce_core/juce_core.h>

namespace studio
{
[[nodiscard]] juce::Result launchUpdateInstaller(
    const juce::File& package,
    const juce::String& version);
}
