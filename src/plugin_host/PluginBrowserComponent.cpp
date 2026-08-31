#include "PluginBrowserComponent.h"

#include "ui/StudioTheme.h"

namespace studio
{
PluginBrowserComponent::PluginBrowserComponent(PluginCatalog& catalogToDisplay)
    : catalog(catalogToDisplay)
{
    setOpaque(true);

    addAndMakeVisible(search);
    search.setTextToShowWhenEmpty("Filter plugins", juce::Colour(StudioColours::secondaryText));
    search.setTooltip("Filter by plugin, manufacturer, category, or format");
    search.onTextChange = [this] { rebuildFilter(); };

    addAndMakeVisible(scanButton);
    scanButton.setTooltip("Scan default VST3 and Audio Unit locations in a worker process");
    scanButton.onClick = [this]
    {
        if (catalog.isScanning())
            catalog.cancelScan();
        else
            catalog.startScan(false);
    };

    addAndMakeVisible(statusLabel);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    statusLabel.setFont(juce::Font(juce::FontOptions(10.5f)));
    statusLabel.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(progressBar);
    progressBar.setPercentageDisplay(false);
    progressBar.setColour(juce::ProgressBar::backgroundColourId, juce::Colour(StudioColours::window));
    progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(StudioColours::orange));

    addAndMakeVisible(list);
    list.setRowHeight(44);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colour(StudioColours::panel));
    list.setColour(juce::ListBox::outlineColourId, juce::Colour(StudioColours::border));
    list.setOutlineThickness(1);

    rebuildFilter();
    startTimerHz(10);
}

PluginBrowserComponent::~PluginBrowserComponent()
{
    stopTimer();
}

void PluginBrowserComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
    graphics.drawText("PLUGIN CATALOG",
                      0,
                      0,
                      getWidth(),
                      20,
                      juce::Justification::centredLeft);
}

void PluginBrowserComponent::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(24);
    auto controls = bounds.removeFromTop(30);
    scanButton.setBounds(controls.removeFromRight(64));
    controls.removeFromRight(6);
    search.setBounds(controls);
    bounds.removeFromTop(7);
    progressBar.setBounds(bounds.removeFromTop(3));
    bounds.removeFromTop(7);
    statusLabel.setBounds(bounds.removeFromTop(42));
    bounds.removeFromTop(4);
    list.setBounds(bounds);
}

int PluginBrowserComponent::getNumRows()
{
    return static_cast<int>(filteredEntries.size());
}

void PluginBrowserComponent::paintListBoxItem(int row,
                                              juce::Graphics& graphics,
                                              int width,
                                              int height,
                                              bool selected)
{
    if (row < 0 || row >= static_cast<int>(filteredEntries.size()))
        return;

    const auto& entry = filteredEntries[static_cast<std::size_t>(row)];
    if (selected)
    {
        graphics.setColour(juce::Colour(0xff292e32));
        graphics.fillRect(0, 0, width, height);
    }

    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    graphics.drawFittedText(entry.name,
                            8,
                            4,
                            width - 16,
                            18,
                            juce::Justification::centredLeft,
                            1);

    auto detail = entry.manufacturer;
    if (detail.isNotEmpty())
        detail << "  |  ";
    detail << entry.format;
    if (entry.instrument)
        detail << "  |  INSTRUMENT";

    graphics.setColour(juce::Colour(entry.instrument ? StudioColours::green
                                                     : StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(9.5f)));
    graphics.drawFittedText(detail,
                            8,
                            23,
                            width - 16,
                            16,
                            juce::Justification::centredLeft,
                            1);

    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawHorizontalLine(height - 1, 6.0f, static_cast<float>(width - 6));
}

void PluginBrowserComponent::selectedRowsChanged(int lastRowSelected)
{
    if (lastRowSelected < 0 || lastRowSelected >= static_cast<int>(filteredEntries.size()))
        return;

    const auto& entry = filteredEntries[static_cast<std::size_t>(lastRowSelected)];
    juce::String description = entry.name + " | " + entry.format;
    if (entry.version.isNotEmpty())
        description << " | " << entry.version;
    description << " | " << juce::String(entry.inputChannels)
                << " in / " << juce::String(entry.outputChannels) << " out";
    statusLabel.setText(description, juce::dontSendNotification);
}

void PluginBrowserComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0
        && row < static_cast<int>(filteredEntries.size())
        && onPluginActivated)
        onPluginActivated(filteredEntries[static_cast<std::size_t>(row)]);
}

void PluginBrowserComponent::timerCallback()
{
    progressValue = catalog.progress();
    scanButton.setButtonText(catalog.isScanning() ? "CANCEL" : "SCAN");

    const auto currentRevision = catalog.revision();
    if (currentRevision == lastRevision)
        return;

    lastRevision = currentRevision;
    statusLabel.setText(catalog.status(), juce::dontSendNotification);
    rebuildFilter();
}

void PluginBrowserComponent::rebuildFilter()
{
    allEntries = catalog.entries();
    filteredEntries.clear();
    filteredEntries.reserve(allEntries.size());

    for (const auto& entry : allEntries)
        if (PluginCatalog::matchesQuery(entry, search.getText()))
            filteredEntries.push_back(entry);

    list.updateContent();
    list.repaint();
}
}
