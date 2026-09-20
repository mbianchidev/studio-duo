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
}
