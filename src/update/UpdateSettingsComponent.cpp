#include "UpdateSettingsComponent.h"

#include "ui/StudioTheme.h"

namespace studio
{
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

    downloadProgress = snapshot.progress;
    progressBar.setVisible(
        snapshot.phase == UpdatePhase::downloading);

    const auto busy =
        snapshot.phase == UpdatePhase::checking
        || snapshot.phase == UpdatePhase::downloading;
    checkButton.setEnabled(
        !busy
        && snapshot.phase != UpdatePhase::ready
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
    StudioAudioDeviceManager& deviceManager,
    UpdateService& updateService,
    std::function<void()> restartRequested,
    bool showUpdatesInitially)
{
    deviceManager.prepareDeviceTypesForSettings();
    audioPage =
        std::make_unique<juce::AudioDeviceSelectorComponent>(
            deviceManager,
            0,
            maximumHardwareAudioChannels,
            0,
            maximumHardwareAudioChannels,
            true,
            true,
            false,
            false);
    updatePage = std::make_unique<UpdateSettingsComponent>(
        updateService,
        std::move(restartRequested));

    addAndMakeVisible(tabs);
    tabs.setTabBarDepth(34);
    tabs.setOutline(1);
    tabs.addTab(
        "Audio / MIDI",
        juce::Colour(StudioColours::panel),
        audioPage.get(),
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
    tabs.setCurrentTabIndex(1);
}
}
