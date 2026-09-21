#include "StartupHubComponent.h"

#include "StudioTheme.h"

namespace studio
{
namespace
{
bool recentProjectIsReadable(
    const juce::File& package)
{
    if (!package.exists())
        return false;
    const auto manifest =
        package.getChildFile("manifest.json");
    if (!manifest.existsAsFile())
        return false;
    return manifest.createInputStream() != nullptr;
}
}

class StartupHubComponent::RecentProjectRow final
    : public juce::Component
{
public:
    RecentProjectRow(
        StartupHubComponent& ownerToUse,
        RecentProject projectToUse)
        : owner(ownerToUse),
          project(std::move(projectToUse)),
          readable(recentProjectIsReadable(project.file))
    {
        addAndMakeVisible(openButton);
        openButton.setButtonText(project.name);
        openButton.setTooltip(
            readable
                ? "Open " + project.file.getFullPathName()
                : "This recent project is missing or inaccessible");
        openButton.setEnabled(readable);
        openButton.onClick = [this]
        {
            if (!readable)
            {
                owner.setStatus(
                    "This project is missing or inaccessible. "
                    "Remove the stale entry or locate it with Open Existing Project.",
                    true);
                return;
            }
            if (!owner.onOpenRecent
                || !owner.onOpenRecent(project.file))
            {
                owner.setStatus(
                    "The recent project could not be opened. "
                    "You can remove the entry and locate the project again.",
                    true);
            }
        };

        addAndMakeVisible(details);
        details.setText(
            project.file.getFullPathName()
                + "    "
                + project.lastEdited.toString(
                    true,
                    true)
                + (readable
                       ? juce::String()
                       : juce::String(
                             "    Missing or inaccessible")),
            juce::dontSendNotification);
        details.setColour(
            juce::Label::textColourId,
            juce::Colour(
                readable
                    ? StudioColours::secondaryText
                    : StudioColours::orange));
        details.setMinimumHorizontalScale(0.7f);

        addAndMakeVisible(removeButton);
        removeButton.setButtonText("REMOVE");
        removeButton.setTooltip(
            "Remove this entry from Recent Projects");
        removeButton.onClick = [this]
        {
            const auto result =
                owner.preferences.removeRecentProject(
                    project.file);
            if (result.failed())
            {
                owner.setStatus(
                    result.getErrorMessage(),
                    true);
                return;
            }
            owner.setStatus(
                "Removed the recent project entry.");
            owner.refresh();
        };
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.setColour(
            juce::Colour(StudioColours::raised));
        graphics.fillRoundedRectangle(
            getLocalBounds().toFloat().reduced(0.5f),
            5.0f);
        graphics.setColour(
            juce::Colour(
                readable
                    ? StudioColours::border
                    : StudioColours::orange));
        graphics.drawRoundedRectangle(
            getLocalBounds().toFloat().reduced(0.5f),
            5.0f,
            1.0f);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10, 6);
        removeButton.setBounds(
            bounds.removeFromRight(86).reduced(2));
        openButton.setBounds(
            bounds.removeFromTop(28));
        details.setBounds(bounds);
    }

private:
    StartupHubComponent& owner;
    RecentProject project;
    bool readable = false;
    juce::TextButton openButton;
    juce::Label details;
    juce::TextButton removeButton;
};

StartupHubComponent::StartupHubComponent(
    StudioPreferences& preferencesToUse)
    : preferences(preferencesToUse)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    addAndMakeVisible(title);
    title.setText(
        "Start a Studio Duo session",
        juce::dontSendNotification);
    title.setFont(
        juce::Font(
            juce::FontOptions(
                28.0f,
                juce::Font::bold)));
    title.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(subtitle);
    subtitle.setText(
        "Create a song, start from a curated template, "
        "or continue a recent project.",
        juce::dontSendNotification);
    subtitle.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));
    subtitle.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(newSongButton);
    newSongButton.setTooltip(
        "Create a blank song with a master track");
    newSongButton.onClick = [this]
    {
        if (onNewSong)
            onNewSong();
    };

    addAndMakeVisible(openExistingButton);
    openExistingButton.setTooltip(
        "Choose an existing .studioduo project");
    openExistingButton.onClick = [this]
    {
        if (onOpenExisting)
            onOpenExisting();
    };

    addAndMakeVisible(templateTitle);
    templateTitle.setText(
        "FROM TEMPLATE",
        juce::dontSendNotification);
    templateTitle.setFont(
        juce::Font(
            juce::FontOptions(
                13.0f,
                juce::Font::bold)));

    addAndMakeVisible(templateSelector);
    int itemId = 1;
    for (const auto& descriptor :
         ProjectTemplates::descriptors())
    {
        templateSelector.addItem(
            descriptor.name,
            itemId++);
    }
    templateSelector.setTooltip(
        "Choose a bundled song template");
    templateSelector.onChange = [this]
    {
        updateTemplateDescription();
    };

    addAndMakeVisible(templateDescription);
    templateDescription.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));
    templateDescription.setJustificationType(
        juce::Justification::topLeft);

    addAndMakeVisible(createTemplateButton);
    createTemplateButton.setTooltip(
        "Create a new song from the selected template");
    createTemplateButton.onClick = [this]
    {
        const auto index =
            templateSelector.getSelectedItemIndex();
        const auto& templates =
            ProjectTemplates::descriptors();
        if (index < 0
            || index
                >= static_cast<int>(
                    templates.size()))
        {
            setStatus(
                "Choose a song template first.",
                true);
            return;
        }
        if (onCreateFromTemplate)
        {
            onCreateFromTemplate(
                templates[
                    static_cast<std::size_t>(
                        index)]
                    .id);
        }
    };

    const auto hasTemplates =
        !ProjectTemplates::descriptors().empty();
    templateSelector.setEnabled(hasTemplates);
    createTemplateButton.setEnabled(hasTemplates);
    if (hasTemplates)
    {
        templateSelector.setSelectedId(
            1,
            juce::dontSendNotification);
        updateTemplateDescription();
    }
    else
    {
        templateDescription.setText(
            "No song templates are currently available.",
            juce::dontSendNotification);
    }

    addAndMakeVisible(recentTitle);
    recentTitle.setText(
        "RECENT PROJECTS",
        juce::dontSendNotification);
    recentTitle.setFont(
        juce::Font(
            juce::FontOptions(
                13.0f,
                juce::Font::bold)));

    addAndMakeVisible(recentViewport);
    recentViewport.setViewedComponent(
        &recentContent,
        false);
    recentViewport.setScrollBarsShown(
        true,
        false);

    addChildComponent(emptyRecentLabel);
    emptyRecentLabel.setText(
        "No recent projects yet. Save a song and it will appear here.",
        juce::dontSendNotification);
    emptyRecentLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));
    emptyRecentLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(status);
    status.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));
    status.setJustificationType(
        juce::Justification::centred);
    status.setMinimumHorizontalScale(0.75f);
    refresh();
}

StartupHubComponent::~StartupHubComponent()
{
    recentViewport.setViewedComponent(
        nullptr,
        false);
}

void StartupHubComponent::refresh()
{
    rebuildRecentRows();
    resized();
    repaint();
}

void StartupHubComponent::setStatus(
    const juce::String& message,
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

void StartupHubComponent::paint(
    juce::Graphics& graphics)
{
    graphics.fillAll(
        juce::Colour(StudioColours::window));
    auto card =
        getLocalBounds()
            .reduced(24)
            .toFloat();
    graphics.setColour(
        juce::Colour(StudioColours::panel));
    graphics.fillRoundedRectangle(
        card,
        10.0f);
    graphics.setColour(
        juce::Colour(StudioColours::border));
    graphics.drawRoundedRectangle(
        card,
        10.0f,
        1.0f);
}

void StartupHubComponent::resized()
{
    if (getWidth() < 120 || getHeight() < 120)
        return;
    auto bounds = getLocalBounds()
        .withSizeKeepingCentre(
            juce::jmax(
                1,
                juce::jmin(
                    900,
                    getWidth() - 48)),
            juce::jmax(
                1,
                juce::jmin(
                    680,
                    getHeight() - 32)))
        .reduced(24);
    title.setBounds(
        bounds.removeFromTop(40));
    subtitle.setBounds(
        bounds.removeFromTop(28));
    bounds.removeFromTop(14);
    auto actions =
        bounds.removeFromTop(42);
    const auto actionWidth =
        juce::jmax(
            160,
            (actions.getWidth() - 12) / 2);
    newSongButton.setBounds(
        actions.removeFromLeft(
            actionWidth)
            .reduced(2));
    actions.removeFromLeft(12);
    openExistingButton.setBounds(
        actions.reduced(2));
    bounds.removeFromTop(18);
    templateTitle.setBounds(
        bounds.removeFromTop(22));
    auto templateRow =
        bounds.removeFromTop(42);
    createTemplateButton.setBounds(
        templateRow.removeFromRight(210)
            .reduced(2));
    templateSelector.setBounds(
        templateRow.reduced(2));
    templateDescription.setBounds(
        bounds.removeFromTop(44));
    bounds.removeFromTop(10);
    recentTitle.setBounds(
        bounds.removeFromTop(22));
    auto statusBounds =
        bounds.removeFromBottom(30);
    status.setBounds(statusBounds);
    recentViewport.setBounds(bounds);
    emptyRecentLabel.setBounds(bounds);

    const auto contentWidth =
        juce::jmax(
            1,
            recentViewport.getWidth()
                - recentViewport
                      .getScrollBarThickness()
                - 2);
    const auto rowHeight = 58;
    recentContent.setSize(
        contentWidth,
        juce::jmax(
            recentViewport.getHeight(),
            static_cast<int>(
                recentRows.size())
                    * (rowHeight + 6)));
    auto rowBounds =
        recentContent.getLocalBounds();
    for (auto& row : recentRows)
    {
        row->setBounds(
            rowBounds.removeFromTop(
                rowHeight));
        rowBounds.removeFromTop(6);
    }
}

void StartupHubComponent::updateTemplateDescription()
{
    const auto index =
        templateSelector.getSelectedItemIndex();
    const auto& templates =
        ProjectTemplates::descriptors();
    templateDescription.setText(
        index >= 0
            && index < static_cast<int>(
                templates.size())
            ? templates[
                  static_cast<std::size_t>(
                      index)]
                  .description
            : juce::String(
                  "No song templates are currently available."),
        juce::dontSendNotification);
}

void StartupHubComponent::rebuildRecentRows()
{
    recentRows.clear();
    recentContent.removeAllChildren();
    for (const auto& recent :
         preferences.recentProjects())
    {
        auto row =
            std::make_unique<RecentProjectRow>(
                *this,
                recent);
        recentContent.addAndMakeVisible(*row);
        recentRows.push_back(std::move(row));
    }
    const auto empty = recentRows.empty();
    recentViewport.setVisible(!empty);
    emptyRecentLabel.setVisible(empty);
}
}
