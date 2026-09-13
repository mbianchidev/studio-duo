#include "LinkedEditModel.h"

#include <algorithm>
#include <limits>

namespace studio
{
const AudioClip* activeClipAt(const Project& project,
                              const juce::String& parentTrackId,
                              double seconds)
{
    const auto* parent = project.findTrack(parentTrackId);
    if (parent == nullptr)
        return nullptr;

    juce::String sourceTrackId;
    const auto compRegion = std::find_if(
        parent->compRegions.cbegin(),
        parent->compRegions.cend(),
        [seconds](const auto& region)
        {
            return seconds >= region.startSeconds - 0.0001
                && seconds < region.endSeconds() + 0.0001;
        });
    if (compRegion != parent->compRegions.cend())
        sourceTrackId = compRegion->sourceTrackId;
    else
        sourceTrackId = project.activeTakeTrackId(parentTrackId);
    if (sourceTrackId.isEmpty())
        sourceTrackId = parentTrackId;

    const auto* source = project.findTrack(sourceTrackId);
    if (source == nullptr)
        return nullptr;
    const auto clip = std::find_if(
        source->clips.cbegin(),
        source->clips.cend(),
        [seconds](const auto& candidate)
        {
            return seconds >= candidate.startSeconds - 0.0001
                && seconds < candidate.endSeconds() + 0.0001;
        });
    return clip == source->clips.cend() ? nullptr : &*clip;
}

LinkedClipSelection linkedClipsAt(const Project& project,
                                  const juce::String& clipId,
                                  double seconds)
{
    LinkedClipSelection selection;
    const auto* selected = project.findClip(clipId);
    const auto* selectedTrack =
        project.findTrackContainingClip(clipId);
    if (selected == nullptr || selectedTrack == nullptr)
        return selection;

    const auto* group = project.editGroupForTrack(selectedTrack->id);
    if (group == nullptr || !group->enabled)
    {
        selection.expectedClipCount = 1;
        selection.clipIds.push_back(clipId);
        return selection;
    }

    selection.expectedClipCount = group->trackIds.size();
    const auto selectedRoot = project.rootTrackId(selectedTrack->id);
    for (const auto& rootTrackId : group->trackIds)
    {
        if (rootTrackId == selectedRoot)
        {
            selection.clipIds.push_back(clipId);
            continue;
        }
        if (const auto* clip = activeClipAt(project,
                                            rootTrackId,
                                            seconds))
            selection.clipIds.push_back(clip->id);
    }
    return selection;
}

double clampLinkedMoveDelta(const Project& project,
                            const LinkedClipSelection& selection,
                            double requestedDelta)
{
    auto earliestStart = std::numeric_limits<double>::max();
    auto foundClip = false;
    for (const auto& clipId : selection.clipIds)
        if (const auto* clip = project.findClip(clipId))
        {
            earliestStart = std::min(earliestStart,
                                     clip->startSeconds);
            foundClip = true;
        }

    return !foundClip
        ? requestedDelta
        : std::max(requestedDelta, -earliestStart);
}
}
