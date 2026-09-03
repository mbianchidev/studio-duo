#include "MixerPanel.h"

#include "StudioTheme.h"

#include <algorithm>
#include <cmath>

namespace studio
{
class MixerPanel::ItemList final : public juce::Component
{
public:
    explicit ItemList(MixerPanel& ownerToUse)
        : owner(ownerToUse)
    {
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(StudioColours::panel));
        const auto values = owner.items();
        if (values.empty())
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(10.0f);
            graphics.drawFittedText(
                "No attached inserts or sends.",
                getLocalBounds().reduced(8),
                juce::Justification::topLeft,
                2);
            return;
        }

        constexpr auto rowHeight = 36;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            const auto& item = values[index];
            const juce::Rectangle<int> row(
                4,
                static_cast<int>(index) * rowHeight,
                getWidth() - 8,
                rowHeight - 2);
            graphics.setColour(juce::Colour(StudioColours::raised));
            graphics.fillRoundedRectangle(row.toFloat(), 3.0f);
            graphics.setColour(juce::Colour(StudioColours::border));
            graphics.drawRoundedRectangle(row.toFloat(), 3.0f, 1.0f);

            const auto textWidth = item.type == Item::Type::plugin
                ? row.getWidth() - 58
                : row.getWidth() - 12;
            graphics.setColour(juce::Colour(StudioColours::text));
            graphics.setFont(juce::Font(
                juce::FontOptions(10.5f, juce::Font::bold)));
            graphics.drawFittedText(
                item.title,
                row.getX() + 7,
                row.getY() + 2,
                textWidth,
                15,
                juce::Justification::centredLeft,
                1);
            graphics.setColour(juce::Colour(
                StudioColours::secondaryText));
            graphics.setFont(juce::Font(juce::FontOptions(8.5f)));
            graphics.drawFittedText(
                item.detail,
                row.getX() + 7,
                row.getY() + 18,
                textWidth,
                13,
                juce::Justification::centredLeft,
                1);

            if (item.type == Item::Type::plugin)
            {
                const auto toggle = row.withLeft(
                    row.getRight() - 46).reduced(5, 6);
                graphics.setColour(juce::Colour(
                    item.enabled
                        ? StudioColours::green
                        : StudioColours::amber));
                graphics.fillRoundedRectangle(toggle.toFloat(), 3.0f);
                graphics.setColour(juce::Colours::white);
                graphics.setFont(juce::Font(
                    juce::FontOptions(8.5f, juce::Font::bold)));
                graphics.drawText(
                    item.enabled ? "ON" : "OFF",
                    toggle,
                    juce::Justification::centred);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        constexpr auto rowHeight = 36;
        const auto values = owner.items();
        const auto index = event.y / rowHeight;
        if (index < 0 || index >= static_cast<int>(values.size()))
            return;

        const auto& item = values[static_cast<std::size_t>(index)];
        const juce::Rectangle<int> row(
            4,
            index * rowHeight,
            getWidth() - 8,
            rowHeight - 2);
        if (item.type == Item::Type::plugin
            && row.withLeft(row.getRight() - 46)
                   .contains(event.getPosition()))
        {
            if (owner.onPluginEnabledChanged)
            {
                owner.onPluginEnabledChanged(
                    item.trackId,
                    item.objectId,
                    !item.enabled);
            }
            return;
        }

        if (item.type == Item::Type::plugin)
        {
            if (owner.onPluginOpen)
                owner.onPluginOpen(item.trackId, item.objectId);
        }
        else if (owner.onRouteOpen)
        {
            owner.onRouteOpen(item.trackId, item.objectId);
        }
    }

private:
    MixerPanel& owner;
};

MixerPanel::MixerPanel()
{
    itemList = std::make_unique<ItemList>(*this);
    itemsViewport.setViewedComponent(itemList.get(), false);
    itemsViewport.setScrollBarsShown(true, false);
    itemsViewport.setScrollBarThickness(8);
    addAndMakeVisible(itemsViewport);
}

MixerPanel::~MixerPanel()
{
    itemsViewport.setViewedComponent(nullptr, false);
}

void MixerPanel::setProject(const Project* value)
{
    project = value;
    refreshItems();
    repaint();
}

void MixerPanel::setSelection(const juce::String& value)
{
    selectedTrack = value;
    repaint();
}

void MixerPanel::setPeaks(float left, float right)
{
    leftPeak = left;
    rightPeak = right;
    repaint();
}

void MixerPanel::setMeters(
    std::vector<StudioAudioEngine::TrackMeterSnapshot> value)
{
    meters = std::move(value);
    repaint();
}

std::vector<const Track*> MixerPanel::mixerTracks() const
{
    std::vector<const Track*> result;
    if (project == nullptr)
        return result;

    for (const auto& track : project->tracks)
        if (track.parentTrackId.isEmpty())
            result.push_back(&track);
    return result;
}

std::vector<MixerPanel::Item> MixerPanel::items() const
{
    std::vector<Item> result;
    if (project == nullptr)
        return result;

    for (const auto& track : project->tracks)
    {
        if (track.parentTrackId.isNotEmpty())
            continue;
        for (const auto& insert : track.inserts)
        {
            result.push_back({
                Item::Type::plugin,
                track.id,
                insert.id,
                track.name + " / " + insert.name,
                insert.format + " INSERT",
                !insert.bypassed
            });
        }
        for (const auto& route : project->routingConnections)
        {
            if (route.sourceTrackId != track.id
                || route.kind == RouteKind::mainOutput)
                continue;
            result.push_back({
                Item::Type::route,
                track.id,
                route.id,
                track.name + " / " + route.name,
                routeKindToString(route.kind).toUpperCase()
                    + " / "
                    + routeTapToString(route.tap).toUpperCase(),
                route.enabled && !route.muted
            });
        }
    }
    return result;
}

void MixerPanel::refreshItems()
{
    if (itemList == nullptr)
        return;
    constexpr auto rowHeight = 36;
    itemList->setSize(
        juce::jmax(1, itemsViewport.getWidth() - 8),
        juce::jmax(
            itemsViewport.getHeight(),
            static_cast<int>(items().size()) * rowHeight));
    itemList->repaint();
}

void MixerPanel::resized()
{
    const auto listWidth = juce::jlimit(
        300,
        420,
        getWidth() / 3);
    itemsViewport.setBounds(
        getWidth() - listWidth,
        30,
        listWidth,
        juce::jmax(0, getHeight() - 38));
    refreshItems();
}

void MixerPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    if (project == nullptr || project->tracks.empty())
        return;

    constexpr auto stripWidth = 112;
    constexpr auto gap = 8;
    const auto itemListLeft = itemsViewport.getX();
    auto x = 14;

    for (const auto* track : mixerTracks())
    {
        if (x + stripWidth > itemListLeft - 36)
            break;
        const juce::Rectangle<int> strip(x, 34, stripWidth, getHeight() - 44);
        const auto selected = track->id == selectedTrack;
        graphics.setColour(juce::Colour(selected ? 0xff292e32 : StudioColours::raised));
        graphics.fillRoundedRectangle(strip.toFloat(), 5.0f);
        graphics.setColour(selected ? track->colour : juce::Colour(StudioColours::border));
        graphics.drawRoundedRectangle(strip.toFloat(), 5.0f, selected ? 1.5f : 1.0f);

        const auto meter = std::find_if(
            meters.cbegin(),
            meters.cend(),
            [track](const auto& value)
            {
                return value.trackId == track->id;
            });
        if (meter != meters.cend())
        {
            const auto pre = juce::jlimit(
                0.0f,
                1.0f,
                std::max(meter->preFaderLeft, meter->preFaderRight));
            const auto post = juce::jlimit(
                0.0f,
                1.0f,
                std::max(meter->postFaderLeft, meter->postFaderRight));
            graphics.setColour(juce::Colour(StudioColours::window));
            graphics.fillRect(strip.getX() + 4,
                              strip.getY() + 46,
                              3,
                              strip.getHeight() - 112);
            graphics.fillRect(strip.getRight() - 7,
                              strip.getY() + 46,
                              3,
                              strip.getHeight() - 112);
            graphics.setColour(juce::Colour(StudioColours::amber));
            graphics.fillRect(
                strip.getX() + 4,
                strip.getY() + 46
                    + static_cast<int>((1.0f - pre)
                                       * static_cast<float>(
                                           strip.getHeight() - 112)),
                3,
                static_cast<int>(pre
                                 * static_cast<float>(
                                     strip.getHeight() - 112)));
            graphics.setColour(juce::Colour(StudioColours::green));
            graphics.fillRect(
                strip.getRight() - 7,
                strip.getY() + 46
                    + static_cast<int>((1.0f - post)
                                       * static_cast<float>(
                                           strip.getHeight() - 112)),
                3,
                static_cast<int>(post
                                 * static_cast<float>(
                                     strip.getHeight() - 112)));
        }

        graphics.setColour(track->colour);
        graphics.fillRect(strip.getX(), strip.getY(), strip.getWidth(), 4);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(12.0f);
        graphics.drawFittedText(track->name,
                                strip.reduced(8).withHeight(24),
                                juce::Justification::centred,
                                1);

        const auto faderTop = strip.getY() + 46;
        const auto faderHeight = strip.getHeight() - 112;
        graphics.setColour(juce::Colour(StudioColours::window));
        graphics.fillRoundedRectangle(static_cast<float>(strip.getCentreX() - 3),
                                      static_cast<float>(faderTop),
                                      6.0f,
                                      static_cast<float>(faderHeight),
                                      3.0f);

        const auto volumeValue = track->id == draggingVolumeTrack
            ? dragPreviewVolume
            : track->volumeDecibels;
        const auto normalised = juce::jmap(juce::jlimit(-60.0f, 12.0f, volumeValue),
                                           -60.0f,
                                           12.0f,
                                           1.0f,
                                           0.0f);
        const auto knobY = faderTop
            + static_cast<int>(normalised * static_cast<float>(faderHeight));
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.fillRoundedRectangle(static_cast<float>(strip.getCentreX() - 12),
                                      static_cast<float>(knobY - 3),
                                      24.0f,
                                      7.0f,
                                      3.5f);

        juce::String state;
        if (track->type != TrackType::audio)
            state << trackTypeToString(track->type).toUpperCase() << " ";
        if (track->muted)
            state << "M ";
        if (track->solo)
            state << "S ";
        if (track->armed)
            state << "R";
        graphics.setColour(track->armed ? juce::Colour(StudioColours::orange)
                                        : juce::Colour(StudioColours::secondaryText));
        graphics.setFont(10.5f);
        graphics.drawText(state.trimEnd(),
                          strip.getX() + 8,
                          strip.getY() + 25,
                          strip.getWidth() - 16,
                          18,
                          juce::Justification::centred);

        const auto panValue = track->id == draggingPanTrack
            ? dragPreviewPan
            : track->pan;
        const auto panText = std::abs(panValue) < 0.005f
            ? juce::String("C")
            : juce::String(static_cast<int>(std::round(std::abs(panValue) * 100.0f)))
                + (panValue < 0.0f ? "% L" : "% R");
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
        graphics.drawText(juce::String(volumeValue, 1) + " dB",
                          strip.getX() + 6,
                          faderTop + faderHeight + 2,
                          strip.getWidth() - 12,
                          16,
                          juce::Justification::centred);

        const juce::Point<float> panCentre(static_cast<float>(strip.getCentreX()),
                                           static_cast<float>(strip.getBottom() - 35));
        constexpr auto panRadius = 12.0f;
        graphics.setColour(juce::Colour(StudioColours::window));
        graphics.fillEllipse(panCentre.x - panRadius,
                             panCentre.y - panRadius,
                             panRadius * 2.0f,
                             panRadius * 2.0f);
        graphics.setColour(std::abs(panValue) < 0.005f
                               ? juce::Colour(StudioColours::secondaryText)
                               : track->colour);
        graphics.drawEllipse(panCentre.x - panRadius,
                             panCentre.y - panRadius,
                             panRadius * 2.0f,
                             panRadius * 2.0f,
                             1.5f);
        const auto angle = juce::jmap(panValue,
                                     -1.0f,
                                     1.0f,
                                     -juce::MathConstants<float>::pi * 0.75f,
                                     juce::MathConstants<float>::pi * 0.75f);
        const juce::Point<float> marker(panCentre.x + std::sin(angle) * 8.0f,
                                        panCentre.y - std::cos(angle) * 8.0f);
        graphics.drawLine(panCentre.x, panCentre.y, marker.x, marker.y, 2.0f);
        graphics.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
        graphics.drawText("L",
                          static_cast<int>(panCentre.x - 30.0f),
                          static_cast<int>(panCentre.y - 8.0f),
                          12,
                          16,
                          juce::Justification::centred);
        graphics.drawText("R",
                          static_cast<int>(panCentre.x + 18.0f),
                          static_cast<int>(panCentre.y - 8.0f),
                          12,
                          16,
                          juce::Justification::centred);
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.drawText(panText,
                          strip.getX() + 6,
                          strip.getBottom() - 18,
                          strip.getWidth() - 12,
                          14,
                          juce::Justification::centred);

        x += stripWidth + gap;
    }

    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawVerticalLine(
        itemListLeft - 8,
        0.0f,
        static_cast<float>(getHeight()));
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(
        juce::FontOptions(10.0f, juce::Font::bold)));
    graphics.drawText(
        "INSERTS & SENDS",
        itemListLeft,
        4,
        itemsViewport.getWidth(),
        20,
        juce::Justification::centredLeft);

    const auto meterX = itemListLeft - 30;
    const auto meterHeight = getHeight() - 58;
    graphics.setColour(juce::Colour(StudioColours::window));
    graphics.fillRoundedRectangle(static_cast<float>(meterX),
                                  38.0f,
                                  7.0f,
                                  static_cast<float>(meterHeight),
                                  3.5f);
    graphics.fillRoundedRectangle(static_cast<float>(meterX + 11),
                                  38.0f,
                                  7.0f,
                                  static_cast<float>(meterHeight),
                                  3.5f);
    graphics.setColour(juce::Colour(StudioColours::green));
    graphics.fillRect(meterX,
                      38 + static_cast<int>((1.0f - leftPeak)
                                            * static_cast<float>(meterHeight)),
                      7,
                      static_cast<int>(leftPeak * static_cast<float>(meterHeight)));
    graphics.fillRect(meterX + 11,
                      38 + static_cast<int>((1.0f - rightPeak)
                                            * static_cast<float>(meterHeight)),
                      7,
                      static_cast<int>(rightPeak * static_cast<float>(meterHeight)));
}

void MixerPanel::mouseDown(const juce::MouseEvent& event)
{
    if (project == nullptr || event.position.y < 34.0f)
        return;

    constexpr auto stripWidth = 112;
    constexpr auto gap = 8;
    const auto index = static_cast<int>((event.position.x - 14.0f) / (stripWidth + gap));
    const auto tracks = mixerTracks();
    if (index < 0 || index >= static_cast<int>(tracks.size()))
        return;

    const auto* track = tracks[static_cast<std::size_t>(index)];
    if (onTrackSelected)
        onTrackSelected(track->id);

    const juce::Rectangle<int> strip(14 + index * (stripWidth + gap),
                                     34,
                                     stripWidth,
                                     getHeight() - 44);
    draggingVolumeTrack.clear();
    draggingPanTrack.clear();
    if (event.position.y >= static_cast<float>(strip.getBottom() - 60))
    {
        draggingPanTrack = track->id;
        dragStartY = event.position.y;
        dragStartPan = track->pan;
        dragPreviewPan = track->pan;
        return;
    }

    const auto faderTop = strip.getY() + 46;
    const auto faderHeight = strip.getHeight() - 112;
    if (event.position.y >= static_cast<float>(faderTop - 8)
        && event.position.y <= static_cast<float>(faderTop + faderHeight + 8))
    {
        draggingVolumeTrack = track->id;
        dragStartY = event.position.y;
        dragStartVolume = track->volumeDecibels;
        dragPreviewVolume = track->volumeDecibels;
        dragFaderHeight = std::max(1, faderHeight);
    }
}

void MixerPanel::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingVolumeTrack.isNotEmpty())
    {
        dragPreviewVolume = juce::jlimit(
            -60.0f,
            12.0f,
            dragStartVolume
                + (dragStartY - event.position.y)
                    * 72.0f
                    / static_cast<float>(dragFaderHeight));
        repaint();
        return;
    }
    if (draggingPanTrack.isEmpty())
        return;

    dragPreviewPan = juce::jlimit(
        -1.0f,
        1.0f,
        dragStartPan + (dragStartY - event.position.y) / 80.0f);
    repaint();
}

void MixerPanel::mouseUp(const juce::MouseEvent&)
{
    if (draggingVolumeTrack.isNotEmpty())
    {
        const auto trackId = draggingVolumeTrack;
        const auto value = dragPreviewVolume;
        draggingVolumeTrack.clear();
        if (onVolumeChanged)
            onVolumeChanged(trackId, value);
        repaint();
        return;
    }
    if (draggingPanTrack.isNotEmpty())
    {
        const auto trackId = draggingPanTrack;
        const auto value = dragPreviewPan;
        draggingPanTrack.clear();
        if (onPanChanged)
            onPanChanged(trackId, value);
        repaint();
    }
}

void MixerPanel::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (project == nullptr || event.position.y < 34.0f)
        return;

    constexpr auto stripWidth = 112;
    constexpr auto gap = 8;
    const auto index = static_cast<int>((event.position.x - 14.0f) / (stripWidth + gap));
    const auto tracks = mixerTracks();
    if (index < 0 || index >= static_cast<int>(tracks.size()))
        return;

    const juce::Rectangle<int> strip(14 + index * (stripWidth + gap),
                                     34,
                                     stripWidth,
                                     getHeight() - 44);
    const auto* track = tracks[static_cast<std::size_t>(index)];
    const auto titleBounds = strip.withHeight(42);
    if (titleBounds.contains(event.getPosition()))
    {
        if (onEditTrack)
        {
            const auto nameBounds = strip.reduced(8).withHeight(28);
            onEditTrack(track->id, localAreaToGlobal(nameBounds));
        }
        return;
    }

    if (event.position.y >= static_cast<float>(strip.getBottom() - 60)
        && onPanChanged)
    {
        onPanChanged(track->id, 0.0f);
        return;
    }

    const auto faderTop = strip.getY() + 46;
    const auto faderHeight = strip.getHeight() - 112;
    if (event.position.y >= static_cast<float>(faderTop - 8)
        && event.position.y <= static_cast<float>(faderTop + faderHeight + 8)
        && onVolumeChanged)
        onVolumeChanged(track->id, 0.0f);
}
}
