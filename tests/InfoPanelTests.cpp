#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/InfoPanelComponent.h"

void infoPanelTests()
{
    studio::InfoPanelComponent panel;
    panel.pushMessage("Ready: CoreAudio", false);
    panel.pushMessage("Ready: CoreAudio", false);
    panel.pushMessage("Plug-in failed", true);
    expect(panel.historySize() == 2
               && panel.latestMessage() == "Plug-in failed"
               && panel.latestIsError(),
           "The info panel keeps ordered history, deduplicates consecutive messages, and exposes the latest error.");

    const auto firstId = panel.history().front().id;
    panel.removeEntry(firstId);
    expect(panel.historySize() == 1
               && panel.latestMessage() == "Plug-in failed",
           "Individual info history entries can be cleared.");

    panel.clearHistory();
    expect(panel.historySize() == 0
               && panel.latestMessage().isEmpty()
               && !panel.latestIsError(),
           "The info panel can clear all history.");

    panel.setLiveMessage("Recording 1 track, 1.0 s.", false);
    panel.setLiveMessage("Recording 1 track, 1.1 s.", false);
    expect(panel.historySize() == 0
               && panel.latestMessage().contains("1.1"),
           "Live progress replaces the display without creating history entries.");
    panel.pushMessage("Saved 1 audio take.", false);
    expect(panel.historySize() == 1
               && panel.latestMessage() == "Saved 1 audio take.",
           "A completed recording contributes one final history event.");
}
