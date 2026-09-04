#include "TimelineComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace studio
{
namespace
{
float waveformAmplitude(const AudioClip& clip, double sourceSeconds) noexcept
{
    const auto sourceKey = clip.sourceFile.getFullPathName().isNotEmpty()
        ? clip.sourceFile.getFullPathName()
        : clip.id;
    auto value = static_cast<std::uint32_t>(sourceKey.hashCode())
        ^ static_cast<std::uint32_t>(std::max(0.0, sourceSeconds) * 64.0);
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return static_cast<float>(value & 0xffffu) / 65535.0f;
}
}

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

void TimelineComponent::setViewportPosition(int horizontalPosition)
{
    const auto newPosition = std::max(0, horizontalPosition);
    if (newPosition == viewportPositionX)
        return;

    viewportPositionX = newPosition;
    repaint();
}

void TimelineComponent::setRecordingPreviews(std::vector<RecordingPreview> previews)
{
    for (auto& preview : previews)
    {
        preview.startSeconds = std::max(0.0, preview.startSeconds);
        preview.durationSeconds = std::max(0.0, preview.durationSeconds);
    }
    recordingPreviews = std::move(previews);
    repaint();
}

void TimelineComponent::clearRecordingPreviews()
{
    recordingPreviews.clear();
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
    auto previewEnd = 0.0;
    for (const auto& preview : recordingPreviews)
        previewEnd = std::max(previewEnd, preview.startSeconds + preview.durationSeconds);
    const auto seconds = std::max(projectSeconds, previewEnd) + 8.0;
    return std::max(minimumWidth,
                    trackHeaderWidth + static_cast<int>(std::ceil(seconds * pixelsPerSecond)));
}

int TimelineComponent::preferredHeight(int minimumHeight) const
{
    const auto trackCount = static_cast<int>(visibleTracks().size());
    return std::max(minimumHeight,
                    rulerHeight + trackCount * trackHeight + addTrackHeight);
}

void TimelineComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, 0, getWidth(), rulerHeight);
    graphics.fillRect(viewportPositionX, 0, trackHeaderWidth, getHeight());

    if (project == nullptr)
        return;

    const auto maximumSeconds = std::max(
        project->lengthSeconds() + 8.0,
        static_cast<double>(getWidth() - trackHeaderWidth) / pixelsPerSecond);
    auto seconds = 0.0;
    for (int line = 0; line < 100000 && seconds <= maximumSeconds; ++line)
    {
        const auto x = static_cast<int>(secondsToX(seconds));
        const auto position = project->musicalPositionAt(seconds);
        const auto isBar = position.beat == 1 && position.ticks < 2;
        graphics.setColour(juce::Colour(isBar ? StudioColours::border : 0xff25292d));
        graphics.drawVerticalLine(x, static_cast<float>(rulerHeight), static_cast<float>(getHeight()));

        if (isBar)
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(12.0f);
            graphics.drawText(juce::String(position.bar),
                              x + 5,
                              0,
                              42,
                              rulerHeight,
                              juce::Justification::centredLeft);
        }

        const auto quarterBeatStep = 4.0
            / static_cast<double>(position.meter.denominator);
        auto nextSeconds = project->secondsAtBeat(
            project->beatsAt(seconds) + quarterBeatStep);
        for (const auto& meterChange : project->meterChanges)
        {
            if (meterChange.timeSeconds > seconds + 0.0001
                && meterChange.timeSeconds < nextSeconds - 0.0001)
            {
                nextSeconds = meterChange.timeSeconds;
                break;
            }
        }
        if (nextSeconds <= seconds + 0.000001)
            break;
        seconds = nextSeconds;
    }

    for (const auto& tempoChange : project->tempoChanges)
    {
        const auto x = static_cast<int>(secondsToX(tempoChange.timeSeconds));
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.fillRect(x - 1, 0, 3, 5);
        graphics.setFont(9.0f);
        graphics.drawText(juce::String(tempoChange.bpm, 1)
                              + (tempoChange.rampToNext ? " R" : ""),
                          x + 4,
                          1,
                          52,
                          12,
                          juce::Justification::centredLeft);
    }
    for (const auto& group : project->editGroups)
    {
        if (!group.enabled)
            continue;
        for (const auto anchor : group.protectedAnchorsSeconds)
        {
            const auto x = secondsToX(anchor);
            graphics.setColour(juce::Colour(StudioColours::violet).withAlpha(0.55f));
            graphics.drawVerticalLine(static_cast<int>(x),
                                      static_cast<float>(rulerHeight),
                                      static_cast<float>(getHeight()));
        }
    }
    for (const auto& meterChange : project->meterChanges)
    {
        const auto x = static_cast<int>(secondsToX(meterChange.timeSeconds));
        graphics.setColour(juce::Colour(StudioColours::green));
        graphics.fillRect(x - 1, 16, 3, 5);
        graphics.setFont(9.0f);
        graphics.drawText(juce::String(meterChange.numerator)
                              + "/"
                              + juce::String(meterChange.denominator),
                          x + 4,
                          15,
                          42,
                          12,
                          juce::Justification::centredLeft);
    }

    const auto tracks = visibleTracks();
    for (std::size_t index = 0; index < tracks.size(); ++index)
    {
        const auto& track = *tracks[index];
        const auto y = rulerHeight + static_cast<int>(index) * trackHeight;
        const auto selected = track.id == selectedTrackId;

        graphics.setColour(juce::Colour(selected ? 0xff22272b : 0xff15181b));
        graphics.fillRect(viewportPositionX, y, trackHeaderWidth, trackHeight);
        graphics.setColour(juce::Colour(StudioColours::border));
        graphics.drawHorizontalLine(y + trackHeight - 1, 0.0f, static_cast<float>(getWidth()));
        graphics.drawVerticalLine(viewportPositionX + trackHeaderWidth - 1,
                                  static_cast<float>(y),
                                  static_cast<float>(y + trackHeight));

        const auto childTrack = track.parentTrackId.isNotEmpty();
        const auto children = static_cast<int>(std::count_if(project->tracks.cbegin(),
                                                             project->tracks.cend(),
                                                             [&track](const auto& candidate)
        {
            return candidate.parentTrackId == track.id;
        }));
        if (children > 0)
        {
            juce::Path disclosure;
            if (track.versionsCollapsed)
                disclosure.addTriangle(static_cast<float>(viewportPositionX + 7),
                                       static_cast<float>(y + 21),
                                       static_cast<float>(viewportPositionX + 7),
                                       static_cast<float>(y + 31),
                                       static_cast<float>(viewportPositionX + 14),
                                       static_cast<float>(y + 26));
            else
                disclosure.addTriangle(static_cast<float>(viewportPositionX + 5),
                                       static_cast<float>(y + 22),
                                       static_cast<float>(viewportPositionX + 15),
                                       static_cast<float>(y + 22),
                                       static_cast<float>(viewportPositionX + 10),
                                       static_cast<float>(y + 30));
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.fillPath(disclosure);
        }

        const auto indent = childTrack ? 16 : children > 0 ? 8 : 0;
        graphics.setColour(track.colour);
        graphics.fillRect(viewportPositionX + 12 + indent, y + 17, 4, 42);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(14.0f);
        graphics.drawText(track.name,
                          viewportPositionX + 26 + indent,
                          y + 12,
                          trackHeaderWidth - 38 - indent,
                          24,
                          juce::Justification::centredLeft);

        const auto drawControl = [&graphics, y, this](int x,
                                                const juce::String& label,
                                                bool active,
                                                juce::Colour activeColour)
        {
            const juce::Rectangle<float> bounds(static_cast<float>(viewportPositionX + x),
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
        auto routingLabel = track.type == TrackType::audio
            ? "IN " + juce::String(track.inputChannel + 1)
                + (track.stereoInput ? "+" + juce::String(track.inputChannel + 2) : juce::String())
            : trackTypeToString(track.type).toUpperCase();
        if (track.type == TrackType::bus)
        {
            const auto* output = project->findTrack(
                project->resolvedOutputTrackId(track));
            routingLabel = output != nullptr
                ? "TO " + output->name.substring(0, 5).toUpperCase()
                : "NO OUT";
        }
        if (track.automationArmed)
            routingLabel << " A";
        graphics.drawText(routingLabel,
                          viewportPositionX + 128,
                          y + 48,
                          40,
                          24,
                          juce::Justification::centredRight);

        if (children > 0)
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
            graphics.drawText(juce::String(children) + " TAKES",
                              viewportPositionX + 128,
                              y + 12,
                              40,
                              24,
                              juce::Justification::centredRight);
        }

        if (childTrack
            && project->activeTakeTrackId(track.parentTrackId) == track.id)
        {
            graphics.setColour(juce::Colour(StudioColours::green));
            graphics.setFont(juce::Font(juce::FontOptions(8.5f,
                                                         juce::Font::bold)));
            graphics.drawText("ACTIVE",
                              viewportPositionX + 112,
                              y + 51,
                              52,
                              18,
                              juce::Justification::centredRight);
        }
        if (!childTrack)
        {
            if (const auto* group = project->editGroupForTrack(track.id))
            {
                graphics.setColour(juce::Colour(group->enabled
                                                    ? StudioColours::violet
                                                    : StudioColours::secondaryText));
                graphics.setFont(juce::Font(juce::FontOptions(8.5f,
                                                              juce::Font::bold)));
                graphics.drawText(group->timingReferenceTrackId == track.id
                                      ? "LINK REF"
                                      : "LINK",
                                  viewportPositionX + 104,
                                  y + 51,
                                  60,
                                  18,
                                  juce::Justification::centredRight);
            }
            if (project->reampRouteForReturn(track.id) != nullptr)
            {
                graphics.setColour(juce::Colour(StudioColours::amber));
                graphics.setFont(juce::Font(juce::FontOptions(8.0f,
                                                              juce::Font::bold)));
                graphics.drawText("TONE RETURN",
                                  viewportPositionX + 88,
                                  y + 67,
                                  76,
                                  15,
                                  juce::Justification::centredRight);
            }
            else if (std::any_of(project->reampRoutes.cbegin(),
                                 project->reampRoutes.cend(),
                                 [&track](const auto& route)
                                 {
                                     return route.sourceTrackId == track.id;
                                 }))
            {
                graphics.setColour(juce::Colour(StudioColours::amber));
                graphics.setFont(juce::Font(juce::FontOptions(8.0f,
                                                              juce::Font::bold)));
                graphics.drawText("DI SOURCE",
                                  viewportPositionX + 96,
                                  y + 67,
                                  68,
                                  15,
                                  juce::Justification::centredRight);
            }
        }
    }

    const auto addTrackY = rulerHeight + static_cast<int>(tracks.size()) * trackHeight;
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(viewportPositionX, addTrackY, trackHeaderWidth, addTrackHeight);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawRect(viewportPositionX + 10,
                      addTrackY + 7,
                      trackHeaderWidth - 20,
                      addTrackHeight - 14,
                      1);
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    graphics.drawText("+ ADD AUDIO TRACK",
                      viewportPositionX + 14,
                      addTrackY + 7,
                      trackHeaderWidth - 28,
                      addTrackHeight - 14,
                      juce::Justification::centred);

    graphics.saveState();
    graphics.excludeClipRegion({
        viewportPositionX,
        0,
        trackHeaderWidth,
        getHeight()
    });

    for (std::size_t visibleIndex = 0; visibleIndex < tracks.size(); ++visibleIndex)
    {
        const auto& parent = *tracks[visibleIndex];
        if (parent.parentTrackId.isNotEmpty())
            continue;

        const auto parentY = rulerHeight + static_cast<int>(visibleIndex) * trackHeight + 12;
        for (const auto& child : project->tracks)
        {
            if (child.parentTrackId != parent.id)
                continue;

            for (const auto& clip : child.clips)
            {
                const juce::Rectangle<float> aggregate(
                    secondsToX(clip.startSeconds),
                    static_cast<float>(parentY),
                    static_cast<float>(std::max(20.0, clip.durationSeconds * pixelsPerSecond)),
                    trackHeight - 24.0f);
                graphics.setColour(parent.colour.withAlpha(0.12f));
                graphics.fillRoundedRectangle(aggregate, 5.0f);
                drawClipWaveform(graphics, clip, aggregate, 0.13f);
                graphics.setColour(parent.colour.withAlpha(0.28f));
                graphics.drawRoundedRectangle(aggregate, 5.0f, 1.0f);
            }
        }

        for (const auto& region : parent.compRegions)
        {
            const auto* source = project->findTrack(region.sourceTrackId);
            const juce::Rectangle<float> compBounds(
                secondsToX(region.startSeconds),
                static_cast<float>(parentY),
                static_cast<float>(std::max(20.0,
                                            region.durationSeconds
                                                * pixelsPerSecond)),
                trackHeight - 24.0f);
            const auto colour = source != nullptr ? source->colour : parent.colour;
            graphics.setColour(colour.withAlpha(0.42f));
            graphics.fillRoundedRectangle(compBounds, 5.0f);
            graphics.setColour(juce::Colours::white.withAlpha(0.55f));
            graphics.drawRoundedRectangle(compBounds, 5.0f, 1.5f);
            graphics.setFont(juce::Font(juce::FontOptions(9.0f,
                                                          juce::Font::bold)));
            graphics.drawText("COMP "
                                  + (source != nullptr ? source->name
                                                       : juce::String("MISSING")),
                              compBounds.toNearestInt().reduced(6, 2),
                              juce::Justification::bottomLeft);
        }
    }

    for (const auto& hit : clipHits())
    {
        const auto* clip = project->findClip(hit.clipId);
        if (clip == nullptr)
            continue;

        const auto recoverableStart = clip->recoverableStartSeconds();
        const auto recoverableEnd = clip->recoverableEndSeconds();
        if (recoverableStart < clip->startSeconds - 0.0001
            || recoverableEnd > clip->endSeconds() + 0.0001)
        {
            const juce::Rectangle<float> ghostBounds(
                secondsToX(recoverableStart),
                hit.bounds.getY(),
                static_cast<float>(std::max(20.0,
                                            (recoverableEnd - recoverableStart)
                                                * pixelsPerSecond)),
                hit.bounds.getHeight());
            auto recoverableClip = *clip;
            recoverableClip.startSeconds = recoverableStart;
            recoverableClip.sourceOffsetSeconds = clip->sourceRangeStartSeconds;
            recoverableClip.durationSeconds = clip->sourceRangeEnd()
                - clip->sourceRangeStartSeconds;

            graphics.setColour(clip->colour.withAlpha(0.10f));
            graphics.fillRoundedRectangle(ghostBounds, 5.0f);
            drawClipWaveform(graphics, recoverableClip, ghostBounds, 0.12f);
            graphics.setColour(juce::Colours::white.withAlpha(
                hit.clipId == selectedClipId ? 0.30f : 0.18f));
            const float dashLengths[] { 4.0f, 4.0f };
            juce::Path ghostShape;
            ghostShape.addRoundedRectangle(ghostBounds, 5.0f);
            juce::Path dashedGhost;
            juce::PathStrokeType(1.0f).createDashedStroke(dashedGhost,
                                                          ghostShape,
                                                          dashLengths,
                                                          2);
            graphics.fillPath(dashedGhost);
        }

        auto bounds = hit.bounds;
        if (draggedClipId == hit.clipId)
        {
            graphics.setColour(clip->colour.withAlpha(0.16f));
            graphics.fillRoundedRectangle(hit.bounds, 5.0f);
            drawClipWaveform(graphics, *clip, hit.bounds, 0.12f);
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
        const auto controlsVisible = selected
            || hit.clipId == hoveredClipId
            || hit.clipId == draggedClipId;
        const auto gainDecibels = draggedClipId == hit.clipId
                && dragMode == DragMode::gain
            ? dragPreviewGainDecibels
            : clip->gainDecibels;
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

        drawClipWaveform(graphics, *clip, bounds, 0.28f);

        graphics.setColour(juce::Colour(StudioColours::green).withAlpha(0.55f));
        for (const auto transient : clip->transientSourceSeconds)
        {
            const auto offset = clip->timelineOffsetForSourceSeconds(transient);
            const auto x = bounds.getX()
                + static_cast<float>(offset * pixelsPerSecond);
            if (x >= bounds.getX() && x <= bounds.getRight())
                graphics.drawVerticalLine(static_cast<int>(x),
                                          bounds.getY() + 6.0f,
                                          bounds.getBottom() - 6.0f);
        }
        graphics.setColour(juce::Colour(StudioColours::orange));
        for (const auto& marker : clip->warpMarkers)
        {
            const auto x = bounds.getX()
                + static_cast<float>(marker.timelineOffsetSeconds
                                     * pixelsPerSecond);
            juce::Path markerShape;
            markerShape.addTriangle(x - 4.0f,
                                    bounds.getBottom() - 3.0f,
                                    x + 4.0f,
                                    bounds.getBottom() - 3.0f,
                                    x,
                                    bounds.getBottom() - 10.0f);
            graphics.fillPath(markerShape);
        }
        const auto editingFadeIn = draggedClipId == hit.clipId
            && (dragMode == DragMode::fadeIn
                || dragMode == DragMode::fadeInCurve);
        const auto editingFadeOut = draggedClipId == hit.clipId
            && (dragMode == DragMode::fadeOut
                || dragMode == DragMode::fadeOutCurve);
        const auto fadeInSeconds = editingFadeIn
            ? dragPreviewFadeSeconds
            : clip->fadeInSeconds;
        const auto fadeOutSeconds = editingFadeOut
            ? dragPreviewFadeSeconds
            : clip->fadeOutSeconds;
        const auto fadeInCurve = editingFadeIn
            ? dragPreviewFadeCurve
            : clip->fadeInCurve;
        const auto fadeOutCurve = editingFadeOut
            ? dragPreviewFadeCurve
            : clip->fadeOutCurve;
        const auto drawFade = [&graphics,
                               bounds,
                               this](
                                  bool fadeIn,
                                  double fadeSeconds,
                                  float curve)
        {
            if (fadeSeconds <= 0.0)
                return;
            const auto width = std::min(
                bounds.getWidth(),
                static_cast<float>(
                    fadeSeconds * pixelsPerSecond));
            juce::Path path;
            constexpr auto steps = 24;
            for (int step = 0; step <= steps; ++step)
            {
                const auto progress = static_cast<float>(step)
                    / static_cast<float>(steps);
                const auto x = fadeIn
                    ? bounds.getX() + width * progress
                    : bounds.getRight() - width
                        + width * progress;
                const auto amplitude = fadeIn
                    ? fadeCurveValue(progress, curve)
                    : fadeCurveValue(1.0f - progress, curve);
                const auto y = bounds.getBottom() - 4.0f
                    - amplitude * (bounds.getHeight() - 8.0f);
                if (step == 0)
                    path.startNewSubPath(x, y);
                else
                    path.lineTo(x, y);
            }
            graphics.strokePath(path, juce::PathStrokeType(1.0f));
        };
        graphics.setColour(juce::Colours::white.withAlpha(0.52f));
        if (fadeInSeconds > 0.0)
            drawFade(true,
                     fadeInSeconds,
                     fadeInCurve);
        if (fadeOutSeconds > 0.0)
        {
            drawFade(false,
                     fadeOutSeconds,
                     fadeOutCurve);
        }

        if (controlsVisible)
        {
            const auto highlighted = [this, &hit](DragMode mode)
            {
                return hit.clipId == hoveredClipId
                    && hoveredDragMode == mode;
            };
            const auto controlColour = [&highlighted](DragMode mode)
            {
                return highlighted(mode)
                    ? juce::Colour(StudioColours::orange)
                    : juce::Colours::white.withAlpha(0.88f);
            };

            const auto gainY = bounds.getY() + 4.5f;
            graphics.setColour(controlColour(DragMode::gain));
            if (bounds.getWidth() > 30.0f)
                graphics.drawHorizontalLine(
                    static_cast<int>(std::round(gainY)),
                    bounds.getX() + 14.0f,
                    bounds.getRight() - 14.0f);
            graphics.fillRoundedRectangle(
                bounds.getCentreX() - 11.0f,
                gainY - 3.0f,
                22.0f,
                6.0f,
                3.0f);

            const auto drawLengthHandle = [&graphics,
                                           &controlColour,
                                           this,
                                           bounds](bool fadeIn,
                                                   double fadeSeconds)
            {
                const auto mode = fadeIn ? DragMode::fadeIn
                                         : DragMode::fadeOut;
                const auto point = fadeLengthHandle(
                    bounds,
                    fadeIn,
                    fadeSeconds);
                graphics.setColour(controlColour(mode));
                graphics.fillEllipse(point.x - 4.0f,
                                     point.y - 4.0f,
                                     8.0f,
                                     8.0f);
            };
            drawLengthHandle(true, fadeInSeconds);
            drawLengthHandle(false, fadeOutSeconds);

            const auto drawCurveHandle = [&graphics,
                                          &controlColour,
                                          clip,
                                          this,
                                          bounds](bool fadeIn,
                                                  double fadeSeconds,
                                                  float curve)
            {
                if (fadeSeconds <= 0.0)
                    return;
                const auto mode = fadeIn ? DragMode::fadeInCurve
                                         : DragMode::fadeOutCurve;
                const auto point = fadeCurveHandle(
                    bounds,
                    fadeIn,
                    fadeSeconds,
                    curve);
                graphics.setColour(controlColour(mode));
                graphics.fillEllipse(point.x - 3.5f,
                                     point.y - 3.5f,
                                     7.0f,
                                     7.0f);
                graphics.setColour(clip->colour.withAlpha(0.9f));
                graphics.fillEllipse(point.x - 1.5f,
                                     point.y - 1.5f,
                                     3.0f,
                                     3.0f);
            };
            drawCurveHandle(true, fadeInSeconds, fadeInCurve);
            drawCurveHandle(false, fadeOutSeconds, fadeOutCurve);
        }

        graphics.setColour(juce::Colours::white);
        graphics.setFont(12.5f);
        const auto processingLabel = (clip->reversed ? " REV" : "")
            + juce::String(clip->polarityInverted ? " INV" : "");
        const auto gainLabel = std::abs(gainDecibels) >= 0.05f
            ? "  " + juce::String(gainDecibels, 1) + " dB"
            : juce::String();
        graphics.drawText(clip->name + processingLabel + gainLabel,
                          bounds.toNearestInt().withHeight(24).reduced(8, 0),
                          juce::Justification::centredLeft);
    }

    for (const auto& lane : project->automationLanes)
    {
        if (!lane.enabled || lane.points.empty())
            continue;
        const auto track = std::find_if(
            tracks.cbegin(),
            tracks.cend(),
            [&lane](const auto* candidate)
            {
                return candidate->id == lane.target.trackId;
            });
        if (track == tracks.cend())
            continue;
        const auto index = static_cast<int>(
            std::distance(tracks.cbegin(), track));
        const auto top = static_cast<float>(
            rulerHeight + index * trackHeight + 8);
        const auto height = static_cast<float>(trackHeight - 16);
        juce::Path path;
        auto started = false;
        graphics.setColour(
            juce::Colour(StudioColours::amber).withAlpha(0.8f));
        for (const auto& point : lane.points)
        {
            const auto pointSeconds =
                lane.timebase == AutomationTimebase::beats
                ? project->secondsAtBeat(point.position)
                : point.position;
            const auto x = secondsToX(pointSeconds);
            const auto y = top
                + (1.0f
                   - static_cast<float>(
                       juce::jlimit(0.0, 1.0, point.value)))
                    * height;
            if (!started)
            {
                path.startNewSubPath(x, y);
                started = true;
            }
            else if (lane.interpolation == AutomationInterpolation::step)
            {
                const auto previous = path.getCurrentPosition();
                path.lineTo(x, previous.y);
                path.lineTo(x, y);
            }
            else
            {
                path.lineTo(x, y);
            }
            graphics.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
        }
        graphics.strokePath(path, juce::PathStrokeType(1.5f));
    }

    const auto visible = visibleTracks();
    for (const auto& recordingPreview : recordingPreviews)
    {
        const auto iterator = std::find_if(visible.cbegin(),
                                           visible.cend(),
                                           [&recordingPreview](const auto* track)
        {
            return track->id == recordingPreview.trackId;
        });
        if (iterator != visible.cend())
        {
            const auto index = static_cast<int>(std::distance(visible.cbegin(), iterator));
            const auto y = rulerHeight + index * trackHeight + 12;
            const juce::Rectangle<float> preview(
                secondsToX(recordingPreview.startSeconds),
                static_cast<float>(y),
                static_cast<float>(std::max(96.0,
                                            recordingPreview.durationSeconds * pixelsPerSecond)),
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

            if (!recordingPreview.waveformPeaks.empty())
            {
                const auto columns = juce::jmax(1,
                                                juce::jmin(
                                                    static_cast<int>(
                                                        recordingPreview.waveformPeaks.size()),
                                                           static_cast<int>(preview.getWidth()) - 12));
                const auto maximumHeight = preview.getHeight() * 0.32f;
                graphics.setColour(juce::Colours::white.withAlpha(0.62f));
                for (int column = 0; column < columns; ++column)
                {
                    const auto peakIndex = static_cast<std::size_t>(
                        static_cast<double>(column)
                        / static_cast<double>(columns)
                        * static_cast<double>(
                            recordingPreview.waveformPeaks.size()));
                    const auto peak = std::sqrt(juce::jlimit(
                        0.0f,
                        1.0f,
                        recordingPreview.waveformPeaks[std::min(
                           peakIndex,
                           recordingPreview.waveformPeaks.size() - 1)]));
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
            graphics.drawText("RECORDING  "
                                 + juce::String(recordingPreview.durationSeconds, 1)
                                 + " s",
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
    graphics.restoreState();
}

void TimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        showContextMenu(event);
        return;
    }

    const auto inTrackHeader = event.position.x >= static_cast<float>(viewportPositionX)
        && event.position.x < static_cast<float>(viewportPositionX + trackHeaderWidth);
    if (project != nullptr && inTrackHeader)
    {
        const auto tracks = visibleTracks();
        const auto addTrackY = rulerHeight + static_cast<int>(tracks.size()) * trackHeight;
        if (event.position.y >= static_cast<float>(addTrackY)
            && event.position.y < static_cast<float>(addTrackY + addTrackHeight))
        {
            if (onAddTrack)
                onAddTrack();
            return;
        }

        const auto trackIndex = trackIndexAt(event.position.y);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
        {
            const auto& track = *tracks[static_cast<std::size_t>(trackIndex)];
            const auto localY = static_cast<int>(event.position.y)
                - rulerHeight
                - trackIndex * trackHeight;
            const auto x = static_cast<int>(event.position.x) - viewportPositionX;
            const auto hasVersions = std::any_of(project->tracks.cbegin(),
                                                 project->tracks.cend(),
                                                 [&track](const auto& candidate)
            {
                return candidate.parentTrackId == track.id;
            });
            if (x < 22 && hasVersions && onToggleTrackVersions)
            {
                onToggleTrackVersions(track.id);
                return;
            }
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

            selectedTrackId = track.id;
            selectedClipId.clear();
            if (onTrackSelected)
                onTrackSelected(track.id);
            repaint();
            return;
        }
    }

    const auto hits = clipHits();
    for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator)
    {
        const auto& hit = *iterator;
        if (!hit.bounds.contains(event.position))
            continue;

        selectedTrackId = hit.trackId;
        selectedClipId = hit.clipId;
        draggedClipId = hit.clipId;
        dragOriginalTrackId = hit.trackId;
        dragPreviewTrackId = hit.trackId;
        dragStartX = event.position.x;
        dragStartY = event.position.y;
        if (const auto* clip = project != nullptr ? project->findClip(hit.clipId) : nullptr)
        {
            dragOriginalStart = clip->startSeconds;
            dragOriginalSourceOffset = clip->sourceOffsetSeconds;
            dragOriginalDuration = clip->durationSeconds;
            dragPreviewStart = dragOriginalStart;
            dragPreviewSourceOffset = dragOriginalSourceOffset;
            dragPreviewDuration = dragOriginalDuration;
            dragOriginalGainDecibels = clip->gainDecibels;
            dragPreviewGainDecibels = clip->gainDecibels;
            dragMode = dragModeAt(hit, *clip, event.position);
            if (dragMode == DragMode::fadeIn
                || dragMode == DragMode::fadeInCurve)
            {
                dragOriginalFadeSeconds = clip->fadeInSeconds;
                dragPreviewFadeSeconds = clip->fadeInSeconds;
                dragMaximumFadeSeconds = clip->durationSeconds;
                dragOriginalFadeCurve = clip->fadeInCurve;
                dragPreviewFadeCurve = clip->fadeInCurve;
            }
            else if (dragMode == DragMode::fadeOut
                     || dragMode == DragMode::fadeOutCurve)
            {
                dragOriginalFadeSeconds = clip->fadeOutSeconds;
                dragPreviewFadeSeconds = clip->fadeOutSeconds;
                dragMaximumFadeSeconds = clip->durationSeconds;
                dragOriginalFadeCurve = clip->fadeOutCurve;
                dragPreviewFadeCurve = clip->fadeOutCurve;
            }
        }

        if (onClipSelected)
            onClipSelected(hit.trackId, hit.clipId);
        if (onSeek
            && dragMode != DragMode::gain
            && dragMode != DragMode::fadeIn
            && dragMode != DragMode::fadeOut
            && dragMode != DragMode::fadeInCurve
            && dragMode != DragMode::fadeOutCurve)
            onSeek(xToSeconds(event.position.x));
        repaint();
        return;
    }

    const auto trackIndex = trackIndexAt(event.position.y);
    const auto tracks = visibleTracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
    {
        const auto clickedTrackId = tracks[static_cast<std::size_t>(trackIndex)]->id;
        if (clickedTrackId != selectedTrackId)
        {
            selectedTrackId = clickedTrackId;
            selectedClipId.clear();
            if (onTrackSelected)
                onTrackSelected(selectedTrackId);
        }
    }

    if (!inTrackHeader && onSeek)
        onSeek(xToSeconds(event.position.x));
    repaint();
}

void TimelineComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (project == nullptr)
        return;
    const auto inTrackHeader = event.position.x >= static_cast<float>(viewportPositionX)
        && event.position.x < static_cast<float>(viewportPositionX + trackHeaderWidth);
    if (!inTrackHeader)
        return;

    const auto tracks = visibleTracks();
    const auto trackIndex = trackIndexAt(event.position.y);
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    const auto localY = static_cast<int>(event.position.y)
        - rulerHeight
        - trackIndex * trackHeight;
    const auto localX = static_cast<int>(event.position.x) - viewportPositionX;
    if (localY > 42 || localX < 22)
        return;

    const auto& track = *tracks[static_cast<std::size_t>(trackIndex)];
    selectedTrackId = track.id;
    selectedClipId.clear();
    if (onTrackSelected)
        onTrackSelected(track.id);
    if (onEditTrack)
    {
        const auto indent = track.parentTrackId.isNotEmpty() ? 16 : 0;
        const juce::Rectangle<int> nameBounds(
            viewportPositionX + 24 + indent,
            rulerHeight + trackIndex * trackHeight + 8,
            trackHeaderWidth - 34 - indent,
            32);
        onEditTrack(track.id, localAreaToGlobal(nameBounds));
    }
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
            && targetTrackIndex < static_cast<int>(visibleTracks().size()))
        {
            const auto tracks = visibleTracks();
            const auto& targetTrack = *tracks[static_cast<std::size_t>(targetTrackIndex)];
            if (targetTrack.type == TrackType::audio)
                dragPreviewTrackId = targetTrack.id;
        }
    }
    else if (dragMode == DragMode::trimStart)
    {
        const auto* clip = project != nullptr ? project->findClip(draggedClipId) : nullptr;
        const auto minimumStart = std::max(
            0.0,
            clip != nullptr
                ? clip->recoverableStartSeconds()
                : dragOriginalStart - dragOriginalSourceOffset);
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
        const auto* clip = project != nullptr ? project->findClip(draggedClipId) : nullptr;
        const auto sourceRemaining = clip != nullptr
            ? clip->sourceRangeEnd() - dragOriginalSourceOffset
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
    else if (dragMode == DragMode::gain)
    {
        dragPreviewGainDecibels = juce::jlimit(
            -60.0f,
            24.0f,
            dragOriginalGainDecibels
                + (dragStartY - event.position.y) * 0.25f);
    }
    else if (dragMode == DragMode::fadeIn
             || dragMode == DragMode::fadeOut)
    {
        const auto hits = clipHits();
        const auto hit = std::find_if(
            hits.cbegin(),
            hits.cend(),
            [this](const auto& candidate)
            {
                return candidate.clipId == draggedClipId;
            });
        if (hit != hits.cend())
        {
            const auto requestedSeconds = dragMode == DragMode::fadeIn
                ? static_cast<double>(
                      event.position.x - hit->bounds.getX())
                    / pixelsPerSecond
                : static_cast<double>(
                      hit->bounds.getRight() - event.position.x)
                    / pixelsPerSecond;
            dragPreviewFadeSeconds = juce::jlimit(
                0.0,
                dragMaximumFadeSeconds,
                requestedSeconds);
        }
    }
    else if (dragMode == DragMode::fadeInCurve
             || dragMode == DragMode::fadeOutCurve)
    {
        dragPreviewFadeCurve = juce::jlimit(
            -1.0f,
            1.0f,
            dragOriginalFadeCurve
                + (event.position.y - dragStartY) / 40.0f);
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
        else if (dragMode == DragMode::gain
                 && std::abs(
                        dragPreviewGainDecibels
                        - dragOriginalGainDecibels)
                     > 0.001f
                 && onClipGainChanged)
            onClipGainChanged(
                draggedClipId,
                dragPreviewGainDecibels);
        else if ((dragMode == DragMode::fadeIn
                  || dragMode == DragMode::fadeOut
                  || dragMode == DragMode::fadeInCurve
                  || dragMode == DragMode::fadeOutCurve)
                 && (std::abs(
                         dragPreviewFadeSeconds
                         - dragOriginalFadeSeconds)
                         > 0.0001
                     || std::abs(
                            dragPreviewFadeCurve
                            - dragOriginalFadeCurve)
                         > 0.001f)
                 && onClipFadeChanged)
            onClipFadeChanged(
                draggedClipId,
                dragMode == DragMode::fadeIn
                    || dragMode == DragMode::fadeInCurve,
                dragPreviewFadeSeconds,
                dragPreviewFadeCurve);
    }

    draggedClipId.clear();
    dragOriginalTrackId.clear();
    dragPreviewTrackId.clear();
    dragMode = DragMode::none;
    dragMaximumFadeSeconds = 0.0;
    repaint();
}

void TimelineComponent::mouseMove(const juce::MouseEvent& event)
{
    updateHoverState(event.position);
}

void TimelineComponent::mouseExit(const juce::MouseEvent&)
{
    if (draggedClipId.isNotEmpty())
        return;

    const auto changed = hoveredClipId.isNotEmpty()
        || hoveredDragMode != DragMode::none;
    hoveredClipId.clear();
    hoveredDragMode = DragMode::none;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    if (changed)
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

void TimelineComponent::showContextMenu(const juce::MouseEvent& event)
{
    if (project == nullptr)
        return;

    const auto tracks = visibleTracks();
    const auto trackIndex = trackIndexAt(event.position.y);
    const auto* clickedTrack = trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
        ? tracks[static_cast<std::size_t>(trackIndex)]
        : nullptr;

    const auto inTrackHeader = event.position.x >= static_cast<float>(viewportPositionX)
        && event.position.x < static_cast<float>(viewportPositionX + trackHeaderWidth);
    if (!inTrackHeader && onSeek)
        onSeek(xToSeconds(event.position.x));

    auto hitClip = false;
    const auto hits = clipHits();
    for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator)
    {
        const auto& hit = *iterator;
        if (!hit.bounds.contains(event.position))
            continue;

        hitClip = true;
        selectedTrackId = hit.trackId;
        selectedClipId = hit.clipId;
        if (onClipSelected)
            onClipSelected(hit.trackId, hit.clipId);
        break;
    }

    if (!hitClip)
    {
        if (clickedTrack != nullptr)
        {
            if (clickedTrack->id != selectedTrackId)
            {
                selectedTrackId = clickedTrack->id;
                selectedClipId.clear();
                if (onTrackSelected)
                    onTrackSelected(clickedTrack->id);
            }
        }
    }

    juce::PopupMenu menu;
    if (inTrackHeader && clickedTrack != nullptr)
    {
        const auto trackId = clickedTrack->id;

        juce::PopupMenu::Item mute(clickedTrack->muted ? "Unmute track" : "Mute track");
        mute.action = [this, trackId]
        {
            if (onTrackMute)
                onTrackMute(trackId);
        };
        menu.addItem(std::move(mute));

        juce::PopupMenu::Item solo(clickedTrack->solo ? "Unsolo track" : "Solo track");
        solo.action = [this, trackId]
        {
            if (onTrackSolo)
                onTrackSolo(trackId);
        };
        menu.addItem(std::move(solo));

        if (clickedTrack->type == TrackType::audio)
        {
            juce::PopupMenu::Item arm(clickedTrack->armed ? "Disarm track" : "Arm track");
            arm.action = [this, trackId]
            {
                if (onTrackArm)
                    onTrackArm(trackId);
            };
            menu.addItem(std::move(arm));
        }

        juce::PopupMenu::Item editTrack("Edit name and color...");
        editTrack.action = [this, trackId]
        {
            if (onEditTrack)
            {
                const auto visible = visibleTracks();
                const auto iterator = std::find_if(
                    visible.cbegin(),
                    visible.cend(),
                    [&trackId](const auto* candidate)
                    {
                        return candidate->id == trackId;
                    });
                if (iterator == visible.cend())
                    return;
                const auto index = static_cast<int>(
                    std::distance(visible.cbegin(), iterator));
                const juce::Rectangle<int> nameBounds(
                    viewportPositionX + 24,
                    rulerHeight + index * trackHeight + 8,
                    trackHeaderWidth - 34,
                    32);
                onEditTrack(trackId, localAreaToGlobal(nameBounds));
            }
        };
        menu.addItem(std::move(editTrack));

        const auto hasVersions = std::any_of(project->tracks.cbegin(),
                                             project->tracks.cend(),
                                             [clickedTrack](const auto& candidate)
        {
            return candidate.parentTrackId == clickedTrack->id;
        });
        if (hasVersions)
        {
            juce::PopupMenu::Item collapse(clickedTrack->versionsCollapsed
                                                ? "Expand versions"
                                                : "Collapse versions");
            collapse.action = [this, trackId]
            {
                if (onToggleTrackVersions)
                    onToggleTrackVersions(trackId);
            };
            menu.addItem(std::move(collapse));
        }

        menu.addSeparator();
        juce::PopupMenu::Item duplicateTrack("Duplicate track");
        duplicateTrack.isEnabled = clickedTrack->type != TrackType::master;
        duplicateTrack.action = [this, trackId]
        {
            if (onDuplicateTrack)
                onDuplicateTrack(trackId);
        };
        menu.addItem(std::move(duplicateTrack));

        juce::PopupMenu::Item deleteTrack("Delete track");
        deleteTrack.shortcutKeyDescription = "Delete";
        deleteTrack.isEnabled = clickedTrack->type != TrackType::master;
        deleteTrack.action = [this, trackId]
        {
            if (onDeleteTrack)
                onDeleteTrack(trackId);
        };
        menu.addItem(std::move(deleteTrack));
    }
    else
    {
        const auto hasClip = selectedClipId.isNotEmpty();

        juce::PopupMenu::Item trimStart("Trim start to playhead");
        trimStart.shortcutKeyDescription = "[";
        trimStart.isEnabled = hasClip;
        trimStart.action = [this]
        {
            if (onTrimStartSelected)
                onTrimStartSelected();
        };
        menu.addItem(std::move(trimStart));

        juce::PopupMenu::Item split("Split at playhead");
        split.shortcutKeyDescription = "S";
        split.isEnabled = hasClip;
        split.action = [this]
        {
            if (onSplitSelected)
                onSplitSelected();
        };
        menu.addItem(std::move(split));

        juce::PopupMenu::Item trimEnd("Trim end to playhead");
        trimEnd.shortcutKeyDescription = "]";
        trimEnd.isEnabled = hasClip;
        trimEnd.action = [this]
        {
            if (onTrimEndSelected)
                onTrimEndSelected();
        };
        menu.addItem(std::move(trimEnd));

        juce::PopupMenu::Item duplicate("Duplicate clip");
        duplicate.shortcutKeyDescription = "Command/Ctrl+C, Command/Ctrl+V";
        duplicate.isEnabled = hasClip;
        duplicate.action = [this]
        {
            if (onDuplicateSelected)
                onDuplicateSelected();
        };
        menu.addItem(std::move(duplicate));

        menu.addSeparator();
        const auto* selectedTrack = project->findTrack(selectedTrackId);
        if (hasClip
            && selectedTrack != nullptr
            && selectedTrack->parentTrackId.isNotEmpty())
        {
            const auto takeId = selectedTrack->id;
            const auto parentId = selectedTrack->parentTrackId;
            juce::PopupMenu::Item useTake("Use this take as playlist");
            useTake.isTicked = project->activeTakeTrackId(parentId) == takeId;
            useTake.action = [this, takeId]
            {
                if (onUseTake)
                    onUseTake(takeId);
            };
            menu.addItem(std::move(useTake));

            juce::PopupMenu::Item useForComp("Use this clip range in comp");
            useForComp.action = [this, clipId = selectedClipId]
            {
                if (onUseClipForComp)
                    onUseClipForComp(clipId);
            };
            menu.addItem(std::move(useForComp));

            const auto* parent = project->findTrack(parentId);
            juce::PopupMenu::Item clearComp("Clear parent comp");
            clearComp.isEnabled = parent != nullptr
                && !parent->compRegions.empty();
            clearComp.action = [this, parentId]
            {
                if (onClearComp)
                    onClearComp(parentId);
            };
            menu.addItem(std::move(clearComp));
            menu.addSeparator();
        }

        const auto* selectedClip = project->findClip(selectedClipId);
        if (selectedClip != nullptr)
        {
            menu.addSectionHeader("Audio processing");
            menu.addItem(
                selectedClip->muted ? "Unmute clip" : "Mute clip",
                true,
                selectedClip->muted,
                [this, clipId = selectedClipId]
                {
                    if (onToggleClipMute)
                        onToggleClipMute(clipId);
                });
            menu.addItem("Detect transients", [this, clipId = selectedClipId]
            {
                if (onAnalyseTransients)
                    onAnalyseTransients(clipId);
            });

            juce::PopupMenu stretchMenu;
            const std::array stretchModes {
                std::pair { "Drums", StretchMode::drums },
                std::pair { "Monophonic", StretchMode::monophonic },
                std::pair { "Polyphonic", StretchMode::polyphonic },
                std::pair { "Full mix", StretchMode::mix }
            };
            for (const auto& [name, mode] : stretchModes)
            {
                stretchMenu.addItem(name,
                                    true,
                                    selectedClip->stretchMode == mode,
                                    [this, clipId = selectedClipId, mode]
                                    {
                                        if (onSetStretchMode)
                                            onSetStretchMode(clipId, mode);
                                    });
            }
            menu.addSubMenu("Stretch mode", stretchMenu);

            juce::PopupMenu rateMenu;
            for (const auto rate : { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 })
            {
                rateMenu.addItem(juce::String(rate, 2) + "x",
                                 true,
                                 std::abs(selectedClip->playbackRate - rate) < 0.0001,
                                 [this, clipId = selectedClipId, rate]
                                 {
                                     if (onSetPlaybackRate)
                                         onSetPlaybackRate(clipId, rate);
                                 });
            }
            menu.addSubMenu("Playback rate", rateMenu);

            menu.addItem("Warp nearest transient to playhead",
                         !selectedClip->transientSourceSeconds.empty(),
                         false,
                         [this,
                          clipId = selectedClipId,
                          timelineSeconds = xToSeconds(event.position.x)]
                         {
                             if (onWarpTransientToTimeline)
                                 onWarpTransientToTimeline(clipId,
                                                           timelineSeconds);
                         });
            menu.addItem("Fade in to playhead",
                         true,
                         false,
                         [this,
                          clipId = selectedClipId,
                          timelineSeconds = xToSeconds(event.position.x)]
                         {
                             if (onSetFadeIn)
                                 onSetFadeIn(clipId, timelineSeconds);
                         });
            menu.addItem("Fade out from playhead",
                         true,
                         false,
                         [this,
                          clipId = selectedClipId,
                          timelineSeconds = xToSeconds(event.position.x)]
                         {
                             if (onSetFadeOut)
                                 onSetFadeOut(clipId, timelineSeconds);
                         });
            menu.addItem("Create crossfade with next clip",
                         [this, clipId = selectedClipId]
                         {
                             if (onCreateCrossfade)
                                 onCreateCrossfade(clipId);
                         });
            menu.addItem("Invert polarity",
                         true,
                         selectedClip->polarityInverted,
                         [this, clipId = selectedClipId]
                         {
                             if (onToggleClipPolarity)
                                 onToggleClipPolarity(clipId);
                         });
            menu.addItem("Reverse audio",
                         true,
                         selectedClip->reversed,
                         [this, clipId = selectedClipId]
                         {
                             if (onToggleClipReverse)
                                 onToggleClipReverse(clipId);
                         });
            menu.addItem("Consolidate processed clip",
                         [this, clipId = selectedClipId]
                         {
                             if (onConsolidateClip)
                                 onConsolidateClip(clipId);
                         });
            menu.addSeparator();
        }

        juce::PopupMenu::Item remove("Delete clip");
        remove.shortcutKeyDescription = "Delete";
        remove.isEnabled = hasClip;
        remove.action = [this]
        {
            if (onDeleteSelected)
                onDeleteSelected();
        };
        menu.addItem(std::move(remove));
    }

    const auto screenPosition = event.getScreenPosition();
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(this)
                           .withTargetScreenArea({ screenPosition.x,
                                                   screenPosition.y,
                                                   1,
                                                   1 }));
    repaint();
}

std::vector<TimelineComponent::Hit> TimelineComponent::clipHits() const
{
    std::vector<Hit> hits;
    if (project == nullptr)
        return hits;

    const auto tracks = visibleTracks();
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
    {
        const auto& track = *tracks[trackIndex];
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

std::vector<const Track*> TimelineComponent::visibleTracks() const
{
    std::vector<const Track*> result;
    if (project == nullptr)
        return result;

    result.reserve(project->tracks.size());
    for (const auto& track : project->tracks)
    {
        if (track.parentTrackId.isNotEmpty())
        {
            const auto* parent = project->findTrack(track.parentTrackId);
            if (parent != nullptr && parent->versionsCollapsed)
                continue;
        }
        result.push_back(&track);
    }
    return result;
}

int TimelineComponent::trackIndexAt(float y) const noexcept
{
    return y < rulerHeight ? -1 : static_cast<int>((y - rulerHeight) / trackHeight);
}

float TimelineComponent::trackY(const juce::String& trackId) const noexcept
{
    if (project == nullptr)
        return static_cast<float>(rulerHeight);

    const auto tracks = visibleTracks();
    const auto iterator = std::find_if(tracks.cbegin(),
                                       tracks.cend(),
                                       [&trackId](const auto* track)
    {
        return track->id == trackId;
    });
    if (iterator == tracks.cend())
        return static_cast<float>(rulerHeight);

    const auto index = static_cast<int>(std::distance(tracks.cbegin(), iterator));
    return static_cast<float>(rulerHeight + index * trackHeight);
}

void TimelineComponent::drawClipWaveform(juce::Graphics& graphics,
                                         const AudioClip& clip,
                                         juce::Rectangle<float> bounds,
                                         float alpha)
{
    graphics.saveState();
    graphics.reduceClipRegion(bounds.toNearestInt().reduced(5));
    const auto middle = bounds.getCentreY() + 8.0f;
    const auto waveformWidth = std::max(1.0f, bounds.getWidth() - 12.0f);
    graphics.setColour(juce::Colours::white.withAlpha(alpha));
    for (float x = bounds.getX() + 8.0f; x < bounds.getRight() - 4.0f; x += 4.0f)
    {
        const auto progress = juce::jlimit(0.0,
                                           1.0,
                                           static_cast<double>(x - bounds.getX() - 8.0f)
                                               / waveformWidth);
        const auto sourceSeconds = clip.sourceOffsetSeconds
            + progress * clip.durationSeconds;
        const auto amplitude = waveformAmplitude(clip, sourceSeconds)
            * (bounds.getHeight() * 0.23f);
        graphics.drawVerticalLine(static_cast<int>(x),
                                  middle - amplitude,
                                  middle + amplitude);
    }
    graphics.restoreState();
}

float TimelineComponent::fadeCurveValue(float progress, float curve) noexcept
{
    progress = juce::jlimit(0.0f, 1.0f, progress);
    if (curve > 0.001f)
        return std::pow(progress, 1.0f + curve * 3.0f);
    if (curve < -0.001f)
        return 1.0f
            - std::pow(1.0f - progress,
                       1.0f - curve * 3.0f);
    return progress;
}

juce::Point<float> TimelineComponent::fadeLengthHandle(
    juce::Rectangle<float> bounds,
    bool fadeIn,
    double fadeSeconds) const noexcept
{
    const auto width = std::min(
        bounds.getWidth(),
        static_cast<float>(
            std::max(0.0, fadeSeconds) * pixelsPerSecond));
    return {
        fadeIn ? bounds.getX() + width
               : bounds.getRight() - width,
        bounds.getY() + 4.0f
    };
}

juce::Point<float> TimelineComponent::fadeCurveHandle(
    juce::Rectangle<float> bounds,
    bool fadeIn,
    double fadeSeconds,
    float curve) const noexcept
{
    const auto width = std::min(
        bounds.getWidth(),
        static_cast<float>(
            std::max(0.0, fadeSeconds) * pixelsPerSecond));
    const auto amplitude = fadeCurveValue(0.5f, curve);
    return {
        fadeIn ? bounds.getX() + width * 0.5f
               : bounds.getRight() - width * 0.5f,
        bounds.getBottom() - 4.0f
            - amplitude * (bounds.getHeight() - 8.0f)
    };
}

TimelineComponent::DragMode TimelineComponent::dragModeAt(
    const Hit& hit,
    const AudioClip& clip,
    juce::Point<float> position) const noexcept
{
    if (!hit.bounds.contains(position))
        return DragMode::none;

    constexpr auto handleRadius = 8.0f;
    const auto fadeInCurvePoint = fadeCurveHandle(
        hit.bounds,
        true,
        clip.fadeInSeconds,
        clip.fadeInCurve);
    const auto fadeOutCurvePoint = fadeCurveHandle(
        hit.bounds,
        false,
        clip.fadeOutSeconds,
        clip.fadeOutCurve);
    const auto fadeInCurveDistance = clip.fadeInSeconds > 0.0
        ? position.getDistanceFrom(fadeInCurvePoint)
        : std::numeric_limits<float>::max();
    const auto fadeOutCurveDistance = clip.fadeOutSeconds > 0.0
        ? position.getDistanceFrom(fadeOutCurvePoint)
        : std::numeric_limits<float>::max();
    if (std::min(fadeInCurveDistance, fadeOutCurveDistance)
        <= handleRadius)
    {
        if (fadeInCurvePoint.getDistanceFrom(fadeOutCurvePoint) < 0.5f)
            return position.x < fadeInCurvePoint.x
                ? DragMode::fadeInCurve
                : DragMode::fadeOutCurve;
        return fadeInCurveDistance <= fadeOutCurveDistance
            ? DragMode::fadeInCurve
            : DragMode::fadeOutCurve;
    }

    const auto fadeInPoint = fadeLengthHandle(
        hit.bounds,
        true,
        clip.fadeInSeconds);
    const auto fadeOutPoint = fadeLengthHandle(
        hit.bounds,
        false,
        clip.fadeOutSeconds);
    const auto fadeInDistance = position.getDistanceFrom(fadeInPoint);
    const auto fadeOutDistance = position.getDistanceFrom(fadeOutPoint);
    if (std::min(fadeInDistance, fadeOutDistance) <= handleRadius)
    {
        if (fadeInPoint.getDistanceFrom(fadeOutPoint) < 0.5f)
            return position.x < fadeInPoint.x
                ? DragMode::fadeIn
                : DragMode::fadeOut;
        return fadeInDistance <= fadeOutDistance
            ? DragMode::fadeIn
            : DragMode::fadeOut;
    }

    if (position.y <= hit.bounds.getY() + 12.0f)
        return DragMode::gain;
    if (position.x <= hit.bounds.getX() + 9.0f)
        return DragMode::trimStart;
    if (position.x >= hit.bounds.getRight() - 9.0f)
        return DragMode::trimEnd;
    return DragMode::move;
}

void TimelineComponent::updateHoverState(juce::Point<float> position)
{
    juce::String nextClipId;
    auto nextMode = DragMode::none;
    if (project != nullptr)
    {
        const auto hits = clipHits();
        for (auto iterator = hits.rbegin();
             iterator != hits.rend();
             ++iterator)
        {
            const auto& hit = *iterator;
            if (!hit.bounds.contains(position))
                continue;
            if (const auto* clip = project->findClip(hit.clipId))
            {
                nextClipId = hit.clipId;
                nextMode = dragModeAt(hit, *clip, position);
            }
            break;
        }
    }

    auto cursor = juce::MouseCursor::NormalCursor;
    if (nextMode == DragMode::gain
        || nextMode == DragMode::fadeInCurve
        || nextMode == DragMode::fadeOutCurve)
    {
        cursor = juce::MouseCursor::UpDownResizeCursor;
    }
    else if (nextMode == DragMode::fadeIn
             || nextMode == DragMode::fadeOut
             || nextMode == DragMode::trimStart
             || nextMode == DragMode::trimEnd)
    {
        cursor = juce::MouseCursor::LeftRightResizeCursor;
    }
    else if (nextMode == DragMode::move)
    {
        cursor = juce::MouseCursor::DraggingHandCursor;
    }
    setMouseCursor(cursor);

    if (nextClipId == hoveredClipId && nextMode == hoveredDragMode)
        return;
    hoveredClipId = std::move(nextClipId);
    hoveredDragMode = nextMode;
    repaint();
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
