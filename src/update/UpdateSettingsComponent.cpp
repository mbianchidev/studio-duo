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

    addAndMakeVisible(tabs);
    tabs.setTabBarDepth(34);
    tabs.setOutline(1);
    tabs.addTab(
        "General",
        juce::Colour(StudioColours::panel),
        generalPage.get(),
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
    tabs.setCurrentTabIndex(2);
}
}
