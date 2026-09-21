#pragma once

#include "PluginCatalog.h"
#include "ui/StudioIconButton.h"

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
    std::function<void(const PluginCatalogEntry&)> onPluginValidate;

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
    void showVst3FolderMenu();
    void beginAddVst3Folder();

    PluginCatalog& catalog;
    juce::TextEditor search;
    StudioIconButton scanButton {
        StudioIcon::scan,
        "Scan plugins",
        "Scan default VST3, Audio Unit, and CLAP locations in a worker process"
    };
    StudioIconButton pathsButton {
        StudioIcon::folder,
        "Plugin search paths",
        "Show, add, or remove VST3 plugin search folders"
    };
    StudioIconButton addButton {
        StudioIcon::add,
        "Add plugin",
        "Add the selected plugin to the selected track"
    };
    juce::TextButton validateButton { "TEST" };
    juce::Label statusLabel;
    juce::ListBox list { "Plugin catalog", this };
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    std::vector<PluginCatalogEntry> allEntries;
    std::vector<PluginCatalogEntry> filteredEntries;
    std::unique_ptr<juce::FileChooser> folderChooser;
    std::uint64_t lastRevision = 0;
    int selectedRow = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBrowserComponent)
};
}
