#pragma once

#include "model/ProjectTemplates.h"
#include "update/StudioPreferences.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <vector>

namespace studio
{
class StartupHubComponent final : public juce::Component
{
public:
    explicit StartupHubComponent(
        StudioPreferences& preferences);
    ~StartupHubComponent() override;

    std::function<void()> onNewSong;
    std::function<void()> onOpenExisting;
    std::function<void(const juce::String&)>
        onCreateFromTemplate;
    std::function<bool(const juce::File&)>
        onOpenRecent;

    void refresh();
    void setStatus(const juce::String& message,
                   bool error = false);
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class RecentProjectRow;

    void updateTemplateDescription();
    void rebuildRecentRows();

    StudioPreferences& preferences;
    juce::Label title;
    juce::Label subtitle;
    juce::TextButton newSongButton {
        "NEW SONG"
    };
    juce::TextButton openExistingButton {
        "OPEN EXISTING PROJECT"
    };
    juce::Label templateTitle;
    juce::ComboBox templateSelector;
    juce::Label templateDescription;
    juce::TextButton createTemplateButton {
        "CREATE FROM TEMPLATE"
    };
    juce::Label recentTitle;
    juce::Viewport recentViewport;
    juce::Component recentContent;
    juce::Label emptyRecentLabel;
    juce::Label status;
    std::vector<std::unique_ptr<RecentProjectRow>>
        recentRows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        StartupHubComponent)
};
}
