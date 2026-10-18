#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/InfoPanelComponent.h"

#include <algorithm>

void infoPanelTests()
{
    studio::InfoPanelComponent panel;
    panel.setSize(400, 32);
    panel.setVisible(false);
    panel.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    panel.pushMessage("Ready: CoreAudio", false);
    panel.pushMessage("Ready: CoreAudio", false);
    panel.pushMessage("Plug-in failed", true);
    expect(panel.historySize() == 2
               && panel.latestMessage() == "Plug-in failed"
               && panel.latestIsError(),
           "The info panel keeps ordered history, deduplicates consecutive messages, and exposes the latest error.");
    const auto* accessibility = panel.getAccessibilityHandler();
    expect(accessibility != nullptr && accessibility->getRole() == juce::AccessibilityRole::button
               && accessibility->getValueInterface() != nullptr
               && accessibility->getValueInterface()->getCurrentValueAsString().contains("Error: Plug-in failed"),
           "The current error is exposed as a native named action/value instead of painted text and color alone.");

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
    for (int index = 0; index < 230; ++index)
        panel.pushMessage("Synthetic history message " + juce::String(index), index % 2 == 0);
    auto view = panel.createHistoryView();
    auto* list = dynamic_cast<juce::ListBox*>(view->findChildWithID("status-history.list"));
    auto* model = list != nullptr ? list->getListBoxModel() : nullptr;
    expect(list != nullptr && model != nullptr && model->getNumRows() == 200
               && list->getWantsKeyboardFocus() && list->getTitle().contains("Status"),
           "All 200 retained messages are exposed through one native keyboard list rather than clipped painted rows.");
    if (list == nullptr || model == nullptr)
        return;
    list->selectRow(199);
    const auto oldestId = panel.history().front().id;
    expect(list->getSelectedRow() == 199 && list->getVerticalPosition() > 0.0
               && model->getNameForRow(199).contains("Error: Synthetic history message 30"),
           "Keyboard selection scrolls the oldest message into view with its error text, not only an error color.");
    expect(list->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)) && panel.historySize() == 199
               && std::none_of(panel.history().begin(), panel.history().end(),
                   [oldestId](const auto& entry) { return entry.id == oldestId; }),
           "Delete removes the selected stable message ID through the ordinary history action.");
    list->selectRow(10);
    const auto selectedName = model->getNameForRow(10);
    panel.pushMessage("Synthetic newly arrived error", true);
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    while (model->getNumRows() != 200 && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    expect(model->getNumRows() == 200
               && model->getNameForRow(list->getSelectedRow()) == selectedName,
           "New messages update an open history without retargeting its keyboard selection.");
    panel.clearHistory();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    expect(model->getNumRows() == 0 && !list->isVisible()
               && panel.getAccessibilityHandler() != nullptr
               && panel.getAccessibilityHandler()->getValueInterface() != nullptr
               && panel.getAccessibilityHandler()->getValueInterface()->getCurrentValueAsString() == "No status messages",
           "Clearing history updates the open native list and current accessible empty state.");
    auto emptyAction = false;
    for (auto* child : view->getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child);
            button != nullptr && button->getTitle() == "Clear all messages")
            emptyAction = button->isEnabled() && button->getWantsKeyboardFocus();
    expect(emptyAction, "An empty history retains a visible native keyboard target instead of focusing a hidden list.");
}
