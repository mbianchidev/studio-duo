#include "MixerPanel.h"

#include "StudioIconButton.h"
#include "NumericInput.h"
#include "StudioPanControl.h"
#include "StudioTheme.h"

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
constexpr int stripWidth = 136;
constexpr int stripGap = 8;
constexpr int stripTop = 34;
constexpr int stripControlY = 48;
constexpr int stripControlSize = 22;
constexpr int stripProcessingTop = 100;
constexpr int processingHeaderHeight = 24;
constexpr int processingRowHeight = 28;
constexpr int visibleProcessingRows = 2;
constexpr int stripFaderTop =
    stripProcessingTop
    + 2 * (processingHeaderHeight
           + visibleProcessingRows * processingRowHeight)
    + 10;
constexpr int stripBottomControls = 54;
constexpr int processingSectionHeight =
    processingHeaderHeight
    + visibleProcessingRows * processingRowHeight;
}

MixerPanel::MixerPanel()
{
    addChildComponent(volumeEditor);
    volumeEditor.setJustification(
        juce::Justification::centred);
    volumeEditor.setSelectAllWhenFocused(true);
    volumeEditor.setInputRestrictions(
        12,
        "0123456789+-. dDbB");
    volumeEditor.setColour(
        juce::TextEditor::backgroundColourId,
        juce::Colour(StudioColours::window));
    volumeEditor.setColour(
        juce::TextEditor::textColourId,
        juce::Colour(StudioColours::text));
    volumeEditor.setColour(
        juce::TextEditor::outlineColourId,
        juce::Colour(StudioColours::orange));
    volumeEditor.onReturnKey = [this]
    {
        commitVolumeEdit();
    };
    volumeEditor.onEscapeKey = [this]
    {
        cancelVolumeEdit();
    };
    volumeEditor.onFocusLost = [this]
    {
        if (!committingVolumeEdit)
            commitVolumeEdit();
    };
}

MixerPanel::~MixerPanel()
{
}

void MixerPanel::setProject(const Project* value)
{
    if (volumeEditor.isVisible())
        cancelVolumeEdit();
    project = value;
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

std::vector<MixerPanel::Item> MixerPanel::itemsForTrack(
    const Track& track) const
{
    std::vector<Item> result;
    if (project == nullptr)
        return result;
    for (const auto& insert : track.inserts)
    {
        result.push_back({
            Item::Type::plugin,
            track.id,
            insert.id,
            insert.name,
            insert.format,
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
            route.name,
            routeKindToString(route.kind).toUpperCase(),
            route.enabled && !route.muted
        });
    }
    return result;
}

void MixerPanel::resized()
{
}

void MixerPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    if (project == nullptr || project->tracks.empty())
        return;

    auto x = 14;

    for (const auto* track : mixerTracks())
    {
        if (x + stripWidth > getWidth() - 42)
            break;
        const juce::Rectangle<int> strip(
            x,
            stripTop,
            stripWidth,
            getHeight() - 44);
        const auto faderTop =
            strip.getY() + stripFaderTop;
        const auto faderHeight = juce::jmax(
            24,
            strip.getHeight()
                - stripFaderTop
                - stripBottomControls);
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
                              faderTop,
                              6,
                              faderHeight);
            graphics.fillRect(strip.getRight() - 10,
                              faderTop,
                              6,
                              faderHeight);
            graphics.setColour(juce::Colour(StudioColours::amber));
            graphics.fillRect(
                strip.getX() + 4,
                faderTop
                    + static_cast<int>((1.0f - pre)
                                       * static_cast<float>(
                                           faderHeight)),
                6,
                static_cast<int>(pre
                                 * static_cast<float>(
                                     faderHeight)));
            graphics.setColour(
                post > 0.9f
                    ? juce::Colour(StudioColours::orange)
                    : post > 0.7f
                        ? juce::Colour(StudioColours::amber)
                        : juce::Colour(StudioColours::green));
            graphics.fillRect(
                strip.getRight() - 10,
                faderTop
                    + static_cast<int>((1.0f - post)
                                       * static_cast<float>(
                                           faderHeight)),
                6,
                static_cast<int>(post
                                 * static_cast<float>(
                                     faderHeight)));
        }

        graphics.setColour(track->colour);
        graphics.fillRect(strip.getX(), strip.getY(), strip.getWidth(), 4);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(12.0f);
        graphics.drawFittedText(track->name,
                                strip.reduced(8).withHeight(24),
                                juce::Justification::centred,
                                1);

        if (track->type == TrackType::audio)
        {
            const juce::Rectangle<float> inputBounds(
                static_cast<float>(strip.getX() + 12),
                static_cast<float>(strip.getY() + 27),
                static_cast<float>(strip.getWidth() - 24),
                18.0f);
            graphics.setColour(
                juce::Colour(StudioColours::window));
            graphics.fillRoundedRectangle(
                inputBounds,
                3.0f);
            graphics.setColour(
                juce::Colour(StudioColours::border));
            graphics.drawRoundedRectangle(
                inputBounds,
                3.0f,
                1.0f);
            graphics.setColour(
                juce::Colour(StudioColours::text));
            graphics.setFont(
                juce::Font(juce::FontOptions(8.5f)));
            const auto inputText =
                "Input "
                + juce::String(track->inputChannel + 1)
                + (track->stereoInput
                       ? "-"
                           + juce::String(
                               track->inputChannel + 2)
                       : juce::String());
            graphics.drawText(
                inputText,
                inputBounds.toNearestInt()
                    .withTrimmedRight(14),
                juce::Justification::centred);
            juce::Path chevron;
            chevron.startNewSubPath(
                inputBounds.getRight() - 11.0f,
                inputBounds.getCentreY() - 2.0f);
            chevron.lineTo(
                inputBounds.getRight() - 7.0f,
                inputBounds.getCentreY() + 2.0f);
            chevron.lineTo(
                inputBounds.getRight() - 3.0f,
                inputBounds.getCentreY() - 2.0f);
            graphics.strokePath(
                chevron,
                juce::PathStrokeType(1.2f));
        }

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

        const auto drawControl = [&graphics, &strip](
                                     int offset,
                                     StudioIcon icon,
                                     bool active,
                                     juce::Colour activeColour)
        {
            const juce::Rectangle<float> bounds(
                static_cast<float>(strip.getX() + offset),
                static_cast<float>(
                    strip.getY() + stripControlY),
                static_cast<float>(stripControlSize),
                static_cast<float>(stripControlSize));
            graphics.setColour(
                active ? activeColour
                       : juce::Colour(StudioColours::window));
            graphics.fillRoundedRectangle(bounds, 4.0f);
            drawStudioIcon(
                graphics,
                icon,
                bounds.reduced(5.0f),
                active ? juce::Colours::white
                       : juce::Colour(
                           StudioColours::secondaryText),
                1.3f);
        };
        drawControl(
            12,
            StudioIcon::mute,
            track->muted,
            juce::Colour(StudioColours::amber));
        drawControl(
            45,
            StudioIcon::solo,
            track->solo,
            juce::Colour(StudioColours::green));
        if (track->type == TrackType::audio
            || track->type == TrackType::instrument
            || track->type == TrackType::midi)
        {
            drawControl(
                78,
                StudioIcon::record,
                track->armed,
                juce::Colour(StudioColours::orange));
        }

        const juce::Rectangle<float> decibelBounds(
            static_cast<float>(strip.getX() + 12),
            static_cast<float>(strip.getY() + 74),
            static_cast<float>(strip.getWidth() - 24),
            20.0f);
        graphics.setColour(juce::Colour(StudioColours::window));
        graphics.fillRoundedRectangle(decibelBounds, 3.0f);
        graphics.setColour(juce::Colour(StudioColours::border));
        graphics.drawRoundedRectangle(decibelBounds, 3.0f, 1.0f);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(
            juce::Font(juce::FontOptions(10.0f,
                                         juce::Font::bold)));
        graphics.drawText(
            formatDecibels(volumeValue),
            decibelBounds.toNearestInt(),
            juce::Justification::centred);

        const auto values = itemsForTrack(*track);
        const auto drawProcessingSection =
            [&graphics, &strip, &values](
                Item::Type type,
                const juce::String& title,
                int sectionTop)
        {
            const auto count = static_cast<int>(
                std::count_if(
                    values.cbegin(),
                    values.cend(),
                    [type](const auto& item)
                    {
                        return item.type == type;
                    }));
            auto header = juce::Rectangle<int>(
                strip.getX() + 4,
                sectionTop,
                strip.getWidth() - 8,
                processingHeaderHeight - 2);
            graphics.setColour(
                juce::Colour(
                    StudioColours::transportRaised));
            graphics.fillRoundedRectangle(
                header.toFloat(),
                3.0f);
            graphics.setColour(
                juce::Colour(StudioColours::text));
            graphics.setFont(
                juce::Font(
                    juce::FontOptions(
                        9.0f,
                        juce::Font::bold)));
            graphics.drawText(
                title
                    + (count > 0
                           ? " " + juce::String(count)
                           : juce::String()),
                header.withTrimmedLeft(6)
                    .withTrimmedRight(28),
                juce::Justification::centredLeft);
            graphics.setFont(
                juce::Font(
                    juce::FontOptions(
                        15.0f,
                        juce::Font::bold)));
            graphics.drawText(
                "+",
                header.removeFromRight(26),
                juce::Justification::centred);

            auto rowY =
                sectionTop + processingHeaderHeight;
            auto drawn = 0;
            for (const auto& item : values)
            {
                if (item.type != type
                    || drawn >= visibleProcessingRows)
                    continue;
                const auto row = juce::Rectangle<int>(
                    strip.getX() + 4,
                    rowY,
                    strip.getWidth() - 8,
                    processingRowHeight - 2);
                graphics.setColour(
                    juce::Colour(StudioColours::window));
                graphics.fillRoundedRectangle(
                    row.toFloat(),
                    3.0f);
                graphics.setColour(
                    juce::Colour(StudioColours::border));
                graphics.drawRoundedRectangle(
                    row.toFloat(),
                    3.0f,
                    1.0f);
                graphics.setColour(
                    juce::Colour(StudioColours::text));
                graphics.setFont(
                    juce::Font(
                        juce::FontOptions(8.5f)));
                graphics.drawFittedText(
                    item.title,
                    row.withTrimmedLeft(6)
                        .withTrimmedRight(28),
                    juce::Justification::centredLeft,
                    1);
                const auto toggle =
                    row.withLeft(row.getRight() - 26)
                        .reduced(3);
                graphics.setColour(
                    juce::Colour(
                        item.enabled
                            ? StudioColours::green
                            : StudioColours::amber));
                graphics.fillRoundedRectangle(
                    toggle.toFloat(),
                    3.0f);
                drawStudioIcon(
                    graphics,
                    StudioIcon::power,
                    toggle.toFloat().reduced(5.0f),
                    juce::Colours::white,
                    1.2f);
                rowY += processingRowHeight;
                ++drawn;
            }
            if (drawn == 0)
            {
                graphics.setColour(
                    juce::Colour(
                        StudioColours::secondaryText));
                graphics.setFont(
                    juce::Font(
                        juce::FontOptions(8.0f)));
                graphics.drawText(
                    type == Item::Type::plugin
                        ? "No inserts"
                        : "No sends",
                    strip.getX() + 10,
                    rowY,
                    strip.getWidth() - 20,
                    processingRowHeight,
                    juce::Justification::centredLeft);
            }
        };
        const auto insertTop =
            strip.getY() + stripProcessingTop;
        drawProcessingSection(
            Item::Type::plugin,
            "INSERTS",
            insertTop);
        drawProcessingSection(
            Item::Type::route,
            "SENDS",
            insertTop + processingSectionHeight);

        graphics.setColour(
            juce::Colour(StudioColours::secondaryText));
        graphics.setFont(juce::Font(juce::FontOptions(7.5f)));
        for (const auto tick : { 12, 0, -12,
                                 -24, -48, -60 })
        {
            const auto tickNormalised = juce::jmap(
                static_cast<float>(tick),
                -60.0f,
                12.0f,
                1.0f,
                0.0f);
            const auto tickY = faderTop
                + static_cast<int>(
                    tickNormalised
                    * static_cast<float>(faderHeight));
            graphics.drawHorizontalLine(
                tickY,
                static_cast<float>(
                    strip.getCentreX() + 9),
                static_cast<float>(
                    strip.getCentreX() + 13));
            if (tick == 0 || tick == -24
                || tick == -60)
            {
                graphics.drawText(
                    juce::String(tick),
                    strip.getCentreX() + 15,
                    tickY - 6,
                    24,
                    12,
                    juce::Justification::centredLeft);
            }
        }
        if (meter != meters.cend())
        {
            const auto peak = juce::jlimit(
                0.0f,
                1.0f,
                std::max(
                    meter->postFaderLeft,
                    meter->postFaderRight));
            const auto peakDb = juce::Decibels::gainToDecibels(
                peak,
                -60.0f);
            graphics.setColour(
                juce::Colour(StudioColours::secondaryText));
            graphics.setFont(
                juce::Font(juce::FontOptions(8.0f)));
            graphics.drawText(
                formatDecibels(peakDb),
                strip.getX() + 12,
                faderTop + faderHeight + 2,
                strip.getWidth() - 24,
                12,
                juce::Justification::centred);
        }

        const auto panValue = track->id == draggingPanTrack
            ? dragPreviewPan
            : track->pan;
        const auto panText = std::abs(panValue) < 0.005f
            ? juce::String("C")
            : juce::String(static_cast<int>(std::round(std::abs(panValue) * 100.0f)))
                + (panValue < 0.0f ? "% L" : "% R");
        drawStudioPanControl(
            graphics,
            {
                static_cast<float>(strip.getX() + 2),
                static_cast<float>(strip.getBottom() - 45),
                static_cast<float>(strip.getWidth() - 4),
                24.0f
            },
            panValue,
            track->colour,
            true);
        graphics.setColour(
            juce::Colour(StudioColours::secondaryText));
        graphics.setFont(
            juce::Font(juce::FontOptions(8.0f)));
        graphics.drawText(panText,
                          strip.getX() + 6,
                          strip.getBottom() - 16,
                          strip.getWidth() - 12,
                          14,
                          juce::Justification::centred);

        x += stripWidth + stripGap;
    }

    const auto meterX = getWidth() - 28;
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
    if (event.mods.isPopupMenu())
    {
        showTrackContextMenu(event);
        return;
    }

    const auto index = static_cast<int>(
        (event.position.x - 14.0f)
        / (stripWidth + stripGap));
    const auto tracks = mixerTracks();
    if (index < 0 || index >= static_cast<int>(tracks.size()))
        return;

    const auto* track = tracks[static_cast<std::size_t>(index)];
    if (onTrackSelected)
        onTrackSelected(track->id);
    const juce::Rectangle<int> strip(
                                     14 + index * (stripWidth + stripGap),
                                     stripTop,
                                     stripWidth,
                                     getHeight() - 44);
    const auto localX =
        static_cast<int>(event.position.x)
        - strip.getX();
    const auto localY =
        static_cast<int>(event.position.y)
        - strip.getY();
    const auto inputBounds = juce::Rectangle<int>(
        strip.getX() + 12,
        strip.getY() + 27,
        strip.getWidth() - 24,
        18);
    if (track->type == TrackType::audio
        && inputBounds.contains(event.getPosition())
        && onInputMenuRequested)
    {
        onInputMenuRequested(
            track->id,
            localAreaToGlobal(inputBounds));
        return;
    }
    const auto decibelBounds = juce::Rectangle<int>(
        strip.getX() + 12,
        strip.getY() + 74,
        strip.getWidth() - 24,
        20);
    if (decibelBounds.contains(event.getPosition())
        && track->type != TrackType::folder
        && track->type != TrackType::midi)
    {
        beginVolumeEdit(*track, decibelBounds);
        return;
    }
    if (localY >= stripControlY
        && localY
            < stripControlY + stripControlSize)
    {
        if (localX >= 12
            && localX < 12 + stripControlSize
            && onTrackMute)
            onTrackMute(track->id);
        else if (localX >= 45
                 && localX < 45 + stripControlSize
                 && onTrackSolo)
            onTrackSolo(track->id);
        else if (localX >= 78
                 && localX < 78 + stripControlSize
                 && (track->type == TrackType::audio
                     || track->type
                         == TrackType::instrument
                     || track->type == TrackType::midi)
                 && onTrackArm)
            onTrackArm(track->id);
        return;
    }

    const auto values = itemsForTrack(*track);
    const auto handleProcessingSection =
        [this, &event, &strip, &values, track](
            Item::Type type,
            int sectionTop) -> bool
    {
        const auto header = juce::Rectangle<int>(
            strip.getX() + 4,
            sectionTop,
            strip.getWidth() - 8,
            processingHeaderHeight - 2);
        if (header.contains(event.getPosition()))
        {
            const auto addBounds =
                header.withLeft(header.getRight() - 30);
            if (!addBounds.contains(event.getPosition()))
                return true;
            if (type == Item::Type::plugin)
            {
                if (onAddInsert)
                    onAddInsert(
                        track->id,
                        localAreaToGlobal(addBounds));
            }
            else if (onAddSend)
            {
                onAddSend(
                    track->id,
                    localAreaToGlobal(addBounds));
            }
            return true;
        }

        auto rowY = sectionTop + processingHeaderHeight;
        auto drawn = 0;
        for (const auto& item : values)
        {
            if (item.type != type
                || drawn >= visibleProcessingRows)
                continue;
            const auto row = juce::Rectangle<int>(
                strip.getX() + 4,
                rowY,
                strip.getWidth() - 8,
                processingRowHeight - 2);
            if (row.contains(event.getPosition()))
            {
                const auto power = row.withLeft(
                    row.getRight() - 28);
                if (power.contains(event.getPosition()))
                {
                    if (type == Item::Type::plugin
                        && onPluginEnabledChanged)
                    {
                        onPluginEnabledChanged(
                            item.trackId,
                            item.objectId,
                            !item.enabled);
                    }
                    else if (type == Item::Type::route
                             && onRouteEnabledChanged)
                    {
                        onRouteEnabledChanged(
                            item.trackId,
                            item.objectId,
                            !item.enabled);
                    }
                }
                else if (type == Item::Type::plugin
                         && onPluginOpen)
                {
                    onPluginOpen(
                        item.trackId,
                        item.objectId);
                }
                else if (type == Item::Type::route
                         && onRouteOpen)
                {
                    onRouteOpen(
                        item.trackId,
                        item.objectId);
                }
                return true;
            }
            rowY += processingRowHeight;
            ++drawn;
        }
        return false;
    };
    const auto insertTop =
        strip.getY() + stripProcessingTop;
    if (handleProcessingSection(
            Item::Type::plugin,
            insertTop)
        || handleProcessingSection(
            Item::Type::route,
            insertTop + processingSectionHeight))
        return;

    if (track->type == TrackType::folder
        || track->type == TrackType::midi)
        return;

    draggingVolumeTrack.clear();
    draggingPanTrack.clear();
    if (track->type != TrackType::vca
        && event.position.y
            >= static_cast<float>(strip.getBottom() - 60))
    {
        draggingPanTrack = track->id;
        dragStartX = event.position.x;
        dragStartY = event.position.y;
        dragStartPan = track->pan;
        dragPreviewPan = track->pan;
        if (onAutomationGestureStarted)
            onAutomationGestureStarted(
                track->id,
                AutomationTargetType::trackPan,
                (track->pan + 1.0f) * 0.5f);
        return;
    }

    const auto faderTop =
        strip.getY() + stripFaderTop;
    const auto faderHeight = juce::jmax(
        24,
        strip.getHeight()
            - stripFaderTop
            - stripBottomControls);
    if (event.position.y >= static_cast<float>(faderTop - 8)
        && event.position.y <= static_cast<float>(faderTop + faderHeight + 8))
    {
        draggingVolumeTrack = track->id;
        dragStartY = event.position.y;
        dragStartVolume = track->volumeDecibels;
        dragPreviewVolume = track->volumeDecibels;
        dragFaderHeight = std::max(1, faderHeight);
        if (onAutomationGestureStarted)
            onAutomationGestureStarted(
                track->id,
                AutomationTargetType::trackVolume,
                (track->volumeDecibels + 60.0f) / 72.0f);
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
        dragStartPan
            + (event.position.x - dragStartX) / 56.0f);
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

void MixerPanel::beginVolumeEdit(
    const Track& track,
    juce::Rectangle<int> bounds)
{
    editingVolumeTrack = track.id;
    volumeEditor.setColour(
        juce::TextEditor::outlineColourId,
        juce::Colour(StudioColours::orange));
    volumeEditor.setText(
        formatDecibels(track.volumeDecibels),
        false);
    volumeEditor.setBounds(bounds);
    volumeEditor.setVisible(true);
    volumeEditor.toFront(true);
    volumeEditor.grabKeyboardFocus();
    volumeEditor.selectAll();
}

void MixerPanel::commitVolumeEdit()
{
    if (!volumeEditor.isVisible()
        || committingVolumeEdit)
        return;
    const auto parsed = parseTrackDecibels(
        volumeEditor.getText());
    if (!parsed.has_value())
    {
        volumeEditor.setColour(
            juce::TextEditor::outlineColourId,
            juce::Colour(StudioColours::orange));
        volumeEditor.setTooltip(
            "Enter a finite value from -60 to +12 dB.");
        const auto safe =
            juce::Component::SafePointer<MixerPanel>(this);
        juce::MessageManager::callAsync([safe]
        {
            if (safe != nullptr
                && safe->volumeEditor.isVisible())
            {
                safe->volumeEditor.grabKeyboardFocus();
                safe->volumeEditor.selectAll();
            }
        });
        return;
    }

    committingVolumeEdit = true;
    const auto trackId = editingVolumeTrack;
    const auto* track = project != nullptr
        ? project->findTrack(trackId)
        : nullptr;
    if (track != nullptr
        && onAutomationGestureStarted)
    {
        onAutomationGestureStarted(
            trackId,
            AutomationTargetType::trackVolume,
            (track->volumeDecibels + 60.0f)
                / 72.0f);
    }
    volumeEditor.setVisible(false);
    editingVolumeTrack.clear();
    volumeEditor.setTooltip({});
    if (onVolumeChanged)
        onVolumeChanged(trackId, *parsed);
    committingVolumeEdit = false;
}

void MixerPanel::cancelVolumeEdit()
{
    committingVolumeEdit = true;
    volumeEditor.setVisible(false);
    volumeEditor.setTooltip({});
    editingVolumeTrack.clear();
    committingVolumeEdit = false;
}

void MixerPanel::showTrackContextMenu(
    const juce::MouseEvent& event)
{
    if (project == nullptr)
        return;
    const auto index = static_cast<int>(
        (event.position.x - 14.0f)
        / (stripWidth + stripGap));
    const auto tracks = mixerTracks();
    if (index < 0
        || index >= static_cast<int>(tracks.size()))
        return;

    const auto* track =
        tracks[static_cast<std::size_t>(index)];
    const auto trackId = track->id;
    if (onTrackSelected)
        onTrackSelected(trackId);

    juce::PopupMenu menu;
    menu.addItem(
        track->muted ? "Unmute track" : "Mute track",
        [this, trackId]
        {
            if (onTrackMute)
                onTrackMute(trackId);
        });
    menu.addItem(
        track->solo ? "Unsolo track" : "Solo track",
        [this, trackId]
        {
            if (onTrackSolo)
                onTrackSolo(trackId);
        });
    if (track->type == TrackType::audio
        || track->type == TrackType::instrument
        || track->type == TrackType::midi)
    {
        menu.addItem(
            track->armed ? "Disarm track" : "Arm track",
            [this, trackId]
            {
                if (onTrackArm)
                    onTrackArm(trackId);
            });
    }
    menu.addItem(
        "Edit name and color...",
        [this, trackId, index]
        {
            if (onEditTrack)
            {
                const juce::Rectangle<int> strip(
                    14 + index
                        * (stripWidth + stripGap),
                    stripTop,
                    stripWidth,
                    getHeight() - 44);
                onEditTrack(
                    trackId,
                    localAreaToGlobal(
                        strip.reduced(8)
                            .withHeight(28)));
            }
        });

    const auto hasVersions = std::any_of(
        project->tracks.cbegin(),
        project->tracks.cend(),
        [track](const auto& candidate)
        {
            return candidate.parentTrackId == track->id;
        });
    if (hasVersions)
    {
        menu.addItem(
            track->versionsCollapsed
                ? "Expand versions"
                : "Collapse versions",
            [this, trackId]
            {
                if (onToggleTrackVersions)
                    onToggleTrackVersions(trackId);
            });
    }

    menu.addSeparator();
    menu.addItem(
        "Duplicate track",
        track->type != TrackType::master,
        false,
        [this, trackId]
        {
            if (onDuplicateTrack)
                onDuplicateTrack(trackId);
        });
    menu.addItem(
        "Delete track",
        track->type != TrackType::master,
        false,
        [this, trackId]
        {
            if (onDeleteTrack)
                onDeleteTrack(trackId);
        });

    const auto screenPosition = event.getScreenPosition();
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(this)
            .withTargetScreenArea({
                screenPosition.x,
                screenPosition.y,
                1,
                1
            }));
}

void MixerPanel::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (project == nullptr || event.position.y < 34.0f)
        return;

    const auto index = static_cast<int>(
        (event.position.x - 14.0f)
        / (stripWidth + stripGap));
    const auto tracks = mixerTracks();
    if (index < 0 || index >= static_cast<int>(tracks.size()))
        return;

    const juce::Rectangle<int> strip(
                                     14 + index * (stripWidth + stripGap),
                                     stripTop,
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

    if (track->type != TrackType::vca
        && track->type != TrackType::folder
        && track->type != TrackType::midi
        && event.position.y
            >= static_cast<float>(strip.getBottom() - 60)
        && onPanChanged)
    {
        onPanChanged(track->id, 0.0f);
        return;
    }

    const auto faderTop =
        strip.getY() + stripFaderTop;
    const auto faderHeight = juce::jmax(
        24,
        strip.getHeight()
            - stripFaderTop
            - stripBottomControls);
    if (track->type != TrackType::folder
        && track->type != TrackType::midi
        && event.position.y >= static_cast<float>(faderTop - 8)
        && event.position.y <= static_cast<float>(faderTop + faderHeight + 8)
        && onVolumeChanged)
        onVolumeChanged(track->id, 0.0f);
}
}
