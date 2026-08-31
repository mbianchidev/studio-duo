#pragma once

#include "PluginCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace studio
{
class PluginBrowserComponent final : public juce::Component,
                                     private juce::ListBoxModel,
                                     private juce::Timer
{
public:
    explicit PluginBrowserComponent(PluginCatalog& catalogToDisplay);
    ~PluginBrowserComponent() override;

    std::function<void(const PluginCatalogEntry&)> onPluginActivated;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row,
                          juce::Graphics& graphics,
                          int width,
                          int height,
                          bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void timerCallback() override;
    void rebuildFilter();

    PluginCatalog& catalog;
    juce::TextEditor search;
    juce::TextButton scanButton { "SCAN" };
    juce::TextButton addButton { "ADD" };
    juce::Label statusLabel;
    juce::ListBox list { "Plugin catalog", this };
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    std::vector<PluginCatalogEntry> allEntries;
    std::vector<PluginCatalogEntry> filteredEntries;
    std::uint64_t lastRevision = 0;
    int selectedRow = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBrowserComponent)
};
}
