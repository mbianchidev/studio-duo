#include "TimelineComponent.h"

#include "NumericInput.h"
#include "StudioIconButton.h"
#include "model/ArrangementEditing.h"
#include "workflow/WorkspaceCommands.h"
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
constexpr int trackControlY = 48;
constexpr int trackControlSize = 22;
constexpr int trackMuteX = 24;
constexpr int trackSoloX = 50;
constexpr int trackArmX = 76;
constexpr int trackVolumeIconX = 105;
constexpr int trackVolumeStartX = 120;
constexpr int trackVolumeEndX = 164;

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
    setWantsKeyboardFocus(true);
    setTitle("Timeline");
    setDescription("Arrangement");
    setHelpText("Up/Down select tracks. Left/Right select clips. Shift+arrows move clips. "
                "Alt+Left/Right move the playhead. Alt+Up/Down change clip gain; add Shift for 0.1 dB. "
                "Shift+F10 opens actions. Escape clears the clip selection. Tab leaves the timeline.");
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TimelineComponent::setProject(const Project* projectToDisplay)
{
    project = projectToDisplay;
    rebuildRows();
    std::erase_if(selectedTracks, [this](const auto& id) { return project == nullptr || !project->isTrackVisible(id); });
    std::erase_if(selectedClips, [this](const auto& id)
    {
        return project == nullptr || (project->findClip(id) == nullptr && project->findMidiClip(id) == nullptr);
    });
    if (project == nullptr || project->findTrack(selectedTrackId) == nullptr)
    {
        selectedTrackId.clear();
        selectedClipId.clear();
    }
    updateAccessibleSelection();
    repaint();
}

void TimelineComponent::setSelection(juce::String trackId, juce::String clipId)
{
    if (trackId != selectedTrackId || clipId != selectedClipId || selectedTracks.empty())
    {
        selectedTracks = trackId.isNotEmpty() ? std::vector<juce::String> { trackId } : std::vector<juce::String> {};
        selectedClips = clipId.isNotEmpty() ? std::vector<juce::String> { clipId } : std::vector<juce::String> {};
    }
    selectedTrackId = std::move(trackId);
    selectedClipId = std::move(clipId);
    updateAccessibleSelection();
    if (hasKeyboardFocus(false))
        ensureSelectionVisible();
    repaint();
}

void TimelineComponent::setMultiSelection(std::vector<juce::String> tracks, std::vector<juce::String> clips)
{
    selectedTracks = std::move(tracks);
    selectedClips = std::move(clips);
    selectedTrackId = selectedTracks.empty() ? juce::String() : selectedTracks.back();
    selectedClipId = selectedClips.empty() ? juce::String() : selectedClips.back();
    notifySelection();
}

void TimelineComponent::notifySelection()
{
    if (project != nullptr && selectedClipId.isNotEmpty())
    {
        const auto* owner = project->findTrackContainingClip(selectedClipId);
        if (owner == nullptr)
            owner = project->findTrackContainingMidiClip(selectedClipId);
        if (owner != nullptr)
            selectedTrackId = owner->id;
    }
    updateAccessibleSelection();
    if (selectedClipId.isNotEmpty() && onClipSelected)
        onClipSelected(selectedTrackId, selectedClipId);
    else if (onTrackSelected)
        onTrackSelected(selectedTrackId);
    repaint();
}

bool TimelineComponent::clipIsSelected(const juce::String& id) const
{
    return id == selectedClipId || std::find(selectedClips.cbegin(), selectedClips.cend(), id) != selectedClips.cend();
}

void TimelineComponent::selectForMouse(const juce::String& trackId, const juce::String& clipId,
                                      juce::ModifierKeys modifiers)
{
    const auto additive = modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isShiftDown();
    if (modifiers.isShiftDown() && clipId.isEmpty() && selectedTrackId.isNotEmpty())
    {
        auto first = std::find_if(visibleTrackCache.begin(), visibleTrackCache.end(),
                                 [this](const auto* track) { return track->id == selectedTrackId; });
        auto last = std::find_if(visibleTrackCache.begin(), visibleTrackCache.end(),
                                [&trackId](const auto* track) { return track->id == trackId; });
        if (first != visibleTrackCache.end() && last != visibleTrackCache.end())
        {
            if (first > last)
                std::swap(first, last);
            selectedTracks.clear();
            for (; first <= last; ++first)
                selectedTracks.push_back((*first)->id);
            selectedClips.clear();
        }
    }
    else if (additive)
    {
        auto& selection = clipId.isNotEmpty() ? selectedClips : selectedTracks;
        const auto& id = clipId.isNotEmpty() ? clipId : trackId;
        const auto found = std::find(selection.begin(), selection.end(), id);
        if (found != selection.end() && !modifiers.isShiftDown())
            selection.erase(found);
        else if (found == selection.end())
            selection.push_back(id);
        if (clipId.isNotEmpty() && project != nullptr)
        {
            selectedTracks.clear();
            for (const auto& selectedId : selectedClips)
            {
                const auto* owner = project->findTrackContainingClip(selectedId);
                if (owner == nullptr)
                    owner = project->findTrackContainingMidiClip(selectedId);
                if (owner != nullptr && std::find(selectedTracks.begin(), selectedTracks.end(), owner->id) == selectedTracks.end())
                    selectedTracks.push_back(owner->id);
            }
        }
        else
            selectedClips.clear();
    }
    else if ((clipId.isNotEmpty() && !clipIsSelected(clipId))
             || (clipId.isEmpty() && std::find(selectedTracks.begin(), selectedTracks.end(), trackId) == selectedTracks.end())
             || selectedTracks.empty())
    {
        selectedTracks = { trackId };
        selectedClips = clipId.isEmpty() ? std::vector<juce::String> {} : std::vector<juce::String> { clipId };
    }
    if (clipId.isEmpty() && !additive)
        selectedClips.clear();
    selectedTrackId = std::find(selectedTracks.begin(), selectedTracks.end(), trackId) != selectedTracks.end()
        ? trackId : selectedTracks.empty() ? juce::String() : selectedTracks.back();
    selectedClipId = clipIsSelected(clipId) && std::find(selectedClips.begin(), selectedClips.end(), clipId) != selectedClips.end()
        ? clipId : selectedClips.empty() ? juce::String() : selectedClips.back();
    notifySelection();
}

void TimelineComponent::setAutomationVisible(bool visible)
{
    if (automationVisible == visible)
        return;
    automationVisible = visible;
    rebuildRows();
    updateAccessibleSelection();
    if (onLayoutChanged)
        onLayoutChanged();
    repaint();
}

void TimelineComponent::setStretchToolEnabled(bool enabled)
{
    stretchToolEnabled = enabled;
    repaint();
}

bool TimelineComponent::dispatchNavigationKey(const juce::KeyPress& key)
{
    const juce::ScopedValueSetter<bool> dispatch(dispatchingNavigationKey, true);
    return keyPressed(key);
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
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        const auto visibleEndSeconds =
            (static_cast<double>(newPosition) + viewport->getViewWidth()
             - trackHeaderWidth) / pixelsPerSecond;
        navigationEndSeconds = std::max(
            navigationEndSeconds,
            visibleEndSeconds + timelineExtensionSeconds);
    }
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
    pixelsPerSecond = juce::jlimit(minimumPixelsPerSecond,
                                   maximumPixelsPerSecond,
                                   pixels);
    repaint();
}

void TimelineComponent::setSnapEnabled(bool enabled)
{
    snapEnabled = enabled;
}

void TimelineComponent::setEditGridBeats(double beats)
{
    if (std::isfinite(beats) && beats > 0.0)
        editGridBeats = beats;
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
    const auto projectSeconds = project != nullptr
        ? project->timelineEndSeconds()
        : 8.0;
    auto previewEnd = 0.0;
    for (const auto& preview : recordingPreviews)
        previewEnd = std::max(previewEnd, preview.startSeconds + preview.durationSeconds);
    const auto seconds = std::max(
        navigationEndSeconds,
        std::max({ projectSeconds, previewEnd, playheadSeconds }) + 8.0);
    const auto maximumContentWidth = static_cast<double>(
        std::numeric_limits<int>::max() - trackHeaderWidth);
    const auto contentWidth = std::min(maximumContentWidth,
                                       std::ceil(seconds * pixelsPerSecond));
    return std::max(minimumWidth,
                    trackHeaderWidth + static_cast<int>(contentWidth));
}

int TimelineComponent::preferredHeight(int minimumHeight) const
{
    return std::max(minimumHeight, rowsEnd + addTrackHeight);
}

void TimelineComponent::updateAccessibleSelection()
{
    auto title = juce::String("Timeline");
    auto value = juce::String();
    if (project != nullptr)
    {
        if (const auto* track = project->findTrack(selectedTrackId))
        {
            title += ": " + track->name;
            value = (track->muted ? "Muted" : "Not muted") + juce::String(track->solo ? "; solo" : "; not solo")
                + (track->armed ? "; record armed" : "; not record armed")
                + "; track level " + juce::String(track->volumeDecibels, 1) + " dB";
            if (track->frozenTrackId.isNotEmpty())
                value += "; frozen; unfreeze to edit";
            else if (!project->isTrackActive(track->id))
                value += "; deactivated";
            if (track->parentTrackId.isNotEmpty())
                value += project->activeTakeTrackId(track->parentTrackId) == track->id ? "; active take" : "; inactive take";
            else if (std::any_of(project->tracks.cbegin(), project->tracks.cend(),
                         [track](const auto& candidate) { return candidate.parentTrackId == track->id; }))
                value += track->versionsCollapsed ? "; takes collapsed" : "; takes expanded";
            if (!track->compRegions.empty())
                value += "; comp regions " + juce::String(static_cast<int>(track->compRegions.size()));
            if (const auto* group = project->editGroupForTrack(track->parentTrackId.isEmpty() ? track->id : track->parentTrackId);
                group != nullptr && group->enabled)
                value += "; linked edits enabled";
        }
        if (const auto* clip = project->findClip(selectedClipId))
        {
            title += " / " + clip->name;
            value += "; start " + juce::String(clip->startSeconds, 3) + " seconds; length "
                + juce::String(clip->durationSeconds, 3) + " seconds; clip gain " + juce::String(clip->gainDecibels, 1)
                + " dB; fade in " + juce::String(clip->fadeInSeconds, 3) + " seconds, curve "
                + juce::String(clip->fadeInCurve, 2) + "; fade out "
                + juce::String(clip->fadeOutSeconds, 3) + " seconds, curve " + juce::String(clip->fadeOutCurve, 2);
            if (clip->muted) value += "; clip muted";
            if (clip->polarityInverted) value += "; polarity inverted";
            if (clip->reversed) value += "; reversed";
        }
        else if (const auto* midi = project->findMidiClip(selectedClipId))
        {
            title += " / " + midi->name;
            value += "; MIDI start " + juce::String(midi->startBeats, 3) + " beats; length "
                + juce::String(midi->durationBeats, 3) + " beats; notes " + juce::String(static_cast<int>(midi->notes.size()));
            if (midi->muted) value += "; clip muted";
        }
    }
    if (selectedClips.size() > 1)
        value += "; " + juce::String(static_cast<int>(selectedClips.size())) + " clips selected";
    else if (selectedTracks.size() > 1)
        value += "; " + juce::String(static_cast<int>(selectedTracks.size())) + " tracks selected";
    value += automationVisible ? "; automation visible" : "; automation hidden";
    value = title + (value.isEmpty() ? "; no selection" : "; " + value);
    if (getTitle() == title && accessibleSelectionValue == value)
        return;
    accessibleSelectionValue = std::move(value);
    setTitle(title);
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

std::unique_ptr<juce::AccessibilityHandler> TimelineComponent::createAccessibilityHandler()
{
    class SelectionValue final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit SelectionValue(TimelineComponent& component) : owner(component) {}
        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return owner.accessibleSelectionValue; }
        void setValueAsString(const juce::String&) override {}
    private:
        TimelineComponent& owner;
    };
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::list,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::focus,
                                               [this] { grabKeyboardFocus(); }),
        juce::AccessibilityHandler::Interfaces(std::make_unique<SelectionValue>(*this)));
}

void TimelineComponent::ensureSelectionVisible()
{
    auto* viewport = findParentComponentOfClass<juce::Viewport>();
    if (viewport == nullptr || selectedTrackId.isEmpty())
        return;
    const auto view = viewport->getViewArea();
    const auto y = static_cast<int>(trackY(selectedTrackId));
    auto viewY = view.getY();
    if (y < viewY)
        viewY = y;
    else if (y + trackHeight > view.getBottom())
        viewY = y + trackHeight - view.getHeight();
    auto viewX = view.getX();
    if (selectedClipId.isNotEmpty())
        for (const auto& hit : clipHits({ 0.0f, static_cast<float>(y), static_cast<float>(getWidth()),
                                        static_cast<float>(trackHeight) }))
            if (hit.clipId == selectedClipId)
            {
                if (hit.bounds.getX() < static_cast<float>(viewX + trackHeaderWidth)
                    || hit.bounds.getX() >= static_cast<float>(view.getRight() - 24))
                    viewX = static_cast<int>(hit.bounds.getX()) - trackHeaderWidth - 24;
                break;
            }
    viewport->setViewPosition(std::max(0, viewX), std::max(0, viewY));
    setViewportPosition(viewport->getViewPositionX());
}

void TimelineComponent::selectForKeyboard(juce::String trackId, juce::String clipId)
{
    selectedTracks = trackId.isEmpty() ? std::vector<juce::String> {} : std::vector<juce::String> { trackId };
    selectedClips = clipId.isEmpty() ? std::vector<juce::String> {} : std::vector<juce::String> { clipId };
    setSelection(trackId, clipId);
    ensureSelectionVisible();
    if (clipId.isNotEmpty())
    {
        if (onClipSelected)
            onClipSelected(trackId, clipId);
    }
    else if (onTrackSelected)
        onTrackSelected(trackId);
}

void TimelineComponent::focusGained(FocusChangeType)
{
    const auto tracks = visibleTracks();
    if (selectedTrackId.isEmpty() && !tracks.empty())
        selectForKeyboard(tracks.front()->id);
    else
        ensureSelectionVisible();
    repaint();
}

void TimelineComponent::focusLost(FocusChangeType)
{
    repaint();
}

juce::Point<float> TimelineComponent::keyboardContextPosition() const
{
    auto position = juce::Point<float>(static_cast<float>(viewportPositionX + trackHeaderWidth / 2),
        trackY(selectedTrackId) + trackHeight * 0.5f);
    for (const auto& hit : clipHits({ 0.0f, trackY(selectedTrackId), static_cast<float>(getWidth()), static_cast<float>(trackHeight) }))
        if (hit.clipId == selectedClipId)
        {
            auto bounds = hit.bounds;
            if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
                bounds = bounds.getIntersection(viewport->getViewArea().toFloat().withTrimmedLeft(trackHeaderWidth));
            if (!bounds.isEmpty())
                position = bounds.getCentre();
            break;
        }
    return position;
}

bool TimelineComponent::keyPressed(const juce::KeyPress& key)
{
    if (onCommandKey && !dispatchingNavigationKey)
        return onCommandKey(key);
    if (project == nullptr || key.getModifiers().isCommandDown()
        || key.getModifiers().isCtrlDown())
        return false;
    const auto code = key.getKeyCode();
    const auto shift = key.getModifiers().isShiftDown();
    const auto alt = key.getModifiers().isAltDown();
    if (code == 'A' && !shift && !alt)
    {
        setAutomationVisible(!automationVisible);
        return true;
    }
    const auto tracks = visibleTracks();
    if (tracks.empty())
        return false;
    const auto selected = std::find_if(tracks.begin(), tracks.end(),
                                       [this](const auto* track) { return track->id == selectedTrackId; });
    const auto index = selected == tracks.end() ? -1 : static_cast<int>(std::distance(tracks.begin(), selected));
    const auto horizontal = code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
    const auto vertical = code == juce::KeyPress::upKey || code == juce::KeyPress::downKey;
    const auto direction = code == juce::KeyPress::leftKey || code == juce::KeyPress::upKey ? -1 : 1;
    if (alt && horizontal && !shift && onSeek)
    {
        onSeek(project->secondsAtBeat(std::max(0.0, project->beatsAt(playheadSeconds) + direction * editGridBeats)));
        return true;
    }
    if (!alt && !shift && (code == juce::KeyPress::homeKey || code == juce::KeyPress::endKey) && onSeek)
    {
        onSeek(code == juce::KeyPress::homeKey ? 0.0 : project->timelineEndSeconds());
        return true;
    }
    if (!alt && !shift && vertical)
    {
        const auto next = index < 0 ? 0 : juce::jlimit(0, static_cast<int>(tracks.size()) - 1, index + direction);
        selectForKeyboard(tracks[static_cast<std::size_t>(next)]->id);
        return true;
    }
    if (index < 0)
        return false;
    const auto* track = tracks[static_cast<std::size_t>(index)];
    if (!alt && !shift && code == juce::KeyPress::escapeKey && selectedClipId.isNotEmpty())
    {
        selectForKeyboard(track->id);
        return true;
    }
    if (alt && vertical && onClipGainChanged)
        if (const auto* clip = project->findClip(selectedClipId))
        {
            const auto gain = juce::jlimit(-60.0f, 24.0f,
                clip->gainDecibels + (code == juce::KeyPress::upKey ? 1.0f : -1.0f) * (shift ? 0.1f : 1.0f));
            if (!juce::approximatelyEqual(gain, clip->gainDecibels))
                onClipGainChanged(clip->id, gain);
            return true;
        }
    if (alt && !shift)
    {
        if (code == 'M' && onTrackMute)
        {
            onTrackMute(track->id);
            return true;
        }
        if (code == 'S' && onTrackSolo)
        {
            onTrackSolo(track->id);
            return true;
        }
        if (code == 'R' && onTrackArm
            && (track->type == TrackType::audio || track->type == TrackType::midi || track->type == TrackType::instrument))
        {
            onTrackArm(track->id);
            return true;
        }
    }
    if (alt)
        return false;
    auto hits = clipHits({ 0.0f, trackY(track->id), static_cast<float>(getWidth()), static_cast<float>(trackHeight) });
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                             [track](const auto& hit) { return hit.trackId != track->id; }), hits.end());
    std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right)
    {
        if (left.startSeconds < right.startSeconds)
            return true;
        if (right.startSeconds < left.startSeconds)
            return false;
        return left.clipId < right.clipId;
    });
    const auto clip = std::find_if(hits.begin(), hits.end(),
                                  [this](const auto& hit) { return hit.clipId == selectedClipId; });
    if (shift && (horizontal || vertical) && clip != hits.end() && onClipMoved)
    {
        if (horizontal)
        {
            const auto seconds = project->secondsAtBeat(
                std::max(0.0, project->beatsAt(clip->startSeconds) + direction * editGridBeats));
            onClipMoved(clip->clipId, track->id, seconds);
            return true;
        }
        for (int destination = index + direction; destination >= 0 && destination < static_cast<int>(tracks.size());
             destination += direction)
        {
            const auto* target = tracks[static_cast<std::size_t>(destination)];
            const auto compatible = clip->midi
                ? target->parentTrackId.isEmpty() && (target->type == TrackType::midi || target->type == TrackType::instrument)
                : target->type == TrackType::audio;
            if (compatible)
            {
                onClipMoved(clip->clipId, target->id, clip->startSeconds);
                return true;
            }
        }
        return false;
    }
    if (!shift && horizontal && !hits.empty())
    {
        const auto current = clip == hits.end() ? -1 : static_cast<int>(std::distance(hits.begin(), clip));
        const auto next = current < 0 ? (direction > 0 ? 0 : static_cast<int>(hits.size()) - 1)
            : juce::jlimit(0, static_cast<int>(hits.size()) - 1, current + direction);
        selectForKeyboard(track->id, hits[static_cast<std::size_t>(next)].clipId);
        return true;
    }
    if (shift && code == juce::KeyPress::F10Key)
    {
        ensureSelectionVisible();
        showContextMenu(keyboardContextPosition(), false);
        return true;
    }
    if (!shift && code == juce::KeyPress::returnKey)
    {
        if (clip != hits.end() && onSeek)
        {
            onSeek(clip->startSeconds);
            return true;
        }
        if (onEditTrack)
        {
            onEditTrack(track->id, localAreaToGlobal(juce::Rectangle<int> {
                viewportPositionX + 24, static_cast<int>(trackY(track->id)) + 8, trackHeaderWidth - 34, 32 }));
            return true;
        }
    }
    return false;
}

void TimelineComponent::paint(juce::Graphics& graphics)
{
#if defined(STUDIO_DUO_TESTING)
    testingPaintStatistics = {};
#endif
    graphics.fillAll(juce::Colour(StudioColours::window));
    const auto dirty = graphics.getClipBounds();
    const auto dirtyBounds = dirty.toFloat();
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, 0, getWidth(), rulerHeight);
    graphics.fillRect(viewportPositionX, 0, trackHeaderWidth, getHeight());

    if (project == nullptr)
        return;

    const auto maximumSeconds = std::max(
        project->timelineEndSeconds() + 8.0,
        static_cast<double>(getWidth() - trackHeaderWidth) / pixelsPerSecond);
    const std::array sectionColours {
        juce::Colour(StudioColours::violet),
        juce::Colour(StudioColours::orange),
        juce::Colour(StudioColours::green),
        juce::Colour(StudioColours::amber)
    };
    for (std::size_t index = 0; index < project->sections.size(); ++index)
    {
        const auto& section = project->sections[index];
        const auto dragging =
            section.id == draggedSectionId;
        const auto startSeconds = dragging
            ? sectionDragPreviewStart
            : section.timeSeconds;
        const auto endSeconds = dragging
            ? sectionDragPreviewEnd
            : sectionEndSeconds(index, maximumSeconds);
        const auto startX = secondsToX(startSeconds);
        const auto endX = secondsToX(endSeconds);
        const auto colour = sectionColours[index % sectionColours.size()];
        graphics.setColour(colour.withAlpha(0.18f));
        graphics.fillRect(startX,
                          static_cast<float>(markerLaneHeight),
                          std::max(1.0f, endX - startX),
                          static_cast<float>(sectionLaneHeight));
        graphics.setColour(colour.withAlpha(0.85f));
        graphics.drawVerticalLine(static_cast<int>(startX),
                                  0.0f,
                                  static_cast<float>(getHeight()));
        graphics.setFont(juce::Font(juce::FontOptions(10.0f,
                                                      juce::Font::bold)));
        graphics.drawText(section.name,
                          static_cast<int>(startX) + 6,
                          markerLaneHeight,
                          std::max(0, static_cast<int>(endX - startX) - 10),
                          sectionLaneHeight,
                          juce::Justification::centredLeft,
                          true);
        graphics.setColour(colour.brighter(0.2f));
        graphics.fillRect(
            endX - 3.0f,
            static_cast<float>(markerLaneHeight + 3),
            3.0f,
            static_cast<float>(sectionLaneHeight - 6));
    }
    for (const auto& marker : project->markers)
    {
        const auto markerSeconds = marker.id == draggedMarkerId
            ? markerDragPreviewSeconds
            : marker.timeSeconds;
        const auto x = secondsToX(markerSeconds);
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.drawVerticalLine(static_cast<int>(x),
                                  2.0f,
                                  static_cast<float>(markerLaneHeight - 2));
        juce::Path flag;
        flag.startNewSubPath(x, 2.0f);
        flag.lineTo(x + 9.0f, 5.5f);
        flag.lineTo(x, 9.0f);
        flag.closeSubPath();
        graphics.fillPath(flag);
        graphics.setFont(juce::Font(juce::FontOptions(10.0f,
                                                      juce::Font::bold)));
        graphics.drawText(marker.name,
                          static_cast<int>(x) + 11,
                          0,
                          juce::jmax(
                              0,
                              getWidth()
                                  - static_cast<int>(x)
                                  - 12),
                          markerLaneHeight,
                          juce::Justification::centredLeft,
                          true);
    }

    const auto drawRangeFill = [this, &graphics](
                                   double startSeconds,
                                   double endSeconds,
                                   juce::Colour colour)
    {
        if (endSeconds <= startSeconds)
            return;
        const auto startX = secondsToX(startSeconds);
        const auto endX = secondsToX(endSeconds);
        graphics.setColour(colour.withAlpha(0.045f));
        graphics.fillRect(startX,
                          static_cast<float>(rulerHeight),
                          std::max(1.0f, endX - startX),
                          static_cast<float>(getHeight() - rulerHeight));
    };
    if (project->loopEnabled)
    {
        drawRangeFill(project->loopStartSeconds,
                      project->loopEndSeconds,
                      juce::Colour(StudioColours::violet));
    }
    if (project->punchEnabled)
    {
        drawRangeFill(project->punchInSeconds,
                      project->punchOutSeconds,
                      juce::Colour(StudioColours::orange));
    }

    const auto firstSeconds = xToSeconds(static_cast<float>(std::max(0, dirty.getX() - 48)));
    const auto firstMeter = project->meterAt(firstSeconds);
    const auto meterAnchor = project->beatsAt(firstMeter.timeSeconds);
    const auto firstStep = 4.0 / firstMeter.denominator;
    auto seconds = project->secondsAtBeat(meterAnchor
        + std::max(0.0, std::floor((project->beatsAt(firstSeconds) - meterAnchor) / firstStep)) * firstStep);
    const auto gridEndSeconds = std::min(maximumSeconds, xToSeconds(static_cast<float>(dirty.getRight() + 48)));
    constexpr auto minimumGridSpacing = 4;
    constexpr auto minimumBarLabelSpacing = 48;
    auto lastGridX =
        trackHeaderWidth - 8;
    auto lastBarLabelRight =
        trackHeaderWidth
        - minimumBarLabelSpacing;
    for (int line = 0; line < 100000 && seconds <= gridEndSeconds; ++line)
    {
#if defined(STUDIO_DUO_TESTING)
        ++testingPaintStatistics.gridLinesConsidered;
#endif
        const auto x = static_cast<int>(secondsToX(seconds));
        const auto position = project->musicalPositionAt(seconds);
        const auto isBar = position.beat == 1 && position.ticks < 2;
        const auto canDrawLine =
            x >= trackHeaderWidth
            && x - lastGridX
                >= minimumGridSpacing;
        if (canDrawLine)
        {
            graphics.setColour(
                juce::Colour(
                    isBar
                        ? StudioColours::border
                        : 0xff25292d));
            graphics.drawVerticalLine(
                x,
                static_cast<float>(rulerHeight),
                static_cast<float>(getHeight()));
            lastGridX = x;
        }

        if (isBar
            && x >= lastBarLabelRight
                + minimumBarLabelSpacing)
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(12.0f);
            graphics.drawText(juce::String(position.bar),
                              x + 5,
                              timelineRulerTop,
                              42,
                              rulerHeight - timelineRulerTop,
                              juce::Justification::centredLeft);
            lastBarLabelRight = x + 42;
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
        graphics.fillRect(x - 1, timelineRulerTop, 3, 5);
        graphics.setFont(9.0f);
        graphics.drawText(juce::String(tempoChange.bpm, 1)
                              + (tempoChange.rampToNext ? " R" : ""),
                          x + 4,
                          timelineRulerTop + 1,
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
        graphics.fillRect(x - 1, timelineRulerTop + 16, 3, 5);
        graphics.setFont(9.0f);
        graphics.drawText(juce::String(meterChange.numerator)
                              + "/"
                              + juce::String(meterChange.denominator),
                          x + 4,
                          timelineRulerTop + 15,
                          42,
                          12,
                          juce::Justification::centredLeft);
    }

    const auto drawRangeMarkers = [this, &graphics](
                                      double startSeconds,
                                      double endSeconds,
                                      juce::Colour colour)
    {
        if (endSeconds <= startSeconds)
            return;
        const auto startX = secondsToX(startSeconds);
        const auto endX = secondsToX(endSeconds);
        graphics.setColour(colour.withAlpha(0.72f));
        graphics.fillRect(startX,
                          static_cast<float>(rulerHeight - 4),
                          std::max(1.0f, endX - startX),
                          4.0f);
        graphics.drawVerticalLine(static_cast<int>(startX),
                                  static_cast<float>(timelineRulerTop),
                                  static_cast<float>(getHeight()));
        graphics.drawVerticalLine(static_cast<int>(endX),
                                  static_cast<float>(timelineRulerTop),
                                  static_cast<float>(getHeight()));
    };
    if (project->loopEnabled)
    {
        drawRangeMarkers(project->loopStartSeconds,
                         project->loopEndSeconds,
                         juce::Colour(StudioColours::violet));
    }
    if (project->punchEnabled)
    {
        drawRangeMarkers(project->punchInSeconds,
                         project->punchOutSeconds,
                         juce::Colour(StudioColours::orange));
    }

    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(viewportPositionX, 0, trackHeaderWidth, rulerHeight);
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(9.0f,
                                                  juce::Font::bold)));
    graphics.drawText("MARKERS",
                      viewportPositionX + 12,
                      0,
                      trackHeaderWidth - 24,
                      markerLaneHeight,
                      juce::Justification::centredLeft);
    graphics.drawText("SECTIONS",
                      viewportPositionX + 12,
                      markerLaneHeight,
                      trackHeaderWidth - 24,
                      sectionLaneHeight,
                      juce::Justification::centredLeft);
    graphics.drawText("TIMELINE",
                      viewportPositionX + 12,
                      timelineRulerTop,
                      trackHeaderWidth - 24,
                      rulerHeight - timelineRulerTop,
                      juce::Justification::centredLeft);

    const auto tracks = visibleTracks();
    for (std::size_t index = 0; index < tracks.size(); ++index)
    {
        const auto y = rowTops[index];
        const auto rowHeight = trackRowHeight(tracks[index]->id);
        if (!dirty.intersects(juce::Rectangle<int> { 0, y, getWidth(), rowHeight }))
            continue;
#if defined(STUDIO_DUO_TESTING)
        ++testingPaintStatistics.tracksPainted;
#endif
        const auto& track = *tracks[index];
        const auto selected = track.id == selectedTrackId
            || std::find(selectedTracks.begin(), selectedTracks.end(), track.id) != selectedTracks.end();

        graphics.setColour(juce::Colour(selected ? 0xff22272b : 0xff15181b));
        graphics.fillRect(viewportPositionX, y, trackHeaderWidth, rowHeight);
        if (selected && hasKeyboardFocus(false))
        {
            graphics.setColour(juce::Colour(StudioColours::orange));
            graphics.drawRect(viewportPositionX + 2, y + 2, trackHeaderWidth - 4, rowHeight - 4, 2);
        }
        graphics.setColour(juce::Colour(StudioColours::border));
        graphics.drawHorizontalLine(y + rowHeight - 1, 0.0f, static_cast<float>(getWidth()));
        graphics.drawVerticalLine(viewportPositionX + trackHeaderWidth - 1,
                                  static_cast<float>(y),
                                  static_cast<float>(y + rowHeight));

        const auto childTrack = track.parentTrackId.isNotEmpty();
        const auto children = static_cast<int>(std::count_if(project->tracks.cbegin(),
                                                             project->tracks.cend(),
                                                             [&track](const auto& candidate)
        {
            return candidate.parentTrackId == track.id || candidate.folderTrackId == track.id;
        }));
        const auto groupChildren = std::any_of(project->tracks.begin(), project->tracks.end(),
            [&track](const auto& candidate) { return candidate.folderTrackId == track.id; });
        const auto activeTake =
            childTrack
            && project->activeTakeTrackId(
                   track.parentTrackId)
                == track.id;
        if (children > 0)
        {
            juce::Path disclosure;
            if (groupChildren ? track.folderCollapsed : track.versionsCollapsed)
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

        auto folderDepth = 0;
        auto folderId = track.folderTrackId;
        while (folderId.isNotEmpty() && folderDepth < 5)
        {
            ++folderDepth;
            const auto* folder = project->findTrack(folderId);
            folderId = folder != nullptr ? folder->folderTrackId : juce::String();
        }
        const auto indent = folderDepth * 10 +
            (
            childTrack
                ? 16
                : children > 0
                ? 8
                : 0);
        const auto colourBandIndent =
            !childTrack && children > 0
                ? 8
                : 0;
        graphics.setColour(track.colour);
        graphics.fillRect(
            viewportPositionX
                + 12
                + colourBandIndent,
            y + 12,
            4,
            trackHeight - 24);
        const auto badgeText =
            activeTake
                ? juce::String("ACTIVE")
                : children > 0
                ? juce::String(children)
                    + (groupChildren ? " TRKS" : " TAKES")
                : juce::String();
        const auto badgeWidth =
            badgeText.isNotEmpty()
                ? 54
                : 0;
        const auto badgeBounds =
            juce::Rectangle<int>(
                viewportPositionX
                    + trackHeaderWidth
                    - badgeWidth
                    - 8,
                y + 11,
                badgeWidth,
                18);
        const auto nameX =
            viewportPositionX + 26 + indent;
        const auto nameRight =
            badgeText.isNotEmpty()
                ? badgeBounds.getX() - 6
                : viewportPositionX
                    + trackHeaderWidth
                    - 8;
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(14.0f);
        graphics.drawText(track.name + (track.frozenTrackId.isNotEmpty() ? " [FROZEN]"
                                       : !project->isTrackActive(track.id) ? " [OFF]" : ""),
                          nameX,
                          y + 12,
                          juce::jmax(
                              0,
                              nameRight - nameX),
                          24,
                          juce::Justification::centredLeft,
                          true);
        if (badgeText.isNotEmpty())
        {
            const auto badgeColour =
                activeTake
                    ? juce::Colour(
                          StudioColours::green)
                    : juce::Colour(
                          StudioColours::raised);
            graphics.setColour(
                activeTake
                    ? badgeColour.withAlpha(0.18f)
                    : badgeColour);
            graphics.fillRoundedRectangle(
                badgeBounds.toFloat(),
                3.0f);
            graphics.setColour(
                activeTake
                    ? badgeColour
                    : juce::Colour(
                          StudioColours::border));
            graphics.drawRoundedRectangle(
                badgeBounds.toFloat(),
                3.0f,
                1.0f);
            graphics.setColour(
                activeTake
                    ? badgeColour
                    : juce::Colour(
                          StudioColours::secondaryText));
            graphics.setFont(
                juce::Font(
                    juce::FontOptions(
                        8.0f,
                        juce::Font::bold)));
            graphics.drawFittedText(
                badgeText,
                badgeBounds,
                juce::Justification::centred,
                1,
                0.75f);
        }

        const auto drawControl = [&graphics, y, this](
                                     int x,
                                     StudioIcon icon,
                                     bool active,
                                     juce::Colour activeColour)
        {
            const juce::Rectangle<float> bounds(
                static_cast<float>(viewportPositionX + x),
                static_cast<float>(y + trackControlY),
                static_cast<float>(trackControlSize),
                static_cast<float>(trackControlSize));
            graphics.setColour(active ? activeColour : juce::Colour(StudioColours::raised));
            graphics.fillRoundedRectangle(bounds, 4.0f);
            drawStudioIcon(
                graphics,
                icon,
                bounds.reduced(5.0f),
                active ? juce::Colours::white
                       : juce::Colour(StudioColours::secondaryText),
                1.35f);
        };

        drawControl(
            trackMuteX,
            StudioIcon::mute,
            track.muted,
            juce::Colour(StudioColours::amber));
        drawControl(
            trackSoloX,
            StudioIcon::solo,
            track.solo,
            juce::Colour(StudioColours::green));
        if (track.type == TrackType::audio
            || track.type == TrackType::instrument
            || track.type == TrackType::midi)
        {
            drawControl(
                trackArmX,
                StudioIcon::record,
                track.armed,
                juce::Colour(StudioColours::orange));
        }

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
        if (track.type == TrackType::audio)
        {
            const juce::Rectangle<float> inputBounds(
                static_cast<float>(viewportPositionX + 104),
                static_cast<float>(y + 29),
                64.0f,
                18.0f);
            graphics.setColour(
                juce::Colour(StudioColours::raised));
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
            graphics.drawText(
                routingLabel,
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
        else
        {
            graphics.drawText(routingLabel,
                              viewportPositionX + 104,
                              y + 30,
                              64,
                              16,
                              juce::Justification::centredRight);
        }

        if (track.type != TrackType::folder
            && track.type != TrackType::midi)
        {
            const auto volume = track.id
                    == draggingTrackVolumeId
                ? dragPreviewTrackVolume
                : track.volumeDecibels;
            const auto faderY =
                static_cast<float>(y + trackControlY + 8);
            drawStudioIcon(
                graphics,
                StudioIcon::volume,
                {
                    static_cast<float>(
                        viewportPositionX + trackVolumeIconX),
                    static_cast<float>(y + trackControlY + 3),
                    12.0f,
                    12.0f
                },
                juce::Colour(StudioColours::secondaryText),
                1.2f);
            graphics.setColour(
                juce::Colour(StudioColours::border));
            graphics.drawLine(
                static_cast<float>(
                    viewportPositionX + trackVolumeStartX),
                faderY,
                static_cast<float>(
                    viewportPositionX + trackVolumeEndX),
                faderY,
                2.0f);
            const auto normalized = juce::jmap(
                juce::jlimit(-60.0f, 12.0f, volume),
                -60.0f,
                12.0f,
                0.0f,
                1.0f);
            const auto knobX = juce::jmap(
                normalized,
                0.0f,
                1.0f,
                static_cast<float>(
                    viewportPositionX + trackVolumeStartX),
                static_cast<float>(
                    viewportPositionX + trackVolumeEndX));
            graphics.setColour(
                juce::Colour(StudioColours::text));
            graphics.fillEllipse(
                knobX - 3.5f,
                faderY - 3.5f,
                7.0f,
                7.0f);
            graphics.setColour(
                juce::Colour(StudioColours::secondaryText));
            graphics.setFont(
                juce::Font(juce::FontOptions(8.0f)));
            graphics.drawText(
                formatDecibels(volume),
                viewportPositionX + trackVolumeIconX,
                y + 68,
                trackVolumeEndX - trackVolumeIconX,
                14,
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

    const auto addTrackY = rowsEnd;
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

        const auto parentY = rowTops[visibleIndex] + 12;
        if (!dirtyBounds.intersects(juce::Rectangle<float> {
                0.0f, static_cast<float>(parentY), static_cast<float>(getWidth()), trackHeight - 24.0f }))
            continue;
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

    const auto paintedClipHits = clipHits(dirtyBounds);
    for (const auto& hit : paintedClipHits)
    {
#if defined(STUDIO_DUO_TESTING)
        ++testingPaintStatistics.clipsPainted;
#endif
        if (hit.midi)
        {
            const auto* midiClip = project->findMidiClip(hit.clipId);
            if (midiClip == nullptr)
                continue;
            const auto selected = clipIsSelected(hit.clipId);
            const auto* track = project->findTrack(hit.trackId);
            const auto colour = track != nullptr
                ? track->colour
                : juce::Colour(StudioColours::violet);
            graphics.setColour(
                colour.withAlpha(midiClip->muted ? 0.32f : 0.82f));
            graphics.fillRoundedRectangle(hit.bounds, 5.0f);
            graphics.setColour(
                selected ? juce::Colours::white
                         : colour.brighter(0.35f));
            graphics.drawRoundedRectangle(
                hit.bounds,
                5.0f,
                selected ? 2.0f : 1.0f);
            if (!midiClip->notes.empty())
            {
                const auto minimumPitch = std::min_element(
                    midiClip->notes.cbegin(),
                    midiClip->notes.cend(),
                    [](const auto& left, const auto& right)
                    {
                        return left.pitch < right.pitch;
                    })->pitch;
                const auto maximumPitch = std::max_element(
                    midiClip->notes.cbegin(),
                    midiClip->notes.cend(),
                    [](const auto& left, const auto& right)
                    {
                        return left.pitch < right.pitch;
                    })->pitch;
                const auto pitchRange = std::max(
                    1,
                    maximumPitch - minimumPitch + 1);
                graphics.setColour(juce::Colours::white.withAlpha(0.62f));
                for (const auto& note : midiClip->notes)
                {
                    const auto noteX = hit.bounds.getX()
                        + static_cast<float>(
                            note.actualStartBeats()
                            / midiClip->durationBeats)
                            * hit.bounds.getWidth();
                    const auto noteRight = hit.bounds.getX()
                        + static_cast<float>(
                            note.endBeats()
                            / midiClip->durationBeats)
                            * hit.bounds.getWidth();
                    const auto noteY = hit.bounds.getBottom() - 7.0f
                        - static_cast<float>(
                            note.pitch - minimumPitch)
                            / static_cast<float>(pitchRange)
                            * (hit.bounds.getHeight() - 28.0f);
                    graphics.fillRoundedRectangle(
                        noteX,
                        noteY,
                        std::max(2.0f, noteRight - noteX),
                        3.0f,
                        1.0f);
                }
            }
            graphics.setColour(juce::Colours::white);
            graphics.setFont(12.5f);
            graphics.drawText(
                midiClip->name
                    + (midiClip->editorMode == MidiEditorMode::drums
                           ? "  DRUMS"
                           : "  MIDI"),
                hit.bounds.toNearestInt().withHeight(24).reduced(8, 0),
                juce::Justification::centredLeft);
            continue;
        }
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

        const auto selected = clipIsSelected(hit.clipId);
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

        auto waveformClip = *clip;
        if (draggedClipId == hit.clipId && dragMode == DragMode::slip)
            waveformClip.sourceOffsetSeconds = dragPreviewSourceOffset;
        drawClipWaveform(graphics, waveformClip, bounds, 0.28f);
        if (controlsVisible)
            drawStudioIcon(graphics, StudioIcon::stretch,
                           { bounds.getRight() - 17.0f, bounds.getBottom() - 18.0f, 14.0f, 14.0f },
                           juce::Colour(stretchToolEnabled ? StudioColours::amber : StudioColours::text));

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
            ? "  " + formatDecibels(gainDecibels)
            : juce::String();
        graphics.drawText(clip->name + processingLabel + gainLabel,
                          bounds.toNearestInt().withHeight(24).reduced(8, 0),
                          juce::Justification::centredLeft);
    }

    if (automationVisible)
    for (const auto& lane : project->automationLanes)
    {
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
        const auto& lanes = automationRows.at(lane.target.trackId);
        const auto laneIndex = static_cast<int>(std::distance(lanes.begin(), std::find(lanes.begin(), lanes.end(), &lane)));
        const auto top = static_cast<float>(rowTops[static_cast<std::size_t>(index)] + trackHeight
                                            + laneIndex * automationLaneHeight + 5);
        const auto height = static_cast<float>(automationLaneHeight - 10);
        if (!dirtyBounds.intersects(juce::Rectangle<float> { 0.0f, top, static_cast<float>(getWidth()), height }))
            continue;
#if defined(STUDIO_DUO_TESTING)
        ++testingPaintStatistics.automationLanesPainted;
#endif
        juce::Path path;
        auto started = false;
        const std::array colours { StudioColours::amber, StudioColours::green, StudioColours::violet,
                                  StudioColours::orange };
        const auto colour = juce::Colour(colours[static_cast<std::size_t>(laneIndex) % colours.size()]);
        graphics.setColour(colour.withAlpha(0.08f));
        graphics.fillRect(juce::Rectangle<float> { static_cast<float>(viewportPositionX + trackHeaderWidth),
                                                 top, static_cast<float>(getWidth()), height });
        graphics.setColour(colour.withAlpha(lane.enabled ? 0.9f : 0.4f));
        graphics.setFont(10.0f);
        graphics.drawText(lane.name + (lane.enabled ? "" : " (disabled)"),
                          viewportPositionX + trackHeaderWidth + 8, static_cast<int>(top),
                          240, 14, juce::Justification::centredLeft);
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
                       juce::jlimit(0.0, 1.0, point.value + lane.trimOffset)))
                    * height;
            if (!started)
            {
                path.startNewSubPath(secondsToX(0.0), y);
                path.lineTo(x, y);
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
        if (started)
            path.lineTo(static_cast<float>(getWidth()), path.getCurrentPosition().y);
        graphics.strokePath(path, juce::PathStrokeType(1.5f));
        if (started)
            for (const auto& hit : paintedClipHits)
            {
                if (hit.trackId != lane.target.trackId)
                    continue;
                graphics.saveState();
                graphics.reduceClipRegion(hit.bounds.toNearestInt().reduced(3));
                const auto scale = (hit.bounds.getHeight() - 12.0f) / height;
                graphics.strokePath(path, juce::PathStrokeType(1.25f),
                    juce::AffineTransform::scale(1.0f, scale)
                        .translated(0.0f, hit.bounds.getY() + 6.0f - top * scale));
                graphics.restoreState();
#if defined(STUDIO_DUO_TESTING)
                ++testingPaintStatistics.clipAutomationOverlaysPainted;
#endif
            }
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
            const auto y = static_cast<int>(trackY((*iterator)->id)) + 12;
            const juce::Rectangle<float> preview(
                secondsToX(recordingPreview.startSeconds),
                static_cast<float>(y),
                static_cast<float>(std::max(96.0,
                                            recordingPreview.durationSeconds * pixelsPerSecond)),
                trackHeight - 24.0f);
            if (!preview.intersects(dirtyBounds))
                continue;
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
    grabKeyboardFocus();
    if (event.mods.isPopupMenu())
    {
        showContextMenu(event.position);
        return;
    }

    if (event.position.y < static_cast<float>(markerLaneHeight))
    {
        const auto inTrackHeader =
            event.position.x
                >= static_cast<float>(viewportPositionX)
            && event.position.x
                < static_cast<float>(
                    viewportPositionX + trackHeaderWidth);
        if (!inTrackHeader && project != nullptr)
        {
            draggedMarkerId = markerIdAt(event.position);
            if (const auto* marker =
                    project->findMarker(draggedMarkerId))
            {
                markerDragOriginalSeconds = marker->timeSeconds;
                markerDragPreviewSeconds = marker->timeSeconds;
                setMouseCursor(
                    juce::MouseCursor::DraggingHandCursor);
            }
        }
        return;
    }
    if (event.position.y
        < static_cast<float>(timelineRulerTop))
    {
        const auto inTrackHeader =
            event.position.x
                >= static_cast<float>(viewportPositionX)
            && event.position.x
                < static_cast<float>(
                    viewportPositionX + trackHeaderWidth);
        if (!inTrackHeader && project != nullptr)
        {
            draggedSectionId = sectionIdAt(event.position);
            const auto section = std::find_if(
                project->sections.cbegin(),
                project->sections.cend(),
                [this](const auto& candidate)
                {
                    return candidate.id
                        == draggedSectionId;
                });
            if (section != project->sections.cend())
            {
                const auto index = static_cast<std::size_t>(
                    std::distance(
                        project->sections.cbegin(),
                        section));
                sectionDragOriginalStart =
                    section->timeSeconds;
                sectionDragOriginalEnd =
                    sectionEndSeconds(
                        index,
                        project->timelineEndSeconds()
                            + 8.0);
                sectionDragPreviewStart =
                    sectionDragOriginalStart;
                sectionDragPreviewEnd =
                    sectionDragOriginalEnd;
                dragStartX = event.position.x;
                const auto endX =
                    secondsToX(sectionDragOriginalEnd);
                sectionDragMode =
                    std::abs(event.position.x - endX)
                            <= 7.0f
                        ? SectionDragMode::resizeEnd
                        : SectionDragMode::move;
                setMouseCursor(
                    sectionDragMode
                            == SectionDragMode::resizeEnd
                        ? juce::MouseCursor::
                              LeftRightResizeCursor
                        : juce::MouseCursor::
                              DraggingHandCursor);
            }
        }
        return;
    }

    const auto inTrackHeader = event.position.x >= static_cast<float>(viewportPositionX)
        && event.position.x < static_cast<float>(viewportPositionX + trackHeaderWidth);
    if (project != nullptr && inTrackHeader)
    {
        const auto tracks = visibleTracks();
        const auto addTrackY = rowsEnd;
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
            const auto localY = static_cast<int>(event.position.y - trackY(track.id));
            const auto x = static_cast<int>(event.position.x) - viewportPositionX;
            const auto hasVersions = std::any_of(project->tracks.cbegin(),
                                                 project->tracks.cend(),
                                                 [&track](const auto& candidate)
            {
                return candidate.parentTrackId == track.id;
            });
            const auto hasFolderChildren = std::any_of(project->tracks.begin(), project->tracks.end(),
                [&track](const auto& candidate) { return candidate.folderTrackId == track.id; });
            if (x < 22 && hasFolderChildren && onToggleFolderCollapsed)
            {
                onToggleFolderCollapsed(track.id);
                return;
            }
            if (x < 22 && hasVersions && onToggleTrackVersions)
            {
                onToggleTrackVersions(track.id);
                return;
            }
            if (track.type == TrackType::audio
                && localY >= 29
                && localY <= 47
                && x >= 104
                && x <= 168
                && onInputMenuRequested)
            {
                selectedTrackId = track.id;
                selectedClipId.clear();
                if (onTrackSelected)
                    onTrackSelected(track.id);
                const juce::Rectangle<int> inputBounds(
                    viewportPositionX + 104,
                    static_cast<int>(trackY(track.id)) + 29,
                    64,
                    18);
                onInputMenuRequested(
                    track.id,
                    localAreaToGlobal(inputBounds));
                return;
            }
            if (localY >= trackControlY
                && localY
                    <= trackControlY + trackControlSize)
            {
                if (x >= trackMuteX
                    && x < trackMuteX + trackControlSize
                    && onTrackMute)
                    onTrackMute(track.id);
                else if (x >= trackSoloX
                         && x
                             < trackSoloX
                                 + trackControlSize
                         && onTrackSolo)
                    onTrackSolo(track.id);
                else if (x >= trackArmX
                         && x
                             < trackArmX
                                 + trackControlSize
                         && (track.type == TrackType::audio
                             || track.type
                                 == TrackType::instrument
                             || track.type == TrackType::midi)
                         && onTrackArm)
                    onTrackArm(track.id);
                else if (
                    x >= trackVolumeIconX
                    && x <= trackVolumeEndX + 4
                    && track.type != TrackType::folder
                    && track.type != TrackType::midi)
                {
                    draggingTrackVolumeId = track.id;
                    const auto normalized =
                        juce::jlimit(
                            0.0f,
                            1.0f,
                            static_cast<float>(
                                x - trackVolumeStartX)
                                / static_cast<float>(
                                    trackVolumeEndX
                                    - trackVolumeStartX));
                    dragPreviewTrackVolume =
                        juce::jmap(
                            normalized,
                            0.0f,
                            1.0f,
                            -60.0f,
                            12.0f);
                    if (onTrackVolumeGestureStarted)
                        onTrackVolumeGestureStarted(
                            track.id,
                            track.volumeDecibels);
                    repaint();
                }
                else
                    return;

                selectedTrackId = track.id;
                selectedClipId.clear();
                repaint();
                return;
            }

            selectForMouse(track.id, {}, event.mods);
            return;
        }
    }

    const auto hits = clipHits();
    for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator)
    {
        const auto& hit = *iterator;
        if (!hit.bounds.contains(event.position))
            continue;

        selectForMouse(hit.trackId, hit.clipId, event.mods);
        if (event.mods.isCommandDown() || event.mods.isCtrlDown() || event.mods.isShiftDown())
            return;
        const auto* owner = project != nullptr ? project->findTrack(hit.trackId) : nullptr;
        if (owner != nullptr && project->isTrackFrozen(owner->id))
            return;
        if (hit.midi)
        {
            if (onSeek)
                onSeek(xToSeconds(event.position.x));
            repaint();
            return;
        }
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
            dragMode = event.mods.isAltDown() ? DragMode::slip : dragModeAt(hit, *clip, event.position);
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

        if (onSeek
            && dragMode != DragMode::slip
            && dragMode != DragMode::stretch
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
    if (project != nullptr && automationVisible)
    {
        const auto row = trackIndexAt(event.position.y);
        if (row >= 0 && event.position.y >= static_cast<float>(rowTops[static_cast<std::size_t>(row)] + trackHeight))
        {
            selectForMouse(visibleTrackCache[static_cast<std::size_t>(row)]->id, {}, {});
            if (onAutomationEditorRequested)
                onAutomationEditorRequested();
            return;
        }
    }
    if (project == nullptr)
        return;
    const auto inTrackHeader = event.position.x >= static_cast<float>(viewportPositionX)
        && event.position.x < static_cast<float>(viewportPositionX + trackHeaderWidth);
    if (!inTrackHeader
        && event.position.y >= 0.0f
        && event.position.y < static_cast<float>(timelineRulerTop))
    {
        if (!event.mods.isPopupMenu())
        {
            if (event.position.y
                < static_cast<float>(markerLaneHeight))
            {
                const auto markerId = markerIdAt(event.position);
                if (markerId.isNotEmpty())
                {
                    if (onEditMarkerRequested)
                        onEditMarkerRequested(markerId);
                }
                else if (onAddMarkerRequested)
                {
                    onAddMarkerRequested(
                        xToSeconds(event.position.x));
                }
            }
            else
            {
                const auto sectionId = sectionIdAt(event.position);
                if (sectionId.isNotEmpty())
                {
                    if (onEditSectionRequested)
                        onEditSectionRequested(sectionId);
                }
                else if (onAddSectionRequested)
                {
                    onAddSectionRequested(
                        xToSeconds(event.position.x));
                }
            }
        }
        return;
    }
    if (!inTrackHeader)
    {
        const auto existingClips = clipHits();
        if (std::any_of(
                existingClips.cbegin(),
                existingClips.cend(),
                [&event](const auto& hit)
                {
                    return hit.bounds.contains(event.position);
                }))
            return;
        const auto tracks = visibleTracks();
        const auto trackIndex = trackIndexAt(event.position.y);
        if (trackIndex >= 0
            && trackIndex < static_cast<int>(tracks.size()))
        {
            const auto& track = *tracks[
                static_cast<std::size_t>(trackIndex)];
            if ((track.type == TrackType::midi
                 || track.type == TrackType::instrument)
                && onCreateMidiClip)
            {
                onCreateMidiClip(
                    track.id,
                    xToSeconds(event.position.x));
            }
        }
        return;
    }

    const auto tracks = visibleTracks();
    const auto trackIndex = trackIndexAt(event.position.y);
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    const auto& track = *tracks[static_cast<std::size_t>(trackIndex)];
    const auto localY = static_cast<int>(event.position.y - trackY(track.id));
    const auto localX = static_cast<int>(event.position.x) - viewportPositionX;
    if (localY > 42 || localX < 22)
        return;

    selectedTrackId = track.id;
    selectedClipId.clear();
    if (onTrackSelected)
        onTrackSelected(track.id);
    if (onEditTrack)
    {
        const auto indent = track.parentTrackId.isNotEmpty() ? 16 : 0;
        const juce::Rectangle<int> nameBounds(
            viewportPositionX + 24 + indent,
            static_cast<int>(trackY(track.id)) + 8,
            trackHeaderWidth - 34 - indent,
            32);
        onEditTrack(track.id, localAreaToGlobal(nameBounds));
    }
    repaint();
}

void TimelineComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingTrackVolumeId.isNotEmpty())
    {
        const auto localX =
            event.position.x
            - static_cast<float>(viewportPositionX);
        const auto normalized = juce::jlimit(
            0.0f,
            1.0f,
            (localX
             - static_cast<float>(trackVolumeStartX))
                / static_cast<float>(
                    trackVolumeEndX
                    - trackVolumeStartX));
        dragPreviewTrackVolume = juce::jmap(
            normalized,
            0.0f,
            1.0f,
            -60.0f,
            12.0f);
        repaint();
        return;
    }

    if (draggedMarkerId.isNotEmpty())
    {
        markerDragPreviewSeconds = snappedSeconds(
            xToSeconds(event.position.x));
        repaint();
        return;
    }

    if (draggedSectionId.isNotEmpty()
        && project != nullptr)
    {
        const auto section = std::find_if(
            project->sections.cbegin(),
            project->sections.cend(),
            [this](const auto& candidate)
            {
                return candidate.id == draggedSectionId;
            });
        if (section == project->sections.cend())
            return;
        const auto index = static_cast<std::size_t>(
            std::distance(
                project->sections.cbegin(),
                section));
        const auto nextStart =
            index + 1 < project->sections.size()
                ? project->sections[index + 1].timeSeconds
                : std::numeric_limits<double>::infinity();
        auto minimumStart = 0.0;
        if (index > 0)
        {
            const auto& previous =
                project->sections[index - 1];
            minimumStart =
                previous.endTimeSeconds.value_or(
                    previous.timeSeconds + 0.0001);
        }
        if (sectionDragMode == SectionDragMode::move)
        {
            const auto duration =
                sectionDragOriginalEnd
                - sectionDragOriginalStart;
            const auto requested = snappedSeconds(
                sectionDragOriginalStart
                + static_cast<double>(
                      event.position.x - dragStartX)
                    / pixelsPerSecond);
            const auto maximumStart =
                std::isfinite(nextStart)
                    ? juce::jmax(
                          minimumStart,
                          nextStart - duration)
                    : requested;
            sectionDragPreviewStart =
                std::isfinite(nextStart)
                    ? juce::jlimit(
                          minimumStart,
                          maximumStart,
                          requested)
                    : juce::jmax(
                          minimumStart,
                          requested);
            sectionDragPreviewEnd =
                sectionDragPreviewStart + duration;
        }
        else if (sectionDragMode
                 == SectionDragMode::resizeEnd)
        {
            const auto requested = juce::jmax(
                sectionDragPreviewStart + 0.01,
                snappedSeconds(
                    xToSeconds(event.position.x)));
            sectionDragPreviewEnd =
                std::isfinite(nextStart)
                    ? juce::jmin(nextStart, requested)
                    : requested;
        }
        repaint();
        return;
    }

    if (draggedClipId.isEmpty())
        return;

    const auto deltaSeconds = (event.position.x - dragStartX) / pixelsPerSecond;
    const auto beat = project != nullptr ? 60.0 / project->tempo : 0.5;
    const auto minimumDuration = std::max(0.01, beat / 16.0);

    if (dragMode == DragMode::move)
    {
        const auto unsnapped = std::max(0.0, dragOriginalStart + deltaSeconds);
        dragPreviewStart = snappedSeconds(unsnapped);

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
        dragPreviewStart = snappedSeconds(unsnapped);
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
        const auto unsnappedEnd =
            dragOriginalStart + unsnappedDuration;
        dragPreviewDuration =
            snappedSeconds(unsnappedEnd)
            - dragOriginalStart;
        dragPreviewDuration = juce::jlimit(minimumDuration,
                                           sourceRemaining,
                                           dragPreviewDuration);
    }
    else if (dragMode == DragMode::slip)
    {
        if (const auto* clip = project != nullptr ? project->findClip(draggedClipId) : nullptr)
        {
            juce::String error;
            const auto slipped = ArrangementEditing::slipClip(
                *clip, deltaSeconds * clip->playbackRate * (clip->reversed ? 1.0 : -1.0), error);
            if (slipped)
                dragPreviewSourceOffset = slipped->sourceOffsetSeconds;
        }
    }
    else if (dragMode == DragMode::stretch)
    {
        if (const auto* clip = project != nullptr ? project->findClip(draggedClipId) : nullptr)
        {
            const auto sourceDuration = clip->durationSeconds * clip->playbackRate;
            dragPreviewDuration = juce::jlimit(sourceDuration / 4.0, sourceDuration / 0.25,
                snappedSeconds(dragOriginalStart + dragOriginalDuration + deltaSeconds) - dragOriginalStart);
        }
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
    if (draggingTrackVolumeId.isNotEmpty())
    {
        const auto trackId = draggingTrackVolumeId;
        draggingTrackVolumeId.clear();
        if (onTrackVolumeChanged)
            onTrackVolumeChanged(
                trackId,
                dragPreviewTrackVolume);
        repaint();
        return;
    }

    if (draggedMarkerId.isNotEmpty())
    {
        if (std::abs(
                markerDragPreviewSeconds
                - markerDragOriginalSeconds)
                > 0.0001
            && onMoveMarkerRequested)
        {
            onMoveMarkerRequested(
                draggedMarkerId,
                markerDragPreviewSeconds);
        }
        draggedMarkerId.clear();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    if (draggedSectionId.isNotEmpty())
    {
        const auto sectionId = draggedSectionId;
        draggedSectionId.clear();
        sectionDragMode = SectionDragMode::none;
        if ((std::abs(
                 sectionDragPreviewStart
                 - sectionDragOriginalStart)
                 > 0.0001
             || std::abs(
                    sectionDragPreviewEnd
                    - sectionDragOriginalEnd)
                    > 0.0001)
            && onSectionRangeChanged)
        {
            onSectionRangeChanged(
                sectionId,
                sectionDragPreviewStart,
                sectionDragPreviewEnd);
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    if (draggedClipId.isNotEmpty())
    {
        if (dragMode == DragMode::slip && onClipSlipped
            && std::abs(dragPreviewSourceOffset - dragOriginalSourceOffset) > 0.000001)
            onClipSlipped(draggedClipId, dragPreviewSourceOffset - dragOriginalSourceOffset);
        else if (dragMode == DragMode::stretch && onClipStretched
                 && std::abs(dragPreviewDuration - dragOriginalDuration) > 0.000001)
            onClipStretched(draggedClipId, dragPreviewDuration);
        else if (dragMode == DragMode::move
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
    if (event.position.y >= 0.0f
        && event.position.y
            < static_cast<float>(markerLaneHeight)
        && markerIdAt(event.position).isNotEmpty())
    {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }
    if (event.position.y
            >= static_cast<float>(markerLaneHeight)
        && event.position.y
            < static_cast<float>(timelineRulerTop)
        && project != nullptr)
    {
        const auto sectionId = sectionIdAt(event.position);
        const auto section = std::find_if(
            project->sections.cbegin(),
            project->sections.cend(),
            [&sectionId](const auto& candidate)
            {
                return candidate.id == sectionId;
            });
        if (section != project->sections.cend())
        {
            const auto index = static_cast<std::size_t>(
                std::distance(
                    project->sections.cbegin(),
                    section));
            const auto endX = secondsToX(
                sectionEndSeconds(
                    index,
                    project->timelineEndSeconds() + 8.0));
            setMouseCursor(
                std::abs(event.position.x - endX)
                        <= 7.0f
                    ? juce::MouseCursor::
                          LeftRightResizeCursor
                    : juce::MouseCursor::
                          DraggingHandCursor);
            return;
        }
    }
    updateHoverState(event.position);
}

void TimelineComponent::mouseExit(const juce::MouseEvent&)
{
    if (draggingTrackVolumeId.isNotEmpty()
        || draggedMarkerId.isNotEmpty()
        || draggedSectionId.isNotEmpty()
        || draggedClipId.isNotEmpty())
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
    if (event.position.x
            < static_cast<float>(
                viewportPositionX + trackHeaderWidth)
        && std::abs(wheel.deltaY) > 0.0001f)
    {
        if (auto* viewport =
                findParentComponentOfClass<
                    juce::Viewport>();
            viewport != nullptr)
        {
            const auto distance = static_cast<int>(
                std::round(
                    wheel.deltaY
                    * (wheel.isSmooth ? 120.0f
                                      : 88.0f)));
            viewport->setViewPosition(
                viewport->getViewPositionX(),
                viewport->getViewPositionY()
                    - distance);
            return;
        }
    }

    if (std::abs(wheel.deltaY) > 0.0001f
        && std::abs(wheel.deltaY) >= std::abs(wheel.deltaX)
        && onZoomRequested)
    {
        const auto exponent = static_cast<double>(wheel.deltaY)
            * (wheel.isSmooth ? 4.0 : 10.0);
        const auto factor = juce::jlimit(0.8,
                                        1.25,
                                        std::pow(1.2, exponent));
        onZoomRequested(factor, xToSeconds(event.position.x));
        return;
    }

    juce::Component::mouseWheelMove(event, wheel);
}

void TimelineComponent::mouseMagnify(const juce::MouseEvent& event,
                                     float scaleFactor)
{
    if (onZoomRequested)
    {
        onZoomRequested(juce::jlimit(0.5,
                                    2.0,
                                    static_cast<double>(scaleFactor)),
                        xToSeconds(event.position.x));
    }
}

juce::PopupMenu TimelineComponent::createContextMenu(juce::Point<float> position, bool selectFromPosition)
{
    if (project == nullptr)
        return {};
    const auto shortcut = [this](WorkspaceCommand command, const char* fallback)
    {
        return onShortcutDescription ? onShortcutDescription(static_cast<juce::CommandID>(command))
                                     : juce::String(fallback);
    };
    const auto actionSeconds = selectFromPosition ? xToSeconds(position.x) : playheadSeconds;

    const auto inTrackHeader = position.x >= static_cast<float>(viewportPositionX)
        && position.x < static_cast<float>(viewportPositionX + trackHeaderWidth);
    if (!inTrackHeader
        && position.y >= 0.0f
        && position.y < static_cast<float>(timelineRulerTop))
    {
        juce::PopupMenu menu;
        const auto safeThis = juce::Component::SafePointer<TimelineComponent>(this);
        if (position.y
            < static_cast<float>(markerLaneHeight))
        {
            const auto markerId = markerIdAt(position);
            if (markerId.isNotEmpty())
            {
                menu.addItem("Rename marker...", [safeThis, markerId]
                {
                    if (safeThis != nullptr
                        && safeThis->onEditMarkerRequested)
                        safeThis->onEditMarkerRequested(markerId);
                });
                menu.addItem("Remove marker", [safeThis, markerId]
                {
                    if (safeThis != nullptr
                        && safeThis->onRemoveMarkerRequested)
                        safeThis->onRemoveMarkerRequested(markerId);
                });
                menu.addSeparator();
            }
            menu.addItem(
                "Add marker here...",
                [safeThis, seconds = xToSeconds(position.x)]
                {
                    if (safeThis != nullptr && safeThis->onAddMarkerRequested)
                        safeThis->onAddMarkerRequested(seconds);
                });
        }
        else
        {
            const auto sectionId = sectionIdAt(position);
            if (sectionId.isNotEmpty())
            {
                menu.addItem("Tempo, time signature and click...", [safeThis, sectionId]
                {
                    if (safeThis != nullptr && safeThis->onConfigureSectionRequested)
                        safeThis->onConfigureSectionRequested(sectionId);
                });
                menu.addItem("Rename / move section...", [safeThis, sectionId]
                {
                    if (safeThis != nullptr && safeThis->onEditSectionRequested)
                        safeThis->onEditSectionRequested(sectionId);
                });
                menu.addItem("Remove section", [safeThis, sectionId]
                {
                    if (safeThis != nullptr && safeThis->onRemoveSectionRequested)
                        safeThis->onRemoveSectionRequested(sectionId);
                });
                menu.addSeparator();
            }
            menu.addItem(
                "Add section here...",
                [safeThis, seconds = xToSeconds(position.x)]
                {
                    if (safeThis != nullptr && safeThis->onAddSectionRequested)
                        safeThis->onAddSectionRequested(seconds);
                });
        }
        return menu;
    }

    const auto tracks = visibleTracks();
    const auto trackIndex = trackIndexAt(position.y);
    const auto* clickedTrack = trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
        ? tracks[static_cast<std::size_t>(trackIndex)]
        : nullptr;

    if (selectFromPosition && !inTrackHeader && onSeek)
        onSeek(xToSeconds(position.x));

    auto hitClip = false;
    const auto hits = clipHits();
    for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator)
    {
        if (!selectFromPosition)
            break;
        const auto& hit = *iterator;
        if (!hit.bounds.contains(position))
            continue;

        hitClip = true;
        selectForMouse(hit.trackId, hit.clipId, {});
        break;
    }

    if (selectFromPosition && !hitClip)
    {
        if (clickedTrack != nullptr)
        {
            if (clickedTrack->id != selectedTrackId)
            {
                selectForMouse(clickedTrack->id, {}, {});
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

        if (clickedTrack->type == TrackType::audio
            || clickedTrack->type == TrackType::instrument
            || clickedTrack->type == TrackType::midi)
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
                const juce::Rectangle<int> nameBounds(
                    viewportPositionX + 24,
                    static_cast<int>(trackY(trackId)) + 8,
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
        const auto editableTrack = clickedTrack->type != TrackType::master;
        menu.addItem(clickedTrack->active ? "Deactivate track" : "Activate track",
                     editableTrack && clickedTrack->frozenTrackId.isEmpty(), false, [this, trackId]
        {
            if (onToggleTrackActive)
                onToggleTrackActive(trackId);
        });
        menu.addItem("Hide track", editableTrack, false, [this, trackId]
        {
            if (onHideTrack)
                onHideTrack(trackId);
        });
        menu.addItem("Show all hidden tracks", [this] { if (onShowHiddenTracks) onShowHiddenTracks(); });
        menu.addItem("Create folder from selected tracks", editableTrack, false,
                     [this] { if (onGroupTracks) onGroupTracks(false); });
        menu.addItem("Create bus folder from selected tracks", editableTrack, false,
                     [this] { if (onGroupTracks) onGroupTracks(true); });
        const auto frozen = project->isTrackFrozen(trackId);
        menu.addItem(frozen ? "Unfreeze track" : "Freeze track",
                     editableTrack, false, [this, trackId, frozen]
        {
            if (frozen && onUnfreezeTrack)
                onUnfreezeTrack(trackId);
            else if (!frozen && onFreezeTrack)
                onFreezeTrack(trackId);
        });
        if (clickedTrack->type == TrackType::folder || clickedTrack->type == TrackType::bus
            || (clickedTrack->type == TrackType::instrument
                && std::any_of(project->tracks.begin(), project->tracks.end(),
                    [&trackId](const auto& track) { return track.folderTrackId == trackId; })))
            menu.addItem(clickedTrack->folderCollapsed ? "Expand folder" : "Collapse folder", [this, trackId]
            {
                if (onToggleFolderCollapsed)
                    onToggleFolderCollapsed(trackId);
            });
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
        deleteTrack.shortcutKeyDescription = shortcut(WorkspaceCommand::deleteSelection, "Delete");
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
        const auto hasAudioClip =
            project->findClip(selectedClipId) != nullptr;
        const auto hasMidiClip =
            project->findMidiClip(selectedClipId) != nullptr;
        if ((hasAudioClip || hasMidiClip) && project->isTrackFrozen(selectedTrackId))
        {
            menu.addSectionHeader("Frozen clip: unfreeze to edit");
            menu.addItem("Unfreeze track", [this]
            {
                if (onUnfreezeTrack)
                    onUnfreezeTrack(selectedTrackId);
            });
            return menu;
        }

        juce::PopupMenu::Item trimStart("Trim start to playhead");
        trimStart.shortcutKeyDescription = shortcut(WorkspaceCommand::trimStart, "[");
        trimStart.isEnabled = hasAudioClip;
        trimStart.action = [this]
        {
            if (onTrimStartSelected)
                onTrimStartSelected();
        };
        menu.addItem(std::move(trimStart));

        juce::PopupMenu::Item split("Split at playhead");
        split.shortcutKeyDescription = shortcut(WorkspaceCommand::split, "S");
        split.isEnabled = hasAudioClip;
        split.action = [this]
        {
            if (onSplitSelected)
                onSplitSelected();
        };
        menu.addItem(std::move(split));

        juce::PopupMenu::Item trimEnd("Trim end to playhead");
        trimEnd.shortcutKeyDescription = shortcut(WorkspaceCommand::trimEnd, "]");
        trimEnd.isEnabled = hasAudioClip;
        trimEnd.action = [this]
        {
            if (onTrimEndSelected)
                onTrimEndSelected();
        };
        menu.addItem(std::move(trimEnd));

        juce::PopupMenu::Item duplicate("Duplicate clip");
        duplicate.shortcutKeyDescription = shortcut(WorkspaceCommand::duplicateClip, "Command/Ctrl+C, Command/Ctrl+V");
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
            menu.addItem("AudioSuite: apply effects to clip range...", [this, clipId = selectedClipId]
            {
                if (onClipEffects)
                    onClipEffects(clipId);
            });
            menu.addItem("Bounce selected clips to new track", [this] { if (onBounceClips) onBounceClips(false); });
            menu.addItem("Bounce selected clips in place", [this] { if (onBounceClips) onBounceClips(true); });
            const auto safeThis = juce::Component::SafePointer<TimelineComponent>(this);
            for (const auto& [label, mode] : {
                     std::pair { "Edit clip gain...", ClipProcessingEditorComponent::Mode::gain },
                     std::pair { "Edit fade in...", ClipProcessingEditorComponent::Mode::fadeIn },
                     std::pair { "Edit fade out...", ClipProcessingEditorComponent::Mode::fadeOut } })
                menu.addItem(label, [safeThis, clipId = selectedClipId, mode, position]
                {
                    if (safeThis != nullptr)
                        safeThis->showClipProcessingEditor(clipId, mode, position);
                });
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
                          timelineSeconds = actionSeconds]
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
                          timelineSeconds = actionSeconds]
                         {
                             if (onSetFadeIn)
                                 onSetFadeIn(clipId, timelineSeconds);
                         });
            menu.addItem("Fade out from playhead",
                         true,
                         false,
                         [this,
                          clipId = selectedClipId,
                          timelineSeconds = actionSeconds]
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
        else if (hasMidiClip)
        {
            menu.addSectionHeader("MIDI");
            menu.addItem(
                "Open lower editor",
                true,
                false,
                [this]
                {
                    if (onClipSelected)
                        onClipSelected(selectedTrackId, selectedClipId);
                });
            menu.addSeparator();
        }

        juce::PopupMenu::Item remove("Delete clip");
        remove.shortcutKeyDescription = shortcut(WorkspaceCommand::deleteSelection, "Delete");
        remove.isEnabled = hasClip;
        remove.action = [this]
        {
            if (onDeleteSelected)
                onDeleteSelected();
        };
        menu.addItem(std::move(remove));
    }

    updateAccessibleSelection();
    repaint();
    return menu;
}

void TimelineComponent::showContextMenu(juce::Point<float> position, bool selectFromPosition)
{
    auto menu = createContextMenu(position, selectFromPosition);
    if (menu.getNumItems() == 0)
        return;
    const auto screenPosition = localPointToGlobal(position).toInt();
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(this)
                           .withTargetScreenArea({ screenPosition.x,
                                                   screenPosition.y,
                                                   1,
                                                   1 }));
}

void TimelineComponent::showClipProcessingEditor(const juce::String& clipId,
                                                ClipProcessingEditorComponent::Mode mode,
                                                juce::Point<float> position)
{
    const auto* clip = project != nullptr ? project->findClip(clipId) : nullptr;
    if (clip == nullptr)
        return;
    auto editor = std::make_unique<ClipProcessingEditorComponent>(*clip, mode);
    const auto safeThis = juce::Component::SafePointer<TimelineComponent>(this);
    const auto safeEditor = juce::Component::SafePointer<ClipProcessingEditorComponent>(editor.get());
    editor->onApply = [safeThis, clipId, mode](double number, float curve)
    {
        const auto* current = safeThis != nullptr && safeThis->project != nullptr
            ? safeThis->project->findClip(clipId) : nullptr;
        if (current == nullptr)
            return juce::Result::fail("This clip is no longer available. Close the editor and select it again.");
        if (mode == ClipProcessingEditorComponent::Mode::gain && safeThis->onClipGainChanged)
            safeThis->onClipGainChanged(clipId, static_cast<float>(number));
        else if (mode != ClipProcessingEditorComponent::Mode::gain && safeThis->onClipFadeChanged)
        {
            if (number > current->durationSeconds)
                return juce::Result::fail("The clip duration changed. Reopen the editor before setting this fade.");
            safeThis->onClipFadeChanged(clipId, mode == ClipProcessingEditorComponent::Mode::fadeIn, number, curve);
        }
        else
            return juce::Result::fail("Clip editing is unavailable. Close and reopen this editor.");
        return juce::Result::ok();
    };
    editor->onClose = [safeThis, safeEditor]
    {
        if (safeEditor != nullptr)
            if (auto* callout = safeEditor->findParentComponentOfClass<juce::CallOutBox>())
                callout->dismiss();
        juce::MessageManager::callAsync([safeThis]
        {
            if (safeThis != nullptr && safeThis->isShowing())
                safeThis->grabKeyboardFocus();
        });
    };
    const auto screen = localPointToGlobal(position).toInt();
    juce::CallOutBox::launchAsynchronously(std::move(editor), { screen.x, screen.y, 1, 1 }, nullptr);
    juce::MessageManager::callAsync([safeEditor]
    {
        if (safeEditor != nullptr && safeEditor->isShowing())
            safeEditor->focusValue();
    });
}

juce::String TimelineComponent::markerIdAt(
    juce::Point<float> position) const
{
    if (project == nullptr
        || position.y < 0.0f
        || position.y
            >= static_cast<float>(markerLaneHeight)
        || position.x
            < static_cast<float>(
                viewportPositionX + trackHeaderWidth)
        || position.x >= static_cast<float>(getWidth()))
        return {};

    const juce::Font font(
        juce::FontOptions(10.0f, juce::Font::bold));
    for (auto index = project->markers.size(); index > 0; --index)
    {
        const auto& marker = project->markers[index - 1];
        const auto x = secondsToX(
            marker.id == draggedMarkerId
                ? markerDragPreviewSeconds
                : marker.timeSeconds);
        const auto labelWidth =
            juce::GlyphArrangement::getStringWidth(
                font,
                marker.name);
        const juce::Rectangle<float> bounds(
            x - 4.0f,
            0.0f,
            16.0f + labelWidth,
            static_cast<float>(markerLaneHeight));
        if (bounds.contains(position))
            return marker.id;
    }
    return {};
}

juce::String TimelineComponent::sectionIdAt(juce::Point<float> position) const
{
    if (project == nullptr
        || position.y < static_cast<float>(markerLaneHeight)
        || position.y >= static_cast<float>(timelineRulerTop)
        || position.x < static_cast<float>(viewportPositionX + trackHeaderWidth)
        || position.x >= static_cast<float>(getWidth()))
        return {};

    for (auto index = project->sections.size(); index > 0; --index)
    {
        const auto& section = project->sections[index - 1];
        const auto dragging =
            section.id == draggedSectionId;
        const auto startX = secondsToX(
            dragging
                ? sectionDragPreviewStart
                : section.timeSeconds);
        const auto endX = secondsToX(
            dragging
                ? sectionDragPreviewEnd
                : sectionEndSeconds(
                    index - 1,
                    xToSeconds(
                        static_cast<float>(getWidth()))));
        const juce::Rectangle<float> bounds(
            startX,
            static_cast<float>(markerLaneHeight),
            std::max(1.0f, endX - startX + 4.0f),
            static_cast<float>(sectionLaneHeight));
        if (bounds.contains(position))
            return section.id;
    }
    return {};
}

double TimelineComponent::sectionEndSeconds(
    std::size_t index,
    double fallback) const noexcept
{
    if (project == nullptr
        || index >= project->sections.size())
        return fallback;
    const auto& section = project->sections[index];
    if (section.endTimeSeconds.has_value())
        return *section.endTimeSeconds;
    return index + 1 < project->sections.size()
        ? project->sections[index + 1].timeSeconds
        : fallback;
}

std::vector<TimelineComponent::Hit> TimelineComponent::clipHits(juce::Rectangle<float> interest) const
{
    std::vector<Hit> hits;
    if (project == nullptr)
        return hits;

    const auto tracks = visibleTracks();
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
    {
        const auto& track = *tracks[trackIndex];
        const auto y = rowTops[trackIndex] + 12;
        const auto activeRow = track.id == selectedTrackId || track.id == dragOriginalTrackId;
        if (!interest.isEmpty() && !activeRow
            && !interest.intersects(juce::Rectangle<float> {
                0.0f, static_cast<float>(y), static_cast<float>(getWidth()), trackHeight - 24.0f }))
            continue;
        for (const auto& clip : track.clips)
        {
            const auto x = secondsToX(clip.startSeconds);
            const auto width = static_cast<float>(std::max(20.0, clip.durationSeconds * pixelsPerSecond));
            const juce::Rectangle<float> bounds { x, static_cast<float>(y), width, trackHeight - 24.0f };
            if (interest.isEmpty() || bounds.expanded(8.0f).intersects(interest)
                || clip.id == selectedClipId || clip.id == draggedClipId || clip.id == hoveredClipId)
                hits.push_back({ track.id, clip.id, bounds, false, clip.startSeconds });
        }
        for (const auto& clip : track.midiClips)
        {
            const auto startSeconds = project->secondsAtBeat(
                clip.startBeats);
            const auto endSeconds = project->secondsAtBeat(
                clip.endBeats());
            const auto x = secondsToX(startSeconds);
            const auto width = static_cast<float>(
                std::max(
                    20.0,
                    (endSeconds - startSeconds) * pixelsPerSecond));
            const juce::Rectangle<float> bounds { x, static_cast<float>(y), width, trackHeight - 24.0f };
            if (!interest.isEmpty() && !bounds.expanded(8.0f).intersects(interest)
                && clip.id != selectedClipId)
                continue;
            hits.push_back({
                track.id,
                clip.id,
                {
                    x,
                    static_cast<float>(y),
                    width,
                    trackHeight - 24.0f
                },
                true,
                startSeconds
            });
        }
    }

    return hits;
}

const std::vector<const Track*>& TimelineComponent::visibleTracks() const
{
    return visibleTrackCache;
}

void TimelineComponent::rebuildRows()
{
    visibleTrackCache = project != nullptr ? ArrangementEditing::visibleTracks(*project)
                                          : std::vector<const Track*> {};
    rowTops.clear();
    rowIndices.clear();
    automationRows.clear();
    rowsEnd = rulerHeight;
    if (project != nullptr)
        for (const auto& lane : project->automationLanes)
            automationRows[lane.target.trackId].push_back(&lane);
    for (std::size_t index = 0; index < visibleTrackCache.size(); ++index)
    {
        const auto& id = visibleTrackCache[index]->id;
        rowIndices.emplace(id, index);
        rowTops.push_back(rowsEnd);
        rowsEnd += trackHeight + (automationVisible
            ? static_cast<int>(automationRows[id].size()) * automationLaneHeight : 0);
    }
}

int TimelineComponent::trackIndexAt(float y) const noexcept
{
    if (y < rulerHeight || y >= static_cast<float>(rowsEnd))
        return -1;
    return static_cast<int>(std::distance(rowTops.begin(),
        std::upper_bound(rowTops.begin(), rowTops.end(), static_cast<int>(y)))) - 1;
}

float TimelineComponent::trackY(const juce::String& trackId) const noexcept
{
    const auto found = rowIndices.find(trackId);
    return static_cast<float>(found == rowIndices.end() ? rulerHeight : rowTops[found->second]);
}

int TimelineComponent::trackRowHeight(const juce::String& id) const
{
    const auto found = rowIndices.find(id);
    if (found == rowIndices.end())
        return trackHeight;
    const auto index = found->second;
    return (index + 1 < rowTops.size() ? rowTops[index + 1] : rowsEnd) - rowTops[index];
}

void TimelineComponent::drawClipWaveform(juce::Graphics& graphics,
                                         const AudioClip& clip,
                                         juce::Rectangle<float> bounds,
                                         float alpha)
{
    graphics.saveState();
    graphics.reduceClipRegion(bounds.toNearestInt().reduced(5));
    const auto visibleBounds = graphics.getClipBounds().toFloat();
    const auto middle = bounds.getCentreY() + 8.0f;
    const auto waveformWidth = std::max(1.0f, bounds.getWidth() - 12.0f);
    constexpr auto columnWidth = 4.0f;
    const auto waveformStart = bounds.getX() + 8.0f;
    const auto firstVisibleColumn = std::max(
        0.0f,
        static_cast<float>(
            std::ceil((visibleBounds.getX() - waveformStart) / columnWidth)));
    const auto firstX = waveformStart + firstVisibleColumn * columnWidth;
    const auto lastX = std::min(bounds.getRight() - 4.0f,
                                visibleBounds.getRight());
    graphics.setColour(juce::Colours::white.withAlpha(alpha));
    for (float x = firstX; x < lastX; x += columnWidth)
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
    if (stretchToolEnabled && position.x >= hit.bounds.getRight() - 12.0f)
        return DragMode::stretch;

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

    auto mouseCursor = juce::MouseCursor::NormalCursor;
    if (nextMode == DragMode::gain
        || nextMode == DragMode::fadeInCurve
        || nextMode == DragMode::fadeOutCurve)
    {
        mouseCursor = juce::MouseCursor::UpDownResizeCursor;
    }
    else if (nextMode == DragMode::fadeIn
             || nextMode == DragMode::fadeOut
             || nextMode == DragMode::trimStart
             || nextMode == DragMode::trimEnd)
    {
        mouseCursor = juce::MouseCursor::LeftRightResizeCursor;
    }
    else if (nextMode == DragMode::stretch || nextMode == DragMode::slip)
        mouseCursor = juce::MouseCursor::LeftRightResizeCursor;
    else if (nextMode == DragMode::move)
    {
        mouseCursor = juce::MouseCursor::DraggingHandCursor;
    }
    setMouseCursor(mouseCursor);

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

double TimelineComponent::snappedSeconds(
    double seconds) const noexcept
{
    const auto clamped = std::max(0.0, seconds);
    if (!snapEnabled
        || project == nullptr
        || !std::isfinite(editGridBeats)
        || editGridBeats <= 0.0)
        return clamped;
    const auto beats = project->beatsAt(clamped);
    const auto snappedBeats =
        std::round(beats / editGridBeats)
        * editGridBeats;
    return std::max(
        0.0,
        project->secondsAtBeat(snappedBeats));
}

float TimelineComponent::secondsToX(double seconds) const noexcept
{
    return static_cast<float>(trackHeaderWidth + seconds * pixelsPerSecond);
}
}
