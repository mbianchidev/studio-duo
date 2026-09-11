#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/MainWindowSizing.h"

namespace
{
bool framedWindowFits(
    juce::Rectangle<int> clientBounds,
    juce::BorderSize<int> nativeFrame,
    juce::Rectangle<int> displayUserBounds)
{
    return displayUserBounds.reduced(16).contains(
        nativeFrame.addedTo(clientBounds));
}
}

void windowSizingTests()
{
    const juce::BorderSize<int> nativeFrame(31, 8, 8, 8);

    const juce::Rectangle<int> largeDisplay(0, 0, 1920, 1040);
    const auto preferred = studio::calculateInitialMainWindowBounds(
        largeDisplay,
        nativeFrame);
    expect(preferred.getWidth() == 1480
               && preferred.getHeight() == 900,
           "A large display keeps the preferred main-window client size.");
    expect(framedWindowFits(preferred, nativeFrame, largeDisplay),
           "The preferred framed window stays inside the display work area.");

    const juce::Rectangle<int> smallDisplay(0, 0, 1280, 760);
    const auto reduced = studio::calculateInitialMainWindowBounds(
        smallDisplay,
        nativeFrame);
    expect(reduced.getWidth() < preferred.getWidth()
               && reduced.getHeight() < preferred.getHeight(),
           "A small display reduces the initial main-window size.");
    expect(framedWindowFits(reduced, nativeFrame, smallDisplay),
           "A reduced framed window keeps every native control on-screen.");

    const juce::Rectangle<int> scaledDisplay(0, 0, 1024, 700);
    const auto scaled = studio::calculateInitialMainWindowBounds(
        scaledDisplay,
        nativeFrame);
    expect(!scaled.isEmpty()
               && framedWindowFits(scaled, nativeFrame, scaledDisplay),
           "A DPI-scaled logical display can fit below the normal resize minimum.");

    const juce::Rectangle<int> secondaryDisplay(1920, 120, 1280, 760);
    const auto offset = studio::calculateInitialMainWindowBounds(
        secondaryDisplay,
        nativeFrame);
    expect(framedWindowFits(offset, nativeFrame, secondaryDisplay),
           "Window sizing preserves a secondary display's screen offset.");

    expect(studio::calculateInitialMainWindowBounds({}, nativeFrame).isEmpty(),
           "Missing display bounds produce no unsafe window position.");
}
