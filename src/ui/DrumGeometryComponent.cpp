#include "DrumGeometryComponent.h"

#include "StudioTheme.h"
#include "logging/StudioLogger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace studio
{
DrumGeometryComponent::DrumGeometryComponent()
{
    setComponentID("drum-geometry");
    setTitle("Drum rhythm geometry");
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::focusContainer);
    setOpaque(true);
    addAndMakeVisible(legend);
    legend.setModel(this);
    legend.setComponentID("geometry-legend");
    legend.setTitle("Drum rhythm rings");
    legend.setDescription(
        "Select a drum voice to highlight its rhythm. "
        "Use Up and Down to navigate; Escape shows all voices.");
    legend.setRowHeight(36);
    legend.setMultipleSelectionEnabled(false);
    legend.setColour(
        juce::ListBox::backgroundColourId, juce::Colour(StudioColours::panel));
    legend.setColour(
        juce::ListBox::outlineColourId, juce::Colour(StudioColours::border));
    legend.setOutlineThickness(1);
    addAndMakeVisible(summaryLabel);
    summaryLabel.setFont(juce::Font(juce::FontOptions(
        juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)));
    addAndMakeVisible(hintLabel);
    hintLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    hintLabel.setJustificationType(juce::Justification::centredLeft);
    setContext(nullptr, nullptr, 4.0);
}

void DrumGeometryComponent::setContext(
    const MidiClip* clip, const DrumMap* map, double beatsPerBar)
{
    const auto previousPitch = juce::isPositiveAndBelow(selectedRing, rhythmRings.size())
        ? rhythmRings[static_cast<std::size_t>(selectedRing)].pitch : -1;
    rhythmRings.clear();
    selectedRing = -1;
    durationBeats = 0.0;
    clipStartBeats = 0.0;
    if (clip == nullptr)
    {
        setDescription("Select a MIDI clip to visualize its drum rhythm.");
        summaryLabel.setText("No MIDI clip selected", juce::dontSendNotification);
    }
    else if (!std::isfinite(clip->startBeats) || clip->startBeats < 0.0
             || !std::isfinite(clip->durationBeats) || clip->durationBeats <= 0.0
             || !std::isfinite(beatsPerBar) || beatsPerBar <= 0.0)
    {
        setDescription("The MIDI clip has an invalid start, duration, or time signature.");
        summaryLabel.setText("Cannot visualize this MIDI clip", juce::dontSendNotification);
        logError("Drum geometry", getDescription());
    }
    else
    {
        durationBeats = clip->durationBeats;
        clipStartBeats = clip->startBeats;
        std::map<int, Ring> grouped;
        for (const auto& note : clip->notes)
        {
            const auto* entry = map != nullptr
                ? map->entryForPitch(note.pitch) : nullptr;
            const auto pitch = entry != nullptr ? entry->noteNumber : note.pitch;
            auto [iterator, inserted] = grouped.try_emplace(pitch);
            auto& ring = iterator->second;
            if (inserted)
            {
                ring.pitch = pitch;
                ring.name = entry != nullptr
                    ? entry->name : "MIDI " + juce::String(pitch);
                ring.articulation = entry != nullptr ? entry->articulation : juce::String();
                ring.outputGroup = entry != nullptr ? entry->outputGroup : juce::String();
            }
            ring.hits.push_back({
                note.id, note.pitch, note.actualStartBeats(),
                note.actualStartBeats() / durationBeats,
                note.velocity, note.probability
            });
        }
        for (auto& [pitch, ring] : grouped)
        {
            juce::ignoreUnused(pitch);
            std::stable_sort(
                ring.hits.begin(), ring.hits.end(),
                [](const auto& left, const auto& right)
                {
                    return left.beat < right.beat;
                });
            rhythmRings.push_back(std::move(ring));
        }
        if (map != nullptr)
        {
            const auto order = [map](int pitch)
            {
                const auto entry = std::find_if(
                    map->entries.cbegin(), map->entries.cend(),
                    [pitch](const auto& candidate)
                    {
                        return candidate.noteNumber == pitch;
                    });
                return entry != map->entries.cend()
                    ? static_cast<int>(std::distance(map->entries.cbegin(), entry))
                    : static_cast<int>(map->entries.size()) + pitch;
            };
            std::stable_sort(
                rhythmRings.begin(), rhythmRings.end(),
                [&](const auto& left, const auto& right)
                {
                    return order(left.pitch) < order(right.pitch);
                });
        }
        summaryLabel.setText(
            clip->name + "  |  " + juce::String(durationBeats, 2) + " beats / "
                + juce::String(durationBeats / beatsPerBar, 2) + " bars",
            juce::dontSendNotification);
        setDescription(
            "Drum rhythm geometry: " + juce::String(static_cast<int>(clip->notes.size()))
                + " hits across " + juce::String(static_cast<int>(rhythmRings.size()))
                + " voices. One clockwise turn shows the whole clip; dot size shows velocity.");
    }
    legend.updateContent();
    legend.deselectAllRows();
    for (std::size_t index = 0; index < rhythmRings.size(); ++index)
        if (rhythmRings[index].pitch == previousPitch)
            legend.selectRow(static_cast<int>(index));
    refreshHint();
    repaint();
}

void DrumGeometryComponent::setTransportPosition(double positionBeats, bool playing)
{
    if (std::abs(transportBeats - positionBeats) < 0.0000001
        && transportPlaying == playing)
        return;
    transportBeats = positionBeats;
    transportPlaying = playing;
    if (!std::isfinite(positionBeats))
        logError("Drum geometry", "The transport position is not finite.");
    if (isShowing())
        repaint();
}

std::optional<double> DrumGeometryComponent::playheadPhase() const noexcept
{
    if (durationBeats <= 0.0 || !std::isfinite(transportBeats)
        || transportBeats < clipStartBeats
        || transportBeats >= clipStartBeats + durationBeats)
        return std::nullopt;
    return (transportBeats - clipStartBeats) / durationBeats;
}

void DrumGeometryComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    const auto wheel = wheelBounds();
    const auto outerRadius = wheel.getWidth() * 0.5f - 18.0f;
    if (outerRadius <= 0.0f)
        return;
    const auto centre = wheel.getCentre();
    if (rhythmRings.empty())
    {
        graphics.setColour(juce::Colour(StudioColours::border).withAlpha(0.3f));
        for (const auto scale : { 0.4f, 0.7f, 1.0f })
        {
            const auto radius = outerRadius * scale;
            graphics.drawEllipse(
                centre.x - radius, centre.y - radius,
                radius * 2.0f, radius * 2.0f, 1.0f);
        }
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(13.0f);
        graphics.drawFittedText(
            durationBeats > 0.0
                ? "No drum hits yet.\nAdd notes in DRUMS or record with DRUM PADS."
                : getDescription(),
            wheel.toNearestInt().reduced(20), juce::Justification::centred, 3);
    }
    else
    {
        const auto innerRadius = std::max(18.0f, outerRadius * 0.32f);
        const auto spacing = rhythmRings.size() > 1
            ? (outerRadius - innerRadius) / static_cast<float>(rhythmRings.size() - 1)
            : outerRadius;
        const auto stride = std::max(1.0, std::ceil(durationBeats / 32.0));
        const auto activePhase = playheadPhase();
        for (double beat = 0.0; beat < durationBeats; beat += stride)
        {
            const auto phase = beat / durationBeats;
            const auto start = pointForPhase(phase, innerRadius - 8.0f);
            const auto end = pointForPhase(phase, outerRadius + 5.0f);
            graphics.setColour(juce::Colour(StudioColours::border).withAlpha(0.15f));
            graphics.drawLine({ start, end }, 1.0f);
            if (durationBeats <= 16.0 || std::fmod(beat, 4.0) < 0.000001)
            {
                const auto label = pointForPhase(phase, outerRadius + 13.0f);
                graphics.setColour(juce::Colour(StudioColours::secondaryText));
                graphics.setFont(9.0f);
                graphics.drawText(
                    juce::String(beat + 1.0, 0),
                    juce::Rectangle<float>(
                        label.x - 14.0f, label.y - 6.0f, 28.0f, 12.0f),
                    juce::Justification::centred);
            }
        }
        auto hits = 0;
        for (std::size_t index = 0; index < rhythmRings.size(); ++index)
        {
            const auto& ring = rhythmRings[index];
            const auto radius = outerRadius - static_cast<float>(index) * spacing;
            const auto highlighted = selectedRing < 0
                || selectedRing == static_cast<int>(index);
            const auto colour = ringColour(index);
            graphics.setColour(colour.withAlpha(highlighted ? 0.5f : 0.2f));
            graphics.drawEllipse(
                centre.x - radius, centre.y - radius,
                radius * 2.0f, radius * 2.0f,
                selectedRing == static_cast<int>(index) ? 2.0f : 1.0f);
            juce::Path polygon;
            for (std::size_t hit = 0; hit < ring.hits.size(); ++hit)
            {
                const auto point = pointForPhase(ring.hits[hit].phase, radius);
                if (hit == 0)
                    polygon.startNewSubPath(point);
                else
                    polygon.lineTo(point);
            }
            if (ring.hits.size() > 2)
                polygon.closeSubPath();
            graphics.setColour(colour.withAlpha(highlighted ? 0.65f : 0.18f));
            graphics.strokePath(polygon, juce::PathStrokeType(1.2f));
            for (const auto& hit : ring.hits)
            {
                const auto point = pointForPhase(hit.phase, radius);
                const auto dotRadius =
                    std::min(6.0f, std::max(2.5f, spacing * 0.38f))
                    * (0.55f + 0.45f * static_cast<float>(hit.velocity) / 127.0f);
                const auto active = transportPlaying && activePhase.has_value()
                    && *activePhase >= hit.phase
                    && (*activePhase - hit.phase) * durationBeats <= 0.2;
                if (active)
                {
                    graphics.setColour(colour.withAlpha(0.3f));
                    graphics.fillEllipse(
                        point.x - dotRadius - 3.0f, point.y - dotRadius - 3.0f,
                        dotRadius * 2.0f + 6.0f, dotRadius * 2.0f + 6.0f);
                }
                graphics.setColour(colour.withAlpha(
                    (highlighted ? 1.0f : 0.4f) * static_cast<float>(0.35 + 0.65 * hit.probability)));
                graphics.fillEllipse(
                    point.x - dotRadius, point.y - dotRadius,
                    dotRadius * 2.0f, dotRadius * 2.0f);
                graphics.setColour(
                    juce::Colour(StudioColours::text)
                        .withAlpha(highlighted ? 0.8f : 0.3f));
                graphics.drawEllipse(
                    point.x - dotRadius, point.y - dotRadius,
                    dotRadius * 2.0f, dotRadius * 2.0f, active ? 2.0f : 1.0f);
            }
            hits += static_cast<int>(ring.hits.size());
        }
        graphics.setColour(juce::Colour(StudioColours::window));
        graphics.fillEllipse(centre.x - 16.0f, centre.y - 16.0f, 32.0f, 32.0f);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
        graphics.drawText(
            juce::String(hits),
            juce::Rectangle<float>(
                centre.x - 28.0f, centre.y - 14.0f, 56.0f, 20.0f),
            juce::Justification::centred);
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(9.0f);
        graphics.drawText(
            "HITS",
            juce::Rectangle<float>(
                centre.x - 22.0f, centre.y + 5.0f, 44.0f, 12.0f),
            juce::Justification::centred);
    }
    if (const auto phase = playheadPhase())
    {
        const auto start = pointForPhase(*phase, 26.0f);
        const auto end = pointForPhase(*phase, outerRadius + 5.0f);
        graphics.setColour(
            juce::Colour(transportPlaying
                ? StudioColours::text : StudioColours::secondaryText)
                .withAlpha(transportPlaying ? 0.9f : 0.6f));
        graphics.drawLine({ start, end }, 1.5f);
        graphics.fillEllipse(end.x - 3.0f, end.y - 3.0f, 6.0f, 6.0f);
    }
    if (hasKeyboardFocus(true))
    {
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.drawRect(getLocalBounds().reduced(2), 2);
    }
}

void DrumGeometryComponent::resized()
{
    auto area = getLocalBounds().reduced(8);
    summaryLabel.setBounds(area.removeFromTop(26));
    hintLabel.setBounds(area.removeFromBottom(26));
    area.removeFromTop(6);
    area.removeFromBottom(6);
    legend.setBounds(area.removeFromLeft(std::min(180, getWidth() / 3)));
}

juce::Rectangle<float> DrumGeometryComponent::wheelBounds() const
{
    auto area = getLocalBounds().reduced(8)
        .withTrimmedTop(32).withTrimmedBottom(32);
    area.removeFromLeft(std::min(180, getWidth() / 3) + 12);
    const auto size = static_cast<float>(
        std::max(0, std::min(area.getWidth(), area.getHeight())));
    return juce::Rectangle<float>(size, size).withCentre(area.toFloat().getCentre());
}

juce::Point<float> DrumGeometryComponent::pointForPhase(
    double phase, float radius) const
{
    const auto angle = phase * juce::MathConstants<double>::twoPi;
    const auto centre = wheelBounds().getCentre();
    return { centre.x + static_cast<float>(std::sin(angle)) * radius,
             centre.y - static_cast<float>(std::cos(angle)) * radius };
}

juce::Colour DrumGeometryComponent::ringColour(std::size_t ring) const
{
    const auto& voice = rhythmRings[ring];
    if (voice.outputGroup.equalsIgnoreCase("Kick"))
        return juce::Colour(StudioColours::orange);
    if (voice.outputGroup.equalsIgnoreCase("Snare"))
        return juce::Colour(StudioColours::amber);
    if (voice.outputGroup.equalsIgnoreCase("Toms"))
        return juce::Colour(StudioColours::violet);
    if (voice.outputGroup.equalsIgnoreCase("Cymbals"))
        return juce::Colour(StudioColours::green);
    const std::array palette {
        StudioColours::orange, StudioColours::amber,
        StudioColours::green, StudioColours::violet
    };
    return juce::Colour(palette[static_cast<std::size_t>(voice.pitch) % palette.size()]);
}

int DrumGeometryComponent::getNumRows()
{
    return static_cast<int>(rhythmRings.size());
}

juce::String DrumGeometryComponent::getNameForRow(int row)
{
    if (!juce::isPositiveAndBelow(row, rhythmRings.size()))
        return {};
    const auto& ring = rhythmRings[static_cast<std::size_t>(row)];
    return ring.name + ", " + ring.articulation + ", "
        + juce::String(static_cast<int>(ring.hits.size())) + " hits";
}

void DrumGeometryComponent::paintListBoxItem(
    int row, juce::Graphics& graphics, int width, int height, bool selected)
{
    if (!juce::isPositiveAndBelow(row, rhythmRings.size()))
        return;
    const auto index = static_cast<std::size_t>(row);
    const auto& ring = rhythmRings[index];
    const auto colour = ringColour(index);
    if (selected)
    {
        graphics.setColour(juce::Colour(StudioColours::raised));
        graphics.fillAll();
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.drawRect(0, 0, width, height, 2);
    }
    graphics.setColour(colour);
    graphics.fillRoundedRectangle(
        6.0f, 7.0f, 4.0f, static_cast<float>(height - 14), 2.0f);
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(12.0f);
    graphics.drawText(ring.name, 16, 3, width - 50, 17, juce::Justification::centredLeft);
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(10.0f);
    graphics.drawText(
        ring.articulation.isNotEmpty()
            ? ring.articulation : "Note " + juce::String(ring.pitch),
        16, 19, width - 50, 14, juce::Justification::centredLeft);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    graphics.drawText(
        juce::String(static_cast<int>(ring.hits.size())),
        width - 32, 0, 24, height, juce::Justification::centredRight);
}

void DrumGeometryComponent::selectedRowsChanged(int row)
{
    selectedRing = row;
    refreshHint();
    repaint();
}

void DrumGeometryComponent::refreshHint()
{
    hintLabel.setText(
        durationBeats <= 0.0 ? getDescription()
        : rhythmRings.empty() ? "Add notes in DRUMS or record a take with DRUM PADS."
        : selectedRing >= 0
            ? "Highlighted voice: " + getNameForRow(selectedRing)
                + ". Escape shows all voices."
            : "Clockwise from the top. One turn = whole clip. "
                "Dot size = velocity. Select a voice to highlight.",
        juce::dontSendNotification);
}

bool DrumGeometryComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        legend.deselectAllRows();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::upKey
        || key.getKeyCode() == juce::KeyPress::downKey
        || key.getKeyCode() == juce::KeyPress::homeKey
        || key.getKeyCode() == juce::KeyPress::endKey)
        return legend.keyPressed(key);
    return false;
}

void DrumGeometryComponent::focusGained(FocusChangeType)
{
    repaint();
}

void DrumGeometryComponent::focusLost(FocusChangeType)
{
    repaint();
}
}
