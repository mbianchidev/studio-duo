#include "MainWindowSizing.h"

namespace studio
{
juce::Rectangle<int> calculateInitialMainWindowBounds(
    juce::Rectangle<int> displayUserBounds,
    juce::BorderSize<int> nativeFrame)
{
    constexpr auto preferredWidth = 1480;
    constexpr auto preferredHeight = 900;
    constexpr auto displayMargin = 16;

    if (displayUserBounds.isEmpty())
        return {};

    const auto horizontalInset = juce::jmin(
        displayMargin,
        juce::jmax(0, (displayUserBounds.getWidth() - 1) / 2));
    const auto verticalInset = juce::jmin(
        displayMargin,
        juce::jmax(0, (displayUserBounds.getHeight() - 1) / 2));
    const auto available =
        displayUserBounds.reduced(horizontalInset, verticalInset);
    const auto frameWidth = nativeFrame.getLeftAndRight();
    const auto frameHeight = nativeFrame.getTopAndBottom();

    if (available.getWidth() <= frameWidth
        || available.getHeight() <= frameHeight)
    {
        return {};
    }

    const auto outerWidth = juce::jmin(
        preferredWidth + frameWidth,
        available.getWidth());
    const auto outerHeight = juce::jmin(
        preferredHeight + frameHeight,
        available.getHeight());
    juce::Rectangle<int> outerBounds(
        0,
        0,
        outerWidth,
        outerHeight);
    outerBounds.setCentre(available.getCentre());
    return nativeFrame.subtractedFrom(outerBounds);
}
}
