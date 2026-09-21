#pragma once

#include "StudioPreferences.h"
#include "UpdateService.h"
#include "audio/StudioAudioDeviceManager.h"
#include "plugin_host/PluginCatalog.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>

namespace studio
{
class UpdateSettingsComponent final
    : public juce::Component,
      private UpdateService::Listener
{
public:
    UpdateSettingsComponent(
        UpdateService& updateService,
        std::function<void()> restartRequested);
    ~UpdateSettingsComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void updateStateChanged(
        const UpdateSnapshot& snapshot) override;
    void refresh(const UpdateSnapshot& snapshot);

    UpdateService& updateService;
    std::function<void()> restartRequested;
    double downloadProgress = 0.0;

    juce::Label titleLabel;
    juce::Label versionLabel;
    juce::Label statusLabel;
    juce::ToggleButton automaticDownloadToggle {
        "Download updates automatically"
    };
    juce::ProgressBar progressBar { downloadProgress };
    juce::TextButton checkButton { "CHECK FOR UPDATES" };
    juce::TextButton downloadButton { "DOWNLOAD" };
    juce::TextButton restartButton { "RESTART AND UPDATE" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        UpdateSettingsComponent)
};

class SettingsComponent final : public juce::Component
{
public:
    SettingsComponent(
        StudioAudioDeviceManager* deviceManager,
        UpdateService& updateService,
        StudioPreferences& preferences,
        PluginCatalog& pluginCatalog,
        std::function<void(const PluginCatalogEntry&)>
            validatePlugin,
        std::function<void(const StudioThemePalette&)>
            themeChanged,
        std::function<void()> restartRequested,
        bool showUpdatesInitially,
        const juce::String& audioUnavailableReason = {});

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void showUpdates();

private:
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioPage;
    juce::Label audioUnavailableLabel;
    std::unique_ptr<juce::Component> generalPage;
    std::unique_ptr<juce::Component> appearancePage;
    std::unique_ptr<juce::Component> vstPage;
    std::unique_ptr<UpdateSettingsComponent> updatePage;
    juce::TabbedComponent tabs {
        juce::TabbedButtonBar::TabsAtTop
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        SettingsComponent)
};
}
