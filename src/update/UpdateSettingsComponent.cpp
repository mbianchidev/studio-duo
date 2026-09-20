#include "UpdateSettingsComponent.h"

#include "ui/StudioTheme.h"

namespace studio
{
namespace
{
class GeneralSettingsComponent final : public juce::Component
{
public:
    explicit GeneralSettingsComponent(
        StudioPreferences& preferencesToUse)
        : preferences(preferencesToUse)
    {
        addAndMakeVisible(title);
        title.setText(
            "Project preferences",
            juce::dontSendNotification);
        title.setFont(
            juce::Font(
                juce::FontOptions(22.0f,
                                  juce::Font::bold)));

        addAndMakeVisible(autosave);
        autosave.setButtonText(
            "Autosave project recovery after edits");
        autosave.setTooltip(
            "Write the latest project state to the package recovery copy after each edit.");
        autosave.setToggleState(
            preferences.autosaveEnabled(),
            juce::dontSendNotification);
        autosave.onClick = [this]
        {
            const auto enabled =
                autosave.getToggleState();
            const auto result =
                preferences.setAutosaveEnabled(enabled);
            if (result.failed())
            {
                autosave.setToggleState(
                    !enabled,
                    juce::dontSendNotification);
                status.setText(
                    result.getErrorMessage(),
                    juce::dontSendNotification);
                status.setColour(
                    juce::Label::textColourId,
                    juce::Colour(StudioColours::orange));
                return;
            }
            status.setText(
                enabled
                    ? "Autosave recovery is enabled."
                    : "Autosave recovery is disabled.",
                juce::dontSendNotification);
            status.setColour(
                juce::Label::textColourId,
                juce::Colour(
                    StudioColours::secondaryText));
        };

        addAndMakeVisible(help);
        help.setText(
            "Autosave writes a recovery copy inside saved .studioduo projects. "
            "Manual Save still creates the durable project generation.",
            juce::dontSendNotification);
        help.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
        help.setJustificationType(
            juce::Justification::topLeft);

        addAndMakeVisible(status);
        status.setText(
            preferences.status(),
            juce::dontSendNotification);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(
            juce::Colour(StudioColours::panel));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(24);
        title.setBounds(bounds.removeFromTop(36));
        bounds.removeFromTop(18);
        autosave.setBounds(bounds.removeFromTop(32));
        help.setBounds(bounds.removeFromTop(58));
        status.setBounds(bounds.removeFromTop(28));
    }

private:
    StudioPreferences& preferences;
    juce::Label title;
    juce::ToggleButton autosave;
    juce::Label help;
    juce::Label status;
};

class VstPluginSettingsComponent final
    : public juce::Component,
      private juce::ListBoxModel
{
public:
    VstPluginSettingsComponent(
        PluginCatalog& catalogToUse,
        StudioPreferences& preferencesToUse)
        : catalog(catalogToUse),
          preferences(preferencesToUse),
          list("VST3 search folders", this)
    {
        addAndMakeVisible(title);
        title.setText(
            "VST plug-ins",
            juce::dontSendNotification);
        title.setFont(
            juce::Font(
                juce::FontOptions(22.0f,
                                  juce::Font::bold)));

        addAndMakeVisible(scanAtStartup);
        scanAtStartup.setButtonText(
            "Scan plug-in folders at startup");
        scanAtStartup.setToggleState(
            preferences.scanPluginsAtStartup(),
            juce::dontSendNotification);
        scanAtStartup.onClick = [this]
        {
            const auto enabled =
                scanAtStartup.getToggleState();
            const auto result =
                preferences.setScanPluginsAtStartup(
                    enabled);
            if (result.failed())
            {
                scanAtStartup.setToggleState(
                    !enabled,
                    juce::dontSendNotification);
                setStatus(
                    result.getErrorMessage(),
                    true);
            }
            else
            {
                setStatus(
                    enabled
                        ? "Startup plug-in scan enabled."
                        : "Startup plug-in scan disabled.",
                    false);
            }
        };

        addAndMakeVisible(list);
        list.setRowHeight(34);
        list.setColour(
            juce::ListBox::backgroundColourId,
            juce::Colour(StudioColours::window));
        list.setColour(
            juce::ListBox::outlineColourId,
            juce::Colour(StudioColours::border));
        list.setOutlineThickness(1);

        addAndMakeVisible(addFolder);
        addFolder.setButtonText("ADD FOLDER");
        addFolder.onClick = [this] { chooseFolder(); };
        addAndMakeVisible(removeFolder);
        removeFolder.setButtonText("REMOVE");
        removeFolder.onClick = [this] { removeSelected(); };
        addAndMakeVisible(restoreDefaults);
        restoreDefaults.setButtonText("RESTORE DEFAULTS");
        restoreDefaults.onClick = [this]
        {
            const auto result =
                catalog.restoreDefaultVst3SearchFolders();
            setStatus(
                result.wasOk()
                    ? "Default VST3 folders restored."
                    : result.getErrorMessage(),
                result.failed());
            refresh();
        };
        addAndMakeVisible(rescan);
        rescan.setButtonText("RESCAN NOW");
        rescan.onClick = [this]
        {
            catalog.startScan(true);
            setStatus(
                "VST3 rescan started.",
                false);
        };

        addAndMakeVisible(status);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
        refresh();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(
            juce::Colour(StudioColours::panel));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(24);
        title.setBounds(bounds.removeFromTop(36));
        scanAtStartup.setBounds(
            bounds.removeFromTop(32));
        bounds.removeFromTop(8);
        auto actions = bounds.removeFromBottom(36);
        addFolder.setBounds(
            actions.removeFromLeft(110).reduced(2));
        removeFolder.setBounds(
            actions.removeFromLeft(92).reduced(2));
        restoreDefaults.setBounds(
            actions.removeFromLeft(150).reduced(2));
        rescan.setBounds(
            actions.removeFromLeft(112).reduced(2));
        status.setBounds(
            bounds.removeFromBottom(28));
        bounds.removeFromBottom(8);
        list.setBounds(bounds);
    }

private:
    int getNumRows() override
    {
        return paths.size();
    }

    void paintListBoxItem(int row,
                          juce::Graphics& graphics,
                          int width,
                          int height,
                          bool selected) override
    {
        if (row < 0 || row >= paths.size())
            return;
        if (selected)
        {
            graphics.setColour(
                juce::Colour(StudioColours::raised));
            graphics.fillRect(
                0,
                0,
                width,
                height);
        }
        const auto file = juce::File(paths[row]);
        graphics.setColour(
            file.isDirectory()
                ? juce::Colour(StudioColours::text)
                : juce::Colour(StudioColours::orange));
        graphics.setFont(10.5f);
        graphics.drawFittedText(
            paths[row],
            8,
            2,
            width - 90,
            height - 4,
            juce::Justification::centredLeft,
            1);
        graphics.setColour(
            juce::Colour(StudioColours::secondaryText));
        graphics.setFont(
            juce::Font(
                juce::FontOptions(8.5f,
                                  juce::Font::bold)));
        graphics.drawText(
            defaultFlags[row] ? "DEFAULT" : "CUSTOM",
            width - 78,
            0,
            70,
            height,
            juce::Justification::centredRight);
    }

    void selectedRowsChanged(int row) override
    {
        removeFolder.setEnabled(
            row >= 0 && row < paths.size());
    }

    void refresh()
    {
        paths.clear();
        defaultFlags.clear();
        const auto defaults =
            catalog.defaultVst3SearchFolders();
        const auto disabled =
            catalog.disabledDefaultVst3SearchFolders();
        for (const auto& path : defaults)
        {
            if (!disabled.contains(path))
            {
                paths.add(path);
                defaultFlags.add(true);
            }
        }
        for (const auto& path :
             catalog.customVst3SearchFolders())
        {
            if (!paths.contains(path))
            {
                paths.add(path);
                defaultFlags.add(false);
            }
        }
        list.updateContent();
        list.deselectAllRows();
        removeFolder.setEnabled(false);
        repaint();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Add VST3 search folder",
            juce::File::getSpecialLocation(
                juce::File::userHomeDirectory));
        const auto safe =
            juce::Component::SafePointer<
                VstPluginSettingsComponent>(this);
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& completed)
            {
                if (safe == nullptr)
                    return;
                const auto folder = completed.getResult();
                if (folder == juce::File())
                    return;
                const auto result =
                    safe->catalog.addCustomVst3SearchFolder(
                        folder);
                safe->setStatus(
                    result.wasOk()
                        ? "VST3 folder added."
                        : result.getErrorMessage(),
                    result.failed());
                safe->refresh();
            });
    }

    void removeSelected()
    {
        const auto row = list.getSelectedRow();
        if (row < 0 || row >= paths.size())
            return;
        const auto result =
            catalog.removeVst3SearchFolder(
                juce::File(paths[row]));
        setStatus(
            result.wasOk()
                ? "VST3 folder removed."
                : result.getErrorMessage(),
            result.failed());
        refresh();
    }

    void setStatus(const juce::String& message,
                   bool error)
    {
        status.setText(
            message,
            juce::dontSendNotification);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(
                error
                    ? StudioColours::orange
                    : StudioColours::secondaryText));
    }

    PluginCatalog& catalog;
    StudioPreferences& preferences;
    juce::Label title;
    juce::ToggleButton scanAtStartup;
    juce::ListBox list;
    juce::TextButton addFolder;
    juce::TextButton removeFolder;
    juce::TextButton restoreDefaults;
    juce::TextButton rescan;
    juce::Label status;
    juce::StringArray paths;
    juce::Array<bool> defaultFlags;
    std::unique_ptr<juce::FileChooser> chooser;
};
}

UpdateSettingsComponent::UpdateSettingsComponent(
    UpdateService& service,
    std::function<void()> restartCallback)
    : updateService(service),
      restartRequested(std::move(restartCallback))
{
    addAndMakeVisible(titleLabel);
    titleLabel.setText(
        "Studio Duo updates",
        juce::dontSendNotification);
    titleLabel.setFont(
        juce::Font(
            juce::FontOptions(22.0f, juce::Font::bold)));
    titleLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::text));

    addAndMakeVisible(versionLabel);
    versionLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::secondaryText));

    addAndMakeVisible(statusLabel);
    statusLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::text));
    statusLabel.setJustificationType(
        juce::Justification::topLeft);
    statusLabel.setMinimumHorizontalScale(0.8f);

    addAndMakeVisible(automaticDownloadToggle);
    automaticDownloadToggle.setTooltip(
        "Download verified updates in the background after launch");
    automaticDownloadToggle.onClick = [this]
    {
        const auto requested =
            automaticDownloadToggle.getToggleState();
        const auto result =
            updateService.setAutomaticDownloads(requested);
        if (result.failed())
        {
            automaticDownloadToggle.setToggleState(
                !requested,
                juce::dontSendNotification);
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Update settings could not be saved",
                result.getErrorMessage());
        }
    };

    addAndMakeVisible(progressBar);
    progressBar.setPercentageDisplay(true);

    addAndMakeVisible(checkButton);
    checkButton.setTooltip(
        "Check the official Studio Duo release feed now");
    checkButton.onClick = [this]
    {
        updateService.checkForUpdates();
    };

    addAndMakeVisible(downloadButton);
    downloadButton.setTooltip(
        "Download and verify the available update");
    downloadButton.onClick = [this]
    {
        updateService.downloadUpdate();
    };

    addAndMakeVisible(restartButton);
    restartButton.setTooltip(
        "Quit Studio Duo, install the downloaded update, and reopen it");
    restartButton.onClick = [this]
    {
        if (restartRequested)
            restartRequested();
    };

    updateService.addListener(this);
    refresh(updateService.snapshot());
}

UpdateSettingsComponent::~UpdateSettingsComponent()
{
    updateService.removeListener(this);
}

void UpdateSettingsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void UpdateSettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced(24);
    titleLabel.setBounds(bounds.removeFromTop(34));
    versionLabel.setBounds(bounds.removeFromTop(26));
    bounds.removeFromTop(14);
    statusLabel.setBounds(bounds.removeFromTop(72));
    bounds.removeFromTop(10);
    automaticDownloadToggle.setBounds(
        bounds.removeFromTop(30));
    bounds.removeFromTop(16);
    progressBar.setBounds(bounds.removeFromTop(24));

    auto buttons = bounds.removeFromBottom(38);
    checkButton.setBounds(
        buttons.removeFromLeft(168).reduced(3));
    downloadButton.setBounds(
        buttons.removeFromLeft(132).reduced(3));
    restartButton.setBounds(
        buttons.removeFromLeft(188).reduced(3));
}

void UpdateSettingsComponent::updateStateChanged(
    const UpdateSnapshot& snapshot)
{
    refresh(snapshot);
}

void UpdateSettingsComponent::refresh(
    const UpdateSnapshot& snapshot)
{
    auto versionText =
        "Current version: " + snapshot.currentVersion;
    if (snapshot.availableVersion.isNotEmpty())
    {
        versionText +=
            "    Available: " + snapshot.availableVersion;
    }
    versionLabel.setText(
        versionText,
        juce::dontSendNotification);
    statusLabel.setText(
        snapshot.message,
        juce::dontSendNotification);
    statusLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(
            snapshot.phase == UpdatePhase::failed
                ? StudioColours::orange
                : StudioColours::text));
    automaticDownloadToggle.setToggleState(
        snapshot.automaticDownloads,
        juce::dontSendNotification);
    automaticDownloadToggle.setEnabled(
        snapshot.phase != UpdatePhase::unsupported);

    downloadProgress = snapshot.progress;
    progressBar.setVisible(
        snapshot.phase == UpdatePhase::downloading);

    const auto busy =
        snapshot.phase == UpdatePhase::checking
        || snapshot.phase == UpdatePhase::downloading;
    checkButton.setEnabled(
        !busy
        && snapshot.phase != UpdatePhase::unsupported);
    downloadButton.setEnabled(
        !busy
        && snapshot.availableVersion.isNotEmpty()
        && snapshot.phase != UpdatePhase::ready);
    downloadButton.setButtonText(
        snapshot.phase == UpdatePhase::failed
            && snapshot.availableVersion.isNotEmpty()
            ? "RETRY DOWNLOAD"
            : "DOWNLOAD");
    restartButton.setEnabled(
        snapshot.phase == UpdatePhase::ready);
}

SettingsComponent::SettingsComponent(
    StudioAudioDeviceManager* deviceManager,
    UpdateService& updateService,
    StudioPreferences& preferences,
    PluginCatalog& pluginCatalog,
    std::function<void()> restartRequested,
    bool showUpdatesInitially,
    const juce::String& audioUnavailableReason)
{
    juce::Component* audioContent = &audioUnavailableLabel;
    if (deviceManager != nullptr)
    {
        deviceManager->prepareDeviceTypesForSettings();
        audioPage = std::make_unique<juce::AudioDeviceSelectorComponent>(
            *deviceManager,
            0,
            maximumHardwareAudioChannels,
            0,
            maximumHardwareAudioChannels,
            true,
            true,
            false,
            false);
        audioContent = audioPage.get();
    }
    else
    {
        audioUnavailableLabel.setText(
            audioUnavailableReason.isNotEmpty()
                ? audioUnavailableReason
                : "Audio is disabled. Close this window and open Settings again "
                  "to configure audio. Updates remain available.",
            juce::dontSendNotification);
        audioUnavailableLabel.setJustificationType(juce::Justification::centred);
        audioUnavailableLabel.setColour(
            juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    }
    updatePage = std::make_unique<UpdateSettingsComponent>(
        updateService,
        std::move(restartRequested));
    generalPage =
        std::make_unique<GeneralSettingsComponent>(
            preferences);
    vstPage =
        std::make_unique<VstPluginSettingsComponent>(
            pluginCatalog,
            preferences);

    addAndMakeVisible(tabs);
    tabs.setTabBarDepth(34);
    tabs.setOutline(1);
    tabs.addTab(
        "General",
        juce::Colour(StudioColours::panel),
        generalPage.get(),
        false);
    tabs.addTab(
        "VST Plug-ins",
        juce::Colour(StudioColours::panel),
        vstPage.get(),
        false);
    tabs.addTab(
        "Audio / MIDI",
        juce::Colour(StudioColours::panel),
        audioContent,
        false);
    tabs.addTab(
        "Updates",
        juce::Colour(StudioColours::panel),
        updatePage.get(),
        false);
    if (showUpdatesInitially)
        showUpdates();
    setSize(680, 520);
}

void SettingsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void SettingsComponent::resized()
{
    tabs.setBounds(getLocalBounds());
}

void SettingsComponent::showUpdates()
{
    tabs.setCurrentTabIndex(3);
}
}
