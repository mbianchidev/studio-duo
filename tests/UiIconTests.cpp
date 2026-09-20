#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/StudioIconButton.h"

#include <cmath>

void uiIconTests()
{
    for (const auto icon : studio::allStudioIcons)
    {
        const auto path = studio::createStudioIconPath(icon);
        const auto bounds = path.getBounds();
        expect(!path.isEmpty(), "Every Studio icon has drawable geometry.");
        expect(std::isfinite(bounds.getX())
                   && std::isfinite(bounds.getY())
                   && std::isfinite(bounds.getRight())
                   && std::isfinite(bounds.getBottom()),
               "Studio icon bounds are finite.");
        expect(bounds.getX() >= 0.0f
                   && bounds.getY() >= 0.0f
                   && bounds.getRight() <= 24.0f
                   && bounds.getBottom() <= 24.0f,
               "Studio icon geometry stays inside the normalized canvas.");
    }

    studio::StudioIconButton button(
        studio::StudioIcon::play,
        "Play",
        "Play (Space)");
    expect(button.getButtonText().isEmpty(),
           "Icon buttons do not expose a visible text label.");
    expect(button.getTitle() == "Play",
           "Icon buttons expose an accessible title.");
    expect(button.getTooltip() == "Play (Space)",
           "Icon buttons preserve an explicit hover tooltip.");
    expect(button.getWantsKeyboardFocus(),
           "Icon buttons participate in keyboard focus.");

    button.setClickingTogglesState(true);
    button.setToggleState(true, juce::dontSendNotification);
    expect(button.getToggleState(),
           "Icon buttons preserve ordinary JUCE toggle state.");

    button.setIcon(studio::StudioIcon::pause);
    button.setAccessibleLabel("Pause");
    button.setTooltip("Pause playback (Space)");
    expect(button.getIcon() == studio::StudioIcon::pause,
           "Stateful controls can replace their icon.");
    expect(button.getTitle() == "Pause"
               && button.getTooltip() == "Pause playback (Space)",
           "Stateful controls can replace their accessible name and tooltip.");
}
