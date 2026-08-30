#include "TimelineComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <cmath>

namespace studio
{
TimelineComponent::TimelineComponent()
{
    setOpaque(true);
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TimelineComponent::setProject(const Project* projectToDisplay)
{
    project = projectToDisplay;
    repaint();
}

void TimelineComponent::setSelection(juce::String trackId, juce::String clipId)
{
    selectedTrackId = std::move(trackId);
    selectedClipId = std::move(clipId);
    repaint();
}

void TimelineComponent::setPlayheadSeconds(double seconds)
{
    const auto oldX = static_cast<int>(secondsToX(playheadSeconds));
    playheadSeconds = std::max(0.0, seconds);
    const auto newX = static_cast<int>(secondsToX(playheadSeconds));
    repaint(std::min(oldX, newX) - 3, 0, std::abs(newX - oldX) + 7, getHeight());
}

void TimelineComponent::setPixelsPerSecond(double pixels)
{
    pixelsPerSecond = juce::jlimit(24.0, 320.0, pixels);
    repaint();
}

int TimelineComponent::preferredWidth(int minimumWidth) const
{
    const auto seconds = project != nullptr ? project->lengthSeconds() + 8.0 : 16.0;
    return std::max(minimumWidth,
                    trackHeaderWidth + static_cast<int>(std::ceil(seconds * pixelsPerSecond)));
}

int TimelineComponent::preferredHeight(int minimumHeight) const
{
    const auto trackCount = project != nullptr ? static_cast<int>(project->tracks.size()) : 0;
    return std::max(minimumHeight, rulerHeight + trackCount * trackHeight);
}

void TimelineComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, 0, getWidth(), rulerHeight);
    graphics.fillRect(0, 0, trackHeaderWidth, getHeight());

    if (project == nullptr)
        return;

    const auto secondsPerBeat = 60.0 / project->tempo;
    const auto beats = static_cast<int>(std::ceil((getWidth() - trackHeaderWidth)
                                                  / pixelsPerSecond
                                                  / secondsPerBeat));
    for (int beat = 0; beat <= beats; ++beat)
    {
        const auto seconds = beat * secondsPerBeat;
        const auto x = static_cast<int>(secondsToX(seconds));
        const auto isBar = beat % project->timeSignatureNumerator == 0;
        graphics.setColour(juce::Colour(isBar ? StudioColours::border : 0xff25292d));
        graphics.drawVerticalLine(x, static_cast<float>(rulerHeight), static_cast<float>(getHeight()));

        if (isBar)
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(12.0f);
            graphics.drawText(juce::String(beat / project->timeSignatureNumerator + 1),
                              x + 5,
                              0,
                              42,
                              rulerHeight,
                              juce::Justification::centredLeft);
        }
    }

    for (std::size_t index = 0; index < project->tracks.size(); ++index)
    {
        const auto& track = project->tracks[index];
        const auto y = rulerHeight + static_cast<int>(index) * trackHeight;
        const auto selected = track.id == selectedTrackId;

        graphics.setColour(juce::Colour(selected ? 0xff22272b : 0xff15181b));
        graphics.fillRect(0, y, trackHeaderWidth, trackHeight);
        graphics.setColour(juce::Colour(StudioColours::border));
        graphics.drawHorizontalLine(y + trackHeight - 1, 0.0f, static_cast<float>(getWidth()));
        graphics.drawVerticalLine(trackHeaderWidth - 1,
                                  static_cast<float>(y),
                                  static_cast<float>(y + trackHeight));

        graphics.setColour(track.colour);
        graphics.fillRect(12, y + 17, 4, 42);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(14.0f);
        graphics.drawText(track.name,
                          26,
                          y + 12,
                          trackHeaderWidth - 38,
                          24,
                          juce::Justification::centredLeft);

        juce::String state;
        if (track.armed) state << "REC ";
        if (track.muted) state << "MUTE ";
        if (track.solo) state << "SOLO";
        if (state.isEmpty()) state = trackTypeToString(track.type).toUpperCase();
        graphics.setColour(juce::Colour(track.armed ? StudioColours::orange
                                                    : StudioColours::secondaryText));
        graphics.setFont(10.5f);
        graphics.drawText(state.trimEnd(),
                          26,
                          y + 38,
                          trackHeaderWidth - 38,
                          22,
                          juce::Justification::centredLeft);
    }

    for (const auto& hit : clipHits())
    {
        const auto* clip = project->findClip(hit.clipId);
        if (clip == nullptr)
            continue;

        auto bounds = hit.bounds;
        if (draggedClipId == hit.clipId)
            bounds.setX(secondsToX(dragPreviewStart));

        const auto selected = hit.clipId == selectedClipId;
        graphics.setColour(clip->colour.withAlpha(clip->muted ? 0.35f : 0.86f));
        graphics.fillRoundedRectangle(bounds, 5.0f);
        graphics.setColour(selected ? juce::Colours::white : clip->colour.brighter(0.35f));
        graphics.drawRoundedRectangle(bounds, 5.0f, selected ? 2.0f : 1.0f);

        graphics.saveState();
        graphics.reduceClipRegion(bounds.toNearestInt().reduced(5));
        const auto seed = static_cast<std::uint32_t>(clip->id.hashCode());
        juce::Random random(seed);
        const auto middle = bounds.getCentreY() + 8.0f;
        graphics.setColour(juce::Colours::white.withAlpha(0.28f));
        for (float x = bounds.getX() + 8.0f; x < bounds.getRight() - 4.0f; x += 4.0f)
        {
            const auto amplitude = random.nextFloat() * (bounds.getHeight() * 0.23f);
            graphics.drawVerticalLine(static_cast<int>(x), middle - amplitude, middle + amplitude);
        }
        graphics.restoreState();

        graphics.setColour(juce::Colours::white);
        graphics.setFont(12.5f);
        graphics.drawText(clip->name,
                          bounds.toNearestInt().withHeight(24).reduced(8, 0),
                          juce::Justification::centredLeft);
    }

    const auto playheadX = secondsToX(playheadSeconds);
    graphics.setColour(juce::Colour(StudioColours::orange));
    graphics.drawLine(playheadX, 0.0f, playheadX, static_cast<float>(getHeight()), 1.5f);
    juce::Path marker;
    marker.addTriangle(playheadX - 5.0f, 0.0f, playheadX + 5.0f, 0.0f, playheadX, 8.0f);
    graphics.fillPath(marker);
}

void TimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    for (const auto& hit : clipHits())
    {
        if (!hit.bounds.contains(event.position))
            continue;

        selectedTrackId = hit.trackId;
        selectedClipId = hit.clipId;
        draggedClipId = hit.clipId;
        dragStartX = event.position.x;
        if (const auto* clip = project != nullptr ? project->findClip(hit.clipId) : nullptr)
        {
            dragOriginalStart = clip->startSeconds;
            dragPreviewStart = dragOriginalStart;
        }

        if (onClipSelected)
            onClipSelected(hit.trackId, hit.clipId);
        repaint();
        return;
    }

    const auto trackIndex = trackIndexAt(event.position.y);
    if (project != nullptr && trackIndex >= 0 && trackIndex < static_cast<int>(project->tracks.size()))
    {
        selectedTrackId = project->tracks[static_cast<std::size_t>(trackIndex)].id;
        selectedClipId.clear();
        if (onTrackSelected)
            onTrackSelected(selectedTrackId);
    }

    if (event.position.x >= trackHeaderWidth && onSeek)
        onSeek(xToSeconds(event.position.x));
    repaint();
}

void TimelineComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (draggedClipId.isEmpty())
        return;

    const auto deltaSeconds = (event.position.x - dragStartX) / pixelsPerSecond;
    const auto unsnapped = std::max(0.0, dragOriginalStart + deltaSeconds);
    const auto beat = project != nullptr ? 60.0 / project->tempo : 0.5;
    dragPreviewStart = std::round(unsnapped / beat) * beat;
    repaint();
}

void TimelineComponent::mouseUp(const juce::MouseEvent&)
{
    if (draggedClipId.isNotEmpty()
        && std::abs(dragPreviewStart - dragOriginalStart) > 0.0001
        && onClipMoved)
        onClipMoved(draggedClipId, dragPreviewStart);

    draggedClipId.clear();
    repaint();
}

std::vector<TimelineComponent::Hit> TimelineComponent::clipHits() const
{
    std::vector<Hit> hits;
    if (project == nullptr)
        return hits;

    for (std::size_t trackIndex = 0; trackIndex < project->tracks.size(); ++trackIndex)
    {
        const auto& track = project->tracks[trackIndex];
        const auto y = rulerHeight + static_cast<int>(trackIndex) * trackHeight + 12;
        for (const auto& clip : track.clips)
        {
            const auto x = secondsToX(clip.startSeconds);
            const auto width = static_cast<float>(std::max(20.0, clip.durationSeconds * pixelsPerSecond));
            hits.push_back({ track.id, clip.id, { x, static_cast<float>(y), width, trackHeight - 24.0f } });
        }
    }

    return hits;
}

int TimelineComponent::trackIndexAt(float y) const noexcept
{
    return y < rulerHeight ? -1 : static_cast<int>((y - rulerHeight) / trackHeight);
}

double TimelineComponent::xToSeconds(float x) const noexcept
{
    return std::max(0.0, static_cast<double>(x - trackHeaderWidth) / pixelsPerSecond);
}

float TimelineComponent::secondsToX(double seconds) const noexcept
{
    return static_cast<float>(trackHeaderWidth + seconds * pixelsPerSecond);
}
}
