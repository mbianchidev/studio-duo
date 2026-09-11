#pragma once

#include <juce_graphics/juce_graphics.h>

namespace studio
{
[[nodiscard]] juce::Rectangle<int> calculateInitialMainWindowBounds(
    juce::Rectangle<int> displayUserBounds,
    juce::BorderSize<int> nativeFrame);
}
