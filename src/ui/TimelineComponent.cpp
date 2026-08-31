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

void TimelineComponent::setRecordingPreview(juce::String trackId,
                                            double startSeconds,
                                            double durationSeconds,
                                            std::vector<float> waveformPeaks)
{
    recordingTrackId = std::move(trackId);
    recordingStartSeconds = std::max(0.0, startSeconds);
    recordingDurationSeconds = std::max(0.0, durationSeconds);
    recordingPeaks = std::move(waveformPeaks);
    repaint();
}

void TimelineComponent::clearRecordingPreview()
{
    recordingTrackId.clear();
    recordingDurationSeconds = 0.0;
    recordingPeaks.clear();
    repaint();
}

void TimelineComponent::setPixelsPerSecond(double pixels)
{
    pixelsPerSecond = juce::jlimit(24.0, 320.0, pixels);
    repaint();
}

double TimelineComponent::getPixelsPerSecond() const noexcept
{
    return pixelsPerSecond;
}

float TimelineComponent::xForSeconds(double seconds) const noexcept
{
    return secondsToX(seconds);
}

int TimelineComponent::preferredWidth(int minimumWidth) const
{
    const auto projectSeconds = project != nullptr ? project->lengthSeconds() : 8.0;
    const auto previewEnd = recordingStartSeconds + recordingDurationSeconds;
    const auto seconds = std::max(projectSeconds, previewEnd) + 8.0;
    return std::max(minimumWidth,
                    trackHeaderWidth + static_cast<int>(std::ceil(seconds * pixelsPerSecond)));
}

int TimelineComponent::preferredHeight(int minimumHeight) const
{
    const auto trackCount = project != nullptr ? static_cast<int>(project->tracks.size()) : 0;
    return std::max(minimumHeight,
                    rulerHeight + trackCount * trackHeight + addTrackHeight);
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

        const auto drawControl = [&graphics, y](int x,
                                                const juce::String& label,
                                                bool active,
                                                juce::Colour activeColour)
        {
            const juce::Rectangle<float> bounds(static_cast<float>(x),
                                                static_cast<float>(y + 48),
                                                28.0f,
                                                24.0f);
            graphics.setColour(active ? activeColour : juce::Colour(StudioColours::raised));
            graphics.fillRoundedRectangle(bounds, 3.0f);
            graphics.setColour(active ? juce::Colours::white
                                      : juce::Colour(StudioColours::secondaryText));
            graphics.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            graphics.drawText(label, bounds.toNearestInt(), juce::Justification::centred);
        };

        drawControl(26, "M", track.muted, juce::Colour(StudioColours::amber));
        drawControl(60, "S", track.solo, juce::Colour(StudioColours::green));
        if (track.type == TrackType::audio)
            drawControl(94, "R", track.armed, juce::Colour(StudioColours::orange));

        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(juce::Font(juce::FontOptions(9.5f)));
        const auto routingLabel = track.type == TrackType::audio
            ? "IN " + juce::String(track.inputChannel + 1)
                + (track.stereoInput ? "+" + juce::String(track.inputChannel + 2) : juce::String())
            : trackTypeToString(track.type).toUpperCase();
        graphics.drawText(routingLabel,
                          128,
                          y + 48,
                          40,
                          24,
                          juce::Justification::centredRight);
    }

    const auto addTrackY = rulerHeight + static_cast<int>(project->tracks.size()) * trackHeight;
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, addTrackY, trackHeaderWidth, addTrackHeight);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawRect(10, addTrackY + 7, trackHeaderWidth - 20, addTrackHeight - 14, 1);
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    graphics.drawText("+ ADD AUDIO TRACK",
                      14,
                      addTrackY + 7,
                      trackHeaderWidth - 28,
                      addTrackHeight - 14,
                      juce::Justification::centred);

    for (const auto& hit : clipHits())
    {
        const auto* clip = project->findClip(hit.clipId);
        if (clip == nullptr)
            continue;

        const auto drawWaveform = [&graphics, clip](juce::Rectangle<float> waveformBounds,
                                                    float alpha)
        {
            graphics.saveState();
            graphics.reduceClipRegion(waveformBounds.toNearestInt().reduced(5));
            const auto seed = static_cast<std::uint32_t>(clip->id.hashCode());
            juce::Random random(seed);
            const auto middle = waveformBounds.getCentreY() + 8.0f;
            graphics.setColour(juce::Colours::white.withAlpha(alpha));
            for (float x = waveformBounds.getX() + 8.0f;
                 x < waveformBounds.getRight() - 4.0f;
                 x += 4.0f)
            {
                const auto amplitude = random.nextFloat() * (waveformBounds.getHeight() * 0.23f);
                graphics.drawVerticalLine(static_cast<int>(x),
                                          middle - amplitude,
                                          middle + amplitude);
            }
            graphics.restoreState();
        };

        auto bounds = hit.bounds;
        if (draggedClipId == hit.clipId)
        {
            graphics.setColour(clip->colour.withAlpha(0.16f));
            graphics.fillRoundedRectangle(hit.bounds, 5.0f);
            drawWaveform(hit.bounds, 0.12f);
            graphics.setColour(juce::Colours::white.withAlpha(0.28f));
            const float dashLengths[] { 4.0f, 4.0f };
            juce::Path originalShape;
            originalShape.addRoundedRectangle(hit.bounds, 5.0f);
            juce::Path dashedOutline;
            juce::PathStrokeType(1.0f).createDashedStroke(dashedOutline,
                                                          originalShape,
                                                          dashLengths,
                                                          2);
            graphics.fillPath(dashedOutline);

            bounds.setX(secondsToX(dragPreviewStart));
            bounds.setY(trackY(dragPreviewTrackId) + 12.0f);
            bounds.setWidth(static_cast<float>(std::max(20.0,
                                                        dragPreviewDuration
                                                            * pixelsPerSecond)));
        }

        const auto selected = hit.clipId == selectedClipId;
        graphics.setColour(clip->colour.withAlpha(clip->muted ? 0.35f : 0.86f));
        graphics.fillRoundedRectangle(bounds, 5.0f);
        graphics.setColour(selected ? juce::Colours::white : clip->colour.brighter(0.35f));
        graphics.drawRoundedRectangle(bounds, 5.0f, selected ? 2.0f : 1.0f);
        if (selected)
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.85f));
            graphics.fillRoundedRectangle(bounds.getX() + 3.0f,
                                          bounds.getY() + 8.0f,
                                          3.0f,
                                          bounds.getHeight() - 16.0f,
                                          1.5f);
            graphics.fillRoundedRectangle(bounds.getRight() - 6.0f,
                                          bounds.getY() + 8.0f,
                                          3.0f,
                                          bounds.getHeight() - 16.0f,
                                          1.5f);
        }

        drawWaveform(bounds, 0.28f);

        graphics.setColour(juce::Colours::white);
        graphics.setFont(12.5f);
        graphics.drawText(clip->name,
                          bounds.toNearestInt().withHeight(24).reduced(8, 0),
                          juce::Justification::centredLeft);
    }

    if (recordingTrackId.isNotEmpty())
    {
        const auto iterator = std::find_if(project->tracks.cbegin(),
                                           project->tracks.cend(),
                                           [this](const auto& track)
        {
            return track.id == recordingTrackId;
        });
        if (iterator != project->tracks.cend())
        {
            const auto index = static_cast<int>(std::distance(project->tracks.cbegin(), iterator));
            const auto y = rulerHeight + index * trackHeight + 12;
            const juce::Rectangle<float> preview(
                secondsToX(recordingStartSeconds),
                static_cast<float>(y),
                static_cast<float>(std::max(96.0, recordingDurationSeconds * pixelsPerSecond)),
                trackHeight - 24.0f);
            graphics.setColour(juce::Colour(StudioColours::orange).withAlpha(0.72f));
            graphics.fillRoundedRectangle(preview, 5.0f);
            graphics.setColour(juce::Colours::white);
            graphics.drawRoundedRectangle(preview, 5.0f, 1.5f);
            const auto centre = preview.getCentreY() + 6.0f;
            graphics.setColour(juce::Colours::white.withAlpha(0.24f));
            graphics.drawHorizontalLine(static_cast<int>(centre),
                                        preview.getX() + 6.0f,
                                        preview.getRight() - 6.0f);

            if (!recordingPeaks.empty())
            {
                const auto columns = juce::jmax(1,
                                                juce::jmin(static_cast<int>(recordingPeaks.size()),
                                                           static_cast<int>(preview.getWidth()) - 12));
                const auto maximumHeight = preview.getHeight() * 0.32f;
                graphics.setColour(juce::Colours::white.withAlpha(0.62f));
                for (int column = 0; column < columns; ++column)
                {
                    const auto peakIndex = static_cast<std::size_t>(
                        static_cast<double>(column)
                        / static_cast<double>(columns)
                        * static_cast<double>(recordingPeaks.size()));
                    const auto peak = std::sqrt(juce::jlimit(
                        0.0f,
                        1.0f,
                        recordingPeaks[std::min(peakIndex, recordingPeaks.size() - 1)]));
                    const auto x = preview.getX()
                        + 6.0f
                        + static_cast<float>(column)
                            / static_cast<float>(columns)
                            * (preview.getWidth() - 12.0f);
                    graphics.drawVerticalLine(static_cast<int>(x),
                                              centre - peak * maximumHeight,
                                              centre + peak * maximumHeight);
                }
            }

            graphics.setColour(juce::Colours::white);
            graphics.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            graphics.drawText("RECORDING  " + juce::String(recordingDurationSeconds, 1) + " s",
                              preview.toNearestInt().withHeight(24).reduced(8, 0),
                              juce::Justification::centredLeft);
        }
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
    if (project != nullptr && event.position.x < trackHeaderWidth)
    {
        const auto addTrackY = rulerHeight + static_cast<int>(project->tracks.size()) * trackHeight;
        if (event.position.y >= static_cast<float>(addTrackY)
            && event.position.y < static_cast<float>(addTrackY + addTrackHeight))
        {
            if (onAddTrack)
                onAddTrack();
            return;
        }

        const auto trackIndex = trackIndexAt(event.position.y);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(project->tracks.size()))
        {
            const auto& track = project->tracks[static_cast<std::size_t>(trackIndex)];
            const auto localY = static_cast<int>(event.position.y)
                - rulerHeight
                - trackIndex * trackHeight;
            const auto x = static_cast<int>(event.position.x);
            if (localY >= 44 && localY <= 76)
            {
                if (x >= 22 && x <= 58 && onTrackMute)
                    onTrackMute(track.id);
                else if (x >= 58 && x <= 92 && onTrackSolo)
                    onTrackSolo(track.id);
                else if (x >= 92 && x <= 126
                         && track.type == TrackType::audio
                         && onTrackArm)
                    onTrackArm(track.id);
                else
                    return;

                selectedTrackId = track.id;
                selectedClipId.clear();
                repaint();
                return;
            }
        }
    }

    for (const auto& hit : clipHits())
    {
        if (!hit.bounds.contains(event.position))
            continue;

        selectedTrackId = hit.trackId;
        selectedClipId = hit.clipId;
        draggedClipId = hit.clipId;
        dragOriginalTrackId = hit.trackId;
        dragPreviewTrackId = hit.trackId;
        dragStartX = event.position.x;
        if (const auto* clip = project != nullptr ? project->findClip(hit.clipId) : nullptr)
        {
            dragOriginalStart = clip->startSeconds;
            dragOriginalSourceOffset = clip->sourceOffsetSeconds;
            dragOriginalDuration = clip->durationSeconds;
            dragPreviewStart = dragOriginalStart;
            dragPreviewSourceOffset = dragOriginalSourceOffset;
            dragPreviewDuration = dragOriginalDuration;
            if (event.position.x <= hit.bounds.getX() + 9.0f)
                dragMode = DragMode::trimStart;
            else if (event.position.x >= hit.bounds.getRight() - 9.0f)
                dragMode = DragMode::trimEnd;
            else
                dragMode = DragMode::move;
        }

        if (onClipSelected)
            onClipSelected(hit.trackId, hit.clipId);
        if (onSeek)
            onSeek(xToSeconds(event.position.x));
        repaint();
        return;
    }

    const auto trackIndex = trackIndexAt(event.position.y);
    if (project != nullptr && trackIndex >= 0 && trackIndex < static_cast<int>(project->tracks.size()))
    {
        const auto clickedTrackId = project->tracks[static_cast<std::size_t>(trackIndex)].id;
        if (clickedTrackId != selectedTrackId)
        {
            selectedTrackId = clickedTrackId;
            selectedClipId.clear();
            if (onTrackSelected)
                onTrackSelected(selectedTrackId);
        }
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
    const auto beat = project != nullptr ? 60.0 / project->tempo : 0.5;
    const auto minimumDuration = std::max(0.01, beat / 16.0);

    if (dragMode == DragMode::move)
    {
        const auto unsnapped = std::max(0.0, dragOriginalStart + deltaSeconds);
        dragPreviewStart = std::round(unsnapped / beat) * beat;

        const auto targetTrackIndex = trackIndexAt(event.position.y);
        if (project != nullptr
            && targetTrackIndex >= 0
            && targetTrackIndex < static_cast<int>(project->tracks.size()))
        {
            const auto& targetTrack = project->tracks[static_cast<std::size_t>(targetTrackIndex)];
            if (targetTrack.type == TrackType::audio)
                dragPreviewTrackId = targetTrack.id;
        }
    }
    else if (dragMode == DragMode::trimStart)
    {
        const auto minimumStart = std::max(0.0,
                                           dragOriginalStart - dragOriginalSourceOffset);
        const auto maximumStart = dragOriginalStart + dragOriginalDuration - minimumDuration;
        const auto unsnapped = juce::jlimit(minimumStart,
                                           maximumStart,
                                           dragOriginalStart + deltaSeconds);
        dragPreviewStart = std::round(unsnapped / (beat / 4.0)) * (beat / 4.0);
        dragPreviewStart = juce::jlimit(minimumStart, maximumStart, dragPreviewStart);
        const auto appliedDelta = dragPreviewStart - dragOriginalStart;
        dragPreviewSourceOffset = dragOriginalSourceOffset + appliedDelta;
        dragPreviewDuration = dragOriginalDuration - appliedDelta;
    }
    else if (dragMode == DragMode::trimEnd)
    {
        const auto sourceRemaining = project != nullptr
            ? project->findClip(draggedClipId)->sourceLengthSeconds - dragOriginalSourceOffset
            : dragOriginalDuration;
        const auto unsnappedDuration = juce::jlimit(minimumDuration,
                                                    sourceRemaining,
                                                    dragOriginalDuration + deltaSeconds);
        dragPreviewDuration = std::round(unsnappedDuration / (beat / 4.0))
            * (beat / 4.0);
        dragPreviewDuration = juce::jlimit(minimumDuration,
                                           sourceRemaining,
                                           dragPreviewDuration);
    }
    repaint();
}

void TimelineComponent::mouseUp(const juce::MouseEvent&)
{
    if (draggedClipId.isNotEmpty())
    {
        if (dragMode == DragMode::move
            && std::abs(dragPreviewStart - dragOriginalStart) > 0.0001
            && onClipMoved)
            onClipMoved(draggedClipId, dragPreviewTrackId, dragPreviewStart);
        else if (dragMode == DragMode::move
                 && dragPreviewTrackId != dragOriginalTrackId
                 && onClipMoved)
            onClipMoved(draggedClipId, dragPreviewTrackId, dragPreviewStart);
        else if ((dragMode == DragMode::trimStart || dragMode == DragMode::trimEnd)
                 && (std::abs(dragPreviewStart - dragOriginalStart) > 0.0001
                     || std::abs(dragPreviewDuration - dragOriginalDuration) > 0.0001)
                 && onClipTrimmed)
            onClipTrimmed(draggedClipId,
                          dragPreviewStart,
                          dragPreviewSourceOffset,
                          dragPreviewDuration);
    }

    draggedClipId.clear();
    dragOriginalTrackId.clear();
    dragPreviewTrackId.clear();
    dragMode = DragMode::none;
    repaint();
}

void TimelineComponent::mouseWheelMove(const juce::MouseEvent& event,
                                       const juce::MouseWheelDetails& wheel)
{
    if ((event.mods.isCommandDown() || event.mods.isCtrlDown()) && onZoomRequested)
    {
        onZoomRequested(wheel.deltaY > 0.0f ? 1.2 : 1.0 / 1.2);
        return;
    }

    juce::Component::mouseWheelMove(event, wheel);
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

float TimelineComponent::trackY(const juce::String& trackId) const noexcept
{
    if (project == nullptr)
        return static_cast<float>(rulerHeight);

    const auto iterator = std::find_if(project->tracks.cbegin(),
                                       project->tracks.cend(),
                                       [&trackId](const auto& track)
    {
        return track.id == trackId;
    });
    if (iterator == project->tracks.cend())
        return static_cast<float>(rulerHeight);

    const auto index = static_cast<int>(std::distance(project->tracks.cbegin(), iterator));
    return static_cast<float>(rulerHeight + index * trackHeight);
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
