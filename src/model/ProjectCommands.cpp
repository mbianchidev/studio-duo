#include "ProjectCommands.h"

#include <algorithm>
#include <numeric>

namespace studio
{
bool CommandStack::perform(std::unique_ptr<ProjectCommand> command,
                           Project& project,
                           juce::String& error)
{
    if (command == nullptr || !command->perform(project, error))
        return false;

    commands.erase(commands.begin() + static_cast<std::ptrdiff_t>(nextCommand), commands.end());
    commands.push_back(std::move(command));
    nextCommand = commands.size();
    return true;
}

bool CommandStack::undo(Project& project)
{
    if (!canUndo())
        return false;

    commands[--nextCommand]->undo(project);
    return true;
}

bool CommandStack::redo(Project& project, juce::String& error)
{
    if (!canRedo() || !commands[nextCommand]->perform(project, error))
        return false;

    ++nextCommand;
    return true;
}

void CommandStack::clear()
{
    commands.clear();
    nextCommand = 0;
}

bool CommandStack::canUndo() const noexcept
{
    return nextCommand > 0;
}

bool CommandStack::canRedo() const noexcept
{
    return nextCommand < commands.size();
}

juce::String CommandStack::undoName() const
{
    return canUndo() ? commands[nextCommand - 1]->name() : juce::String();
}

juce::String CommandStack::redoName() const
{
    return canRedo() ? commands[nextCommand]->name() : juce::String();
}

BatchProjectCommand::BatchProjectCommand(
    juce::String name,
    std::vector<std::unique_ptr<ProjectCommand>> commandsToRun)
    : commandName(std::move(name)),
      commands(std::move(commandsToRun))
{
}

juce::String BatchProjectCommand::name() const
{
    return commandName;
}

bool BatchProjectCommand::perform(Project& project, juce::String& error)
{
    auto completed = std::size_t { 0 };
    for (; completed < commands.size(); ++completed)
    {
        if (commands[completed] != nullptr
            && commands[completed]->perform(project, error))
            continue;

        while (completed > 0)
            commands[--completed]->undo(project);
        if (error.isEmpty())
            error = "A linked edit command could not be completed.";
        return false;
    }
    return true;
}

void BatchProjectCommand::undo(Project& project)
{
    for (auto iterator = commands.rbegin(); iterator != commands.rend(); ++iterator)
        if (*iterator != nullptr)
            (*iterator)->undo(project);
}

AddTrackCommand::AddTrackCommand(Track trackToAdd)
    : track(std::move(trackToAdd))
{
}

juce::String AddTrackCommand::name() const
{
    return "Add track";
}

bool AddTrackCommand::perform(Project& project, juce::String& error)
{
    if (project.findTrack(track.id) != nullptr)
    {
        error = "A track with the same ID already exists.";
        return false;
    }

    if (track.parentTrackId.isNotEmpty())
    {
        const auto parent = std::find_if(project.tracks.begin(),
                                         project.tracks.end(),
                                         [this](const auto& candidate)
        {
            return candidate.id == track.parentTrackId;
        });
        if (parent == project.tracks.end() || parent->type != TrackType::audio)
        {
            error = "A version lane requires an existing audio parent track.";
            return false;
        }

        auto insertion = parent + 1;
        while (insertion != project.tracks.end()
               && insertion->parentTrackId == track.parentTrackId)
            ++insertion;
        insertionIndex = static_cast<std::size_t>(std::distance(project.tracks.begin(), insertion));
        project.tracks.insert(insertion, track);
        return true;
    }

    const auto master = std::find_if(project.tracks.begin(), project.tracks.end(), [](const auto& candidate)
    {
        return candidate.type == TrackType::master;
    });

    insertionIndex = static_cast<std::size_t>(std::distance(project.tracks.begin(), master));
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(insertionIndex), track);
    return true;
}

void AddTrackCommand::undo(Project& project)
{
    project.tracks.erase(std::remove_if(project.tracks.begin(), project.tracks.end(), [this](const auto& candidate)
    {
        return candidate.id == track.id;
    }), project.tracks.end());
}

RenameTrackCommand::RenameTrackCommand(juce::String trackToRename,
                                       juce::String replacementName)
    : trackId(std::move(trackToRename)),
      newName(std::move(replacementName).trim())
{
}

juce::String RenameTrackCommand::name() const
{
    return "Rename track";
}

bool RenameTrackCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The track to rename no longer exists.";
        return false;
    }
    if (newName.isEmpty())
    {
        error = "A track name cannot be empty.";
        return false;
    }

    if (!capturedOriginal)
    {
        oldName = track->name;
        capturedOriginal = true;
    }
    track->name = newName;
    return true;
}

void RenameTrackCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        track->name = oldName;
}

AddClipCommand::AddClipCommand(juce::String destinationTrackId, AudioClip clipToAdd)
    : trackId(std::move(destinationTrackId)),
      clip(std::move(clipToAdd))
{
}

juce::String AddClipCommand::name() const
{
    return "Add clip";
}

bool AddClipCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr || track->type == TrackType::master)
    {
        error = "The destination audio track is unavailable.";
        return false;
    }

    if (project.findClip(clip.id) != nullptr)
    {
        error = "A clip with the same ID already exists.";
        return false;
    }

    insertionIndex = track->clips.size();
    track->clips.push_back(clip);
    return true;
}

void AddClipCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(), [this](const auto& candidate)
        {
            return candidate.id == clip.id;
        }), track->clips.end());
}

AddRecordingTakeCommand::AddRecordingTakeCommand(std::vector<Track> tracksToAdd)
    : tracks(std::move(tracksToAdd))
{
}

juce::String AddRecordingTakeCommand::name() const
{
    return tracks.size() == 1 ? "Record take" : "Record multitrack take";
}

bool AddRecordingTakeCommand::perform(Project& project, juce::String& error)
{
    if (tracks.empty())
    {
        error = "A recording take must contain at least one track.";
        return false;
    }

    for (std::size_t index = 0; index < tracks.size(); ++index)
    {
        const auto& track = tracks[index];
        const auto* parent = project.findTrack(track.parentTrackId);
        if (track.type != TrackType::audio
            || track.parentTrackId.isEmpty()
            || parent == nullptr
            || parent->type != TrackType::audio)
        {
            error = "Every recording take requires an existing audio parent track.";
            return false;
        }
        if (track.versionNumber <= 0 || track.clips.size() != 1)
        {
            error = "Every recording take requires one version number and one audio clip.";
            return false;
        }
        if (project.findTrack(track.id) != nullptr)
        {
            error = "A recording take track with the same ID already exists.";
            return false;
        }

        for (std::size_t otherIndex = 0; otherIndex < tracks.size(); ++otherIndex)
        {
            if (index == otherIndex)
                continue;

            const auto& other = tracks[otherIndex];
            if (track.id == other.id
                || (track.parentTrackId == other.parentTrackId
                    && track.versionNumber == other.versionNumber))
            {
                error = "Recording take tracks must have unique IDs and version numbers.";
                return false;
            }
        }

        const auto versionExists = std::any_of(
            project.tracks.cbegin(),
            project.tracks.cend(),
            [&track](const auto& candidate)
            {
                return candidate.parentTrackId == track.parentTrackId
                    && candidate.versionNumber == track.versionNumber;
            });
        if (versionExists)
        {
            error = "A recording take with the same version number already exists.";
            return false;
        }
    }

    if (!capturedOriginal)
    {
        for (const auto& track : tracks)
        {
            if (std::none_of(parentCollapseStates.cbegin(),
                             parentCollapseStates.cend(),
                             [&track](const auto& state)
                             {
                                 return state.first == track.parentTrackId;
                             }))
            {
                parentCollapseStates.emplace_back(
                    track.parentTrackId,
                    project.findTrack(track.parentTrackId)->versionsCollapsed);
            }
        }
        capturedOriginal = true;
    }

    for (const auto& track : tracks)
    {
        const auto parent = std::find_if(project.tracks.begin(),
                                         project.tracks.end(),
                                         [&track](const auto& candidate)
        {
            return candidate.id == track.parentTrackId;
        });
        auto insertion = parent + 1;
        while (insertion != project.tracks.end()
               && insertion->parentTrackId == track.parentTrackId)
            ++insertion;
        project.tracks.insert(insertion, track);
    }

    for (const auto& [parentId, ignored] : parentCollapseStates)
        if (auto* parent = project.findTrack(parentId))
            parent->versionsCollapsed = false;

    return true;
}

void AddRecordingTakeCommand::undo(Project& project)
{
    project.tracks.erase(
        std::remove_if(project.tracks.begin(),
                       project.tracks.end(),
                       [this](const auto& candidate)
                       {
                           return std::any_of(tracks.cbegin(),
                                              tracks.cend(),
                                              [&candidate](const auto& recorded)
                                              {
                                                  return candidate.id == recorded.id;
                                              });
                       }),
        project.tracks.end());

    for (const auto& [parentId, collapsed] : parentCollapseStates)
        if (auto* parent = project.findTrack(parentId))
            parent->versionsCollapsed = collapsed;
}

MoveClipCommand::MoveClipCommand(juce::String clipToMove,
                                 double destinationSeconds,
                                 juce::String destinationTrackId)
    : clipId(std::move(clipToMove)),
      newTrackId(std::move(destinationTrackId)),
      newStartSeconds(std::max(0.0, destinationSeconds))
{
}

juce::String MoveClipCommand::name() const
{
    return "Move clip";
}

bool MoveClipCommand::perform(Project& project, juce::String& error)
{
    auto* sourceTrack = project.findTrackContainingClip(clipId);
    if (sourceTrack == nullptr)
    {
        error = "The clip to move no longer exists.";
        return false;
    }

    auto* destinationTrack = newTrackId.isEmpty()
        ? sourceTrack
        : project.findTrack(newTrackId);
    if (destinationTrack == nullptr || destinationTrack->type != TrackType::audio)
    {
        error = "Audio clips can only be moved to an audio track.";
        return false;
    }

    const auto sourceIterator = std::find_if(sourceTrack->clips.begin(),
                                             sourceTrack->clips.end(),
                                             [this](const auto& candidate)
    {
        return candidate.id == clipId;
    });
    if (sourceIterator == sourceTrack->clips.end())
    {
        error = "The clip to move no longer exists.";
        return false;
    }

    if (!capturedOriginal)
    {
        oldTrackId = sourceTrack->id;
        oldClipIndex = static_cast<std::size_t>(std::distance(sourceTrack->clips.begin(),
                                                              sourceIterator));
        oldStartSeconds = sourceIterator->startSeconds;
        if (newTrackId.isEmpty())
            newTrackId = sourceTrack->id;
        capturedOriginal = true;
    }

    if (sourceTrack == destinationTrack)
    {
        sourceIterator->startSeconds = newStartSeconds;
        return true;
    }

    auto movedClip = *sourceIterator;
    movedClip.startSeconds = newStartSeconds;
    sourceTrack->clips.erase(sourceIterator);
    destinationTrack->clips.push_back(std::move(movedClip));
    return true;
}

void MoveClipCommand::undo(Project& project)
{
    auto* currentTrack = project.findTrackContainingClip(clipId);
    auto* originalTrack = project.findTrack(oldTrackId);
    if (currentTrack == nullptr || originalTrack == nullptr)
        return;

    const auto iterator = std::find_if(currentTrack->clips.begin(),
                                       currentTrack->clips.end(),
                                       [this](const auto& candidate)
    {
        return candidate.id == clipId;
    });
    if (iterator == currentTrack->clips.end())
        return;

    if (currentTrack == originalTrack)
    {
        iterator->startSeconds = oldStartSeconds;
        return;
    }

    auto movedClip = *iterator;
    movedClip.startSeconds = oldStartSeconds;
    currentTrack->clips.erase(iterator);
    const auto index = std::min(oldClipIndex, originalTrack->clips.size());
    originalTrack->clips.insert(originalTrack->clips.begin() + static_cast<std::ptrdiff_t>(index),
                                std::move(movedClip));
}

SetClipStateCommand::SetClipStateCommand(juce::String targetTrackId,
                                         AudioClip before,
                                         AudioClip after,
                                         juce::String name)
    : trackId(std::move(targetTrackId)),
      oldClip(std::move(before)),
      newClip(std::move(after)),
      commandName(std::move(name))
{
}

juce::String SetClipStateCommand::name() const
{
    return commandName;
}

bool SetClipStateCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr || oldClip.id != newClip.id)
    {
        error = "The clip processing target is unavailable.";
        return false;
    }
    const auto clip = std::find_if(track->clips.begin(),
                                   track->clips.end(),
                                   [this](const auto& candidate)
    {
        return candidate.id == newClip.id;
    });
    if (clip == track->clips.end())
    {
        error = "The clip processing target no longer exists.";
        return false;
    }
    *clip = newClip;
    return true;
}

void SetClipStateCommand::undo(Project& project)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    const auto clip = std::find_if(track->clips.begin(),
                                   track->clips.end(),
                                   [this](const auto& candidate)
    {
        return candidate.id == oldClip.id;
    });
    if (clip != track->clips.end())
        *clip = oldClip;
}

TrimClipCommand::TrimClipCommand(juce::String clipToTrim,
                                 double destinationStartSeconds,
                                 double destinationSourceOffsetSeconds,
                                 double destinationDurationSeconds)
    : clipId(std::move(clipToTrim)),
      newStartSeconds(std::max(0.0, destinationStartSeconds)),
      newSourceOffsetSeconds(std::max(0.0, destinationSourceOffsetSeconds)),
      newDurationSeconds(destinationDurationSeconds)
{
}

juce::String TrimClipCommand::name() const
{
    return "Trim clip";
}

bool TrimClipCommand::perform(Project& project, juce::String& error)
{
    auto* clip = project.findClip(clipId);
    if (clip == nullptr)
    {
        error = "The clip to trim no longer exists.";
        return false;
    }
    if (newDurationSeconds < 0.001)
    {
        error = "A clip must remain at least one millisecond long.";
        return false;
    }
    if (newSourceOffsetSeconds < clip->sourceRangeStartSeconds - 0.0001
        || newSourceOffsetSeconds + newDurationSeconds > clip->sourceRangeEnd() + 0.0001)
    {
        error = "The trim extends beyond this clip's recoverable source range.";
        return false;
    }

    if (!capturedOriginal)
    {
        originalClip = *clip;
        capturedOriginal = true;
    }

    auto updated = originalClip;
    const auto startDelta = newStartSeconds - originalClip.startSeconds;
    updated.startSeconds = newStartSeconds;
    updated.sourceOffsetSeconds = newSourceOffsetSeconds;
    updated.durationSeconds = newDurationSeconds;
    if (std::abs(startDelta) > 0.0001)
    {
        for (auto& marker : updated.warpMarkers)
            marker.timelineOffsetSeconds -= startDelta;
        updated.warpMarkers.erase(
            std::remove_if(updated.warpMarkers.begin(),
                           updated.warpMarkers.end(),
                           [](const auto& marker)
                           {
                               return marker.timelineOffsetSeconds <= 0.0;
                           }),
            updated.warpMarkers.end());
        updated.fadeInSeconds = std::max(0.0,
                                         originalClip.fadeInSeconds - startDelta);
    }
    updated.warpMarkers.erase(
        std::remove_if(updated.warpMarkers.begin(),
                       updated.warpMarkers.end(),
                       [&updated](const auto& marker)
                       {
                           return marker.timelineOffsetSeconds
                               >= updated.durationSeconds;
                       }),
        updated.warpMarkers.end());
    updated.fadeInSeconds = std::min(updated.fadeInSeconds,
                                     updated.durationSeconds);
    updated.fadeOutSeconds = std::min(updated.fadeOutSeconds,
                                      updated.durationSeconds);
    *clip = std::move(updated);
    return true;
}

void TrimClipCommand::undo(Project& project)
{
    if (auto* clip = project.findClip(clipId))
        *clip = originalClip;
}

SplitClipCommand::SplitClipCommand(juce::String clipToSplit, double splitPositionSeconds)
    : clipId(std::move(clipToSplit)),
      splitSeconds(splitPositionSeconds)
{
}

juce::String SplitClipCommand::name() const
{
    return "Split clip";
}

bool SplitClipCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrackContainingClip(clipId);
    auto* clip = project.findClip(clipId);
    if (track == nullptr || clip == nullptr)
    {
        error = "The clip to split no longer exists.";
        return false;
    }

    if (splitSeconds <= clip->startSeconds + 0.001 || splitSeconds >= clip->endSeconds() - 0.001)
    {
        error = "Move the playhead inside the selected clip before splitting.";
        return false;
    }
    if (clip->reversed)
    {
        error = "Consolidate a reversed clip before splitting it.";
        return false;
    }

    const auto iterator = std::find_if(track->clips.begin(), track->clips.end(), [this](const auto& candidate)
    {
        return candidate.id == clipId;
    });

    if (!capturedOriginal)
    {
        original = *clip;
        trackId = track->id;
        clipIndex = static_cast<std::size_t>(std::distance(track->clips.begin(), iterator));
        rightSide = original;
        rightSide.id = juce::Uuid().toString();
        rightSide.name = original.name + " B";
        capturedOriginal = true;
    }

    const auto leftDuration = splitSeconds - original.startSeconds;
    const auto splitSourceSeconds = original.sourceSecondsAt(leftDuration);
    auto leftSide = original;
    leftSide.durationSeconds = leftDuration;
    leftSide.sourceRangeEndSeconds = splitSourceSeconds;
    leftSide.fadeOutSeconds = 0.0;
    leftSide.warpMarkers.erase(
        std::remove_if(leftSide.warpMarkers.begin(),
                       leftSide.warpMarkers.end(),
                       [leftDuration](const auto& marker)
                       {
                           return marker.timelineOffsetSeconds
                               >= leftDuration;
                       }),
        leftSide.warpMarkers.end());
    const auto rightId = rightSide.id;
    rightSide = original;
    rightSide.id = rightId;
    rightSide.name = original.name + " B";
    rightSide.startSeconds = splitSeconds;
    rightSide.sourceOffsetSeconds = splitSourceSeconds;
    rightSide.sourceRangeStartSeconds = splitSourceSeconds;
    rightSide.sourceRangeEndSeconds = original.sourceRangeEnd();
    rightSide.durationSeconds = original.durationSeconds - leftDuration;
    rightSide.fadeInSeconds = 0.0;
    for (auto& marker : rightSide.warpMarkers)
        marker.timelineOffsetSeconds -= leftDuration;
    rightSide.warpMarkers.erase(
        std::remove_if(rightSide.warpMarkers.begin(),
                       rightSide.warpMarkers.end(),
                       [this](const auto& marker)
                       {
                           return marker.timelineOffsetSeconds <= 0.0
                               || marker.timelineOffsetSeconds
                                      >= rightSide.durationSeconds;
                       }),
        rightSide.warpMarkers.end());

    *iterator = std::move(leftSide);
    track->clips.insert(iterator + 1, rightSide);
    return true;
}

void SplitClipCommand::undo(Project& project)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return;

    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(), [this](const auto& candidate)
    {
        return candidate.id == clipId || candidate.id == rightSide.id;
    }), track->clips.end());

    const auto index = std::min(clipIndex, track->clips.size());
    track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(index), original);
}

DeleteClipCommand::DeleteClipCommand(juce::String clipToDelete)
    : clipId(std::move(clipToDelete))
{
}

juce::String DeleteClipCommand::name() const
{
    return "Delete clip";
}

bool DeleteClipCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrackContainingClip(clipId);
    if (track == nullptr)
    {
        error = "The clip to delete no longer exists.";
        return false;
    }

    const auto iterator = std::find_if(track->clips.begin(), track->clips.end(), [this](const auto& candidate)
    {
        return candidate.id == clipId;
    });

    if (!capturedOriginal)
    {
        deletedClip = *iterator;
        trackId = track->id;
        clipIndex = static_cast<std::size_t>(std::distance(track->clips.begin(), iterator));
        capturedOriginal = true;
    }

    track->clips.erase(iterator);
    return true;
}

void DeleteClipCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
    {
        const auto index = std::min(clipIndex, track->clips.size());
        track->clips.insert(track->clips.begin() + static_cast<std::ptrdiff_t>(index), deletedClip);
    }
}

SetActiveTakeCommand::SetActiveTakeCommand(juce::String parentTrackId,
                                           juce::String activeTakeTrackId)
    : parentId(std::move(parentTrackId)),
      newActiveTakeId(std::move(activeTakeTrackId))
{
}

juce::String SetActiveTakeCommand::name() const
{
    return "Select active take";
}

bool SetActiveTakeCommand::perform(Project& project, juce::String& error)
{
    auto* parent = project.findTrack(parentId);
    if (parent == nullptr || parent->parentTrackId.isNotEmpty())
    {
        error = "The take parent track is unavailable.";
        return false;
    }
    if (newActiveTakeId.isNotEmpty())
    {
        const auto* take = project.findTrack(newActiveTakeId);
        if (take == nullptr || take->parentTrackId != parentId)
        {
            error = "The selected take does not belong to this parent track.";
            return false;
        }
    }

    if (!capturedOriginal)
    {
        oldActiveTakeId = parent->activeTakeTrackId;
        capturedOriginal = true;
    }
    parent->activeTakeTrackId = newActiveTakeId;
    return true;
}

void SetActiveTakeCommand::undo(Project& project)
{
    if (auto* parent = project.findTrack(parentId))
        parent->activeTakeTrackId = oldActiveTakeId;
}

SetCompRegionsCommand::SetCompRegionsCommand(juce::String parentTrackId,
                                             std::vector<CompRegion> before,
                                             std::vector<CompRegion> after)
    : parentId(std::move(parentTrackId)),
      oldRegions(std::move(before)),
      newRegions(std::move(after))
{
}

juce::String SetCompRegionsCommand::name() const
{
    return "Change comp";
}

bool SetCompRegionsCommand::perform(Project& project, juce::String& error)
{
    if (!validate(project, parentId, newRegions, error))
        return false;
    project.findTrack(parentId)->compRegions = newRegions;
    return true;
}

void SetCompRegionsCommand::undo(Project& project)
{
    if (auto* parent = project.findTrack(parentId))
        parent->compRegions = oldRegions;
}

bool SetCompRegionsCommand::validate(const Project& project,
                                     const juce::String& parentId,
                                     const std::vector<CompRegion>& regions,
                                     juce::String& error)
{
    const auto* parent = project.findTrack(parentId);
    if (parent == nullptr || parent->parentTrackId.isNotEmpty())
    {
        error = "The comp parent track is unavailable.";
        return false;
    }

    auto previousEnd = 0.0;
    for (const auto& region : regions)
    {
        const auto* take = project.findTrack(region.sourceTrackId);
        if (region.id.isEmpty()
            || region.durationSeconds <= 0.0
            || region.startSeconds < previousEnd - 0.0001
            || take == nullptr
            || take->parentTrackId != parentId)
        {
            error = "Comp regions must be ordered, non-overlapping, and reference a take lane.";
            return false;
        }
        previousEnd = region.endSeconds();
    }
    return true;
}

SetEditGroupsCommand::SetEditGroupsCommand(std::vector<EditGroup> before,
                                           std::vector<EditGroup> after)
    : oldGroups(std::move(before)),
      newGroups(std::move(after))
{
}

juce::String SetEditGroupsCommand::name() const
{
    return "Change edit groups";
}

bool SetEditGroupsCommand::perform(Project& project, juce::String& error)
{
    if (!validate(project, newGroups, error))
        return false;
    project.editGroups = newGroups;
    return true;
}

void SetEditGroupsCommand::undo(Project& project)
{
    project.editGroups = oldGroups;
}

bool SetEditGroupsCommand::validate(const Project& project,
                                    const std::vector<EditGroup>& groups,
                                    juce::String& error)
{
    std::vector<juce::String> assignedTracks;
    for (const auto& group : groups)
    {
        if (group.id.isEmpty()
            || group.name.trim().isEmpty()
            || group.trackIds.size() < 2
            || group.quantizeStrength < 0.0
            || group.quantizeStrength > 1.0)
        {
            error = "Edit groups need a name, at least two tracks, and valid quantize strength.";
            return false;
        }

        for (const auto& trackId : group.trackIds)
        {
            const auto* track = project.findTrack(trackId);
            if (track == nullptr
                || track->type != TrackType::audio
                || track->parentTrackId.isNotEmpty()
                || std::find(assignedTracks.cbegin(),
                             assignedTracks.cend(),
                             trackId) != assignedTracks.cend())
            {
                error = "Edit group tracks must be unique audio parent tracks.";
                return false;
            }
            assignedTracks.push_back(trackId);
        }

        if (std::find(group.trackIds.cbegin(),
                      group.trackIds.cend(),
                      group.timingReferenceTrackId) == group.trackIds.cend())
        {
            error = "The timing reference must belong to its edit group.";
            return false;
        }
        if (std::any_of(group.protectedAnchorsSeconds.cbegin(),
                        group.protectedAnchorsSeconds.cend(),
                        [](double anchor)
                        {
                            return anchor < 0.0;
                        }))
        {
            error = "Protected edit anchors cannot be negative.";
            return false;
        }
    }
    return true;
}

SetReampRoutesCommand::SetReampRoutesCommand(std::vector<ReampRoute> before,
                                             std::vector<ReampRoute> after)
    : oldRoutes(std::move(before)),
      newRoutes(std::move(after))
{
}

juce::String SetReampRoutesCommand::name() const
{
    return "Change reamp routes";
}

bool SetReampRoutesCommand::perform(Project& project, juce::String& error)
{
    if (!validate(project, newRoutes, error))
        return false;
    project.reampRoutes = newRoutes;
    return true;
}

void SetReampRoutesCommand::undo(Project& project)
{
    project.reampRoutes = oldRoutes;
}

bool SetReampRoutesCommand::validate(const Project& project,
                                     const std::vector<ReampRoute>& routes,
                                     juce::String& error)
{
    std::vector<juce::String> returnTracks;
    for (const auto& route : routes)
    {
        const auto* source = project.findTrack(route.sourceTrackId);
        const auto* returnTrack = project.findTrack(route.returnTrackId);
        if (route.id.isEmpty()
            || route.name.trim().isEmpty()
            || source == nullptr
            || returnTrack == nullptr
            || source->type != TrackType::audio
            || returnTrack->type != TrackType::audio
            || source->parentTrackId.isNotEmpty()
            || returnTrack->parentTrackId.isNotEmpty()
            || source->id == returnTrack->id
            || route.outputChannel < 0
            || route.inputChannel < 0
            || route.latencySamples < 0
            || std::find(returnTracks.cbegin(),
                         returnTracks.cend(),
                         route.returnTrackId) != returnTracks.cend())
        {
            error = "Reamp routes need unique audio returns and valid hardware channels.";
            return false;
        }
        returnTracks.push_back(route.returnTrackId);
    }
    return true;
}

ProjectTransportState ProjectTransportState::fromProject(const Project& project)
{
    return {
        project.tempo,
        project.timeSignatureNumerator,
        project.timeSignatureDenominator,
        project.tempoChanges,
        project.meterChanges,
        project.metronomeEnabled,
        project.metronomeSubdivision,
        project.metronomeOutputChannel,
        project.metronomeLevel,
        project.metronomeAccentLevel,
        project.punchEnabled,
        project.punchInSeconds,
        project.punchOutSeconds,
        project.countInBars,
        project.preRollSeconds,
        project.postRollSeconds,
        project.loopEnabled,
        project.loopStartSeconds,
        project.loopEndSeconds
    };
}

SetProjectTransportCommand::SetProjectTransportCommand(ProjectTransportState before,
                                                       ProjectTransportState after)
    : oldState(std::move(before)),
      newState(std::move(after))
{
}

juce::String SetProjectTransportCommand::name() const
{
    return "Change transport settings";
}

bool SetProjectTransportCommand::perform(Project& project, juce::String& error)
{
    if (newState.tempo < 20.0
        || newState.tempo > 400.0
        || newState.timeSignatureNumerator < 1
        || newState.timeSignatureNumerator > 32
        || (newState.timeSignatureDenominator != 1
            && newState.timeSignatureDenominator != 2
            && newState.timeSignatureDenominator != 4
            && newState.timeSignatureDenominator != 8
            && newState.timeSignatureDenominator != 16
            && newState.timeSignatureDenominator != 32)
        || newState.punchInSeconds < 0.0
        || newState.punchOutSeconds <= newState.punchInSeconds
        || newState.loopStartSeconds < 0.0
        || newState.loopEndSeconds <= newState.loopStartSeconds
        || newState.countInBars < 0
        || newState.preRollSeconds < 0.0
        || newState.postRollSeconds < 0.0)
    {
        error = "The transport settings contain an invalid tempo or time range.";
        return false;
    }

    apply(project, newState);
    return true;
}

void SetProjectTransportCommand::undo(Project& project)
{
    apply(project, oldState);
}

void SetProjectTransportCommand::apply(Project& project,
                                       const ProjectTransportState& state)
{
    project.tempo = state.tempo;
    project.timeSignatureNumerator = state.timeSignatureNumerator;
    project.timeSignatureDenominator = state.timeSignatureDenominator;
    project.tempoChanges = state.tempoChanges;
    project.meterChanges = state.meterChanges;
    project.metronomeEnabled = state.metronomeEnabled;
    project.metronomeSubdivision = state.metronomeSubdivision;
    project.metronomeOutputChannel = state.metronomeOutputChannel;
    project.metronomeLevel = state.metronomeLevel;
    project.metronomeAccentLevel = state.metronomeAccentLevel;
    project.punchEnabled = state.punchEnabled;
    project.punchInSeconds = state.punchInSeconds;
    project.punchOutSeconds = state.punchOutSeconds;
    project.countInBars = state.countInBars;
    project.preRollSeconds = state.preRollSeconds;
    project.postRollSeconds = state.postRollSeconds;
    project.loopEnabled = state.loopEnabled;
    project.loopStartSeconds = state.loopStartSeconds;
    project.loopEndSeconds = state.loopEndSeconds;
}

TrackMixState TrackMixState::fromTrack(const Track& track)
{
    return {
        track.volumeDecibels,
        track.pan,
        track.muted,
        track.solo,
        track.armed,
        track.inputChannel,
        track.stereoInput,
        track.inputMonitoring,
        track.versionsCollapsed,
        track.colour
    };
}

SetTrackMixCommand::SetTrackMixCommand(juce::String trackToChange,
                                       TrackMixState before,
                                       TrackMixState after)
    : trackId(std::move(trackToChange)),
      oldState(before),
      newState(after)
{
}

juce::String SetTrackMixCommand::name() const
{
    return "Change track controls";
}

bool SetTrackMixCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The track to change no longer exists.";
        return false;
    }

    apply(*track, newState);
    return true;
}

void SetTrackMixCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        apply(*track, oldState);
}

void SetTrackMixCommand::apply(Track& track, const TrackMixState& state)
{
    track.volumeDecibels = state.volumeDecibels;
    track.pan = state.pan;
    track.muted = state.muted;
    track.solo = state.solo;
    track.armed = state.armed;
    track.inputChannel = state.inputChannel;
    track.stereoInput = state.stereoInput;
    track.inputMonitoring = state.inputMonitoring;
    track.versionsCollapsed = state.versionsCollapsed;
    track.colour = state.colour;
}

AddPluginInsertCommand::AddPluginInsertCommand(juce::String destinationTrackId,
                                               PluginInsert insertToAdd)
    : trackId(std::move(destinationTrackId)),
      insert(std::move(insertToAdd))
{
}

juce::String AddPluginInsertCommand::name() const
{
    return "Add plugin insert";
}

bool AddPluginInsertCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The destination track no longer exists.";
        return false;
    }

    const auto duplicate = std::find_if(track->inserts.cbegin(), track->inserts.cend(), [this](const auto& candidate)
    {
        return candidate.id == insert.id;
    });
    if (duplicate != track->inserts.cend())
    {
        error = "A plugin insert with the same ID already exists.";
        return false;
    }

    if (!capturedIndex)
    {
        insertionIndex = track->inserts.size();
        capturedIndex = true;
    }
    insertionIndex = std::min(insertionIndex, track->inserts.size());
    track->inserts.insert(track->inserts.begin() + static_cast<std::ptrdiff_t>(insertionIndex), insert);
    return true;
}

void AddPluginInsertCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        track->inserts.erase(std::remove_if(track->inserts.begin(), track->inserts.end(), [this](const auto& candidate)
        {
            return candidate.id == insert.id;
        }), track->inserts.end());
}

RemovePluginInsertCommand::RemovePluginInsertCommand(juce::String sourceTrackId,
                                                     juce::String insertToRemove)
    : trackId(std::move(sourceTrackId)),
      insertId(std::move(insertToRemove))
{
}

juce::String RemovePluginInsertCommand::name() const
{
    return "Remove plugin insert";
}

bool RemovePluginInsertCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The source track no longer exists.";
        return false;
    }

    const auto iterator = std::find_if(track->inserts.begin(), track->inserts.end(), [this](const auto& candidate)
    {
        return candidate.id == insertId;
    });
    if (iterator == track->inserts.end())
    {
        error = "The plugin insert no longer exists.";
        return false;
    }

    if (!capturedOriginal)
    {
        removedInsert = *iterator;
        removalIndex = static_cast<std::size_t>(std::distance(track->inserts.begin(), iterator));
        capturedOriginal = true;
    }
    track->inserts.erase(iterator);
    return true;
}

void RemovePluginInsertCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
    {
        const auto index = std::min(removalIndex, track->inserts.size());
        track->inserts.insert(track->inserts.begin() + static_cast<std::ptrdiff_t>(index), removedInsert);
    }
}

SetPluginBypassCommand::SetPluginBypassCommand(juce::String sourceTrackId,
                                               juce::String insertToChange,
                                               bool shouldBeBypassed)
    : trackId(std::move(sourceTrackId)),
      insertId(std::move(insertToChange)),
      newBypassed(shouldBeBypassed)
{
}

juce::String SetPluginBypassCommand::name() const
{
    return newBypassed ? "Bypass plugin insert" : "Enable plugin insert";
}

bool SetPluginBypassCommand::perform(Project& project, juce::String& error)
{
    auto* insert = find(project);
    if (insert == nullptr)
    {
        error = "The plugin insert no longer exists.";
        return false;
    }

    if (!capturedOriginal)
    {
        oldBypassed = insert->bypassed;
        capturedOriginal = true;
    }
    insert->bypassed = newBypassed;
    return true;
}

void SetPluginBypassCommand::undo(Project& project)
{
    if (auto* insert = find(project))
        insert->bypassed = oldBypassed;
}

PluginInsert* SetPluginBypassCommand::find(Project& project) const
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return nullptr;

    const auto iterator = std::find_if(track->inserts.begin(), track->inserts.end(), [this](const auto& candidate)
    {
        return candidate.id == insertId;
    });
    return iterator == track->inserts.end() ? nullptr : &*iterator;
}

RemoveTrackCommand::RemoveTrackCommand(juce::String trackToRemove)
    : trackId(std::move(trackToRemove))
{
}

juce::String RemoveTrackCommand::name() const
{
    return "Remove track";
}

bool RemoveTrackCommand::perform(Project& project, juce::String& error)
{
    if (!capturedOriginal)
    {
        const auto iterator = std::find_if(project.tracks.begin(),
                                           project.tracks.end(),
                                           [this](const auto& candidate)
        {
            return candidate.id == trackId;
        });
        if (iterator == project.tracks.end())
        {
            error = "The track to remove no longer exists.";
            return false;
        }
        if (iterator->type == TrackType::master)
        {
            error = "The master track cannot be removed.";
            return false;
        }
        if (iterator->parentTrackId.isNotEmpty())
        {
            affectedParentId = iterator->parentTrackId;
            if (const auto* parent = project.findTrack(affectedParentId))
            {
                oldActiveTakeId = parent->activeTakeTrackId;
                oldCompRegions = parent->compRegions;
            }
        }
        oldEditGroups = project.editGroups;
        oldReampRoutes = project.reampRoutes;

        std::vector<juce::String> rootIdsToRemove;
        if (iterator->parentTrackId.isEmpty())
        {
            rootIdsToRemove.push_back(iterator->id);
            for (auto addedRoute = true; addedRoute;)
            {
                addedRoute = false;
                for (const auto& route : project.reampRoutes)
                {
                    if (!route.ownsReturnTrack
                        || std::find(rootIdsToRemove.cbegin(),
                                     rootIdsToRemove.cend(),
                                     route.sourceTrackId) == rootIdsToRemove.cend()
                        || std::find(rootIdsToRemove.cbegin(),
                                     rootIdsToRemove.cend(),
                                     route.returnTrackId) != rootIdsToRemove.cend())
                        continue;
                    rootIdsToRemove.push_back(route.returnTrackId);
                    addedRoute = true;
                }
            }
        }

        for (std::size_t index = 0; index < project.tracks.size(); ++index)
        {
            const auto& candidate = project.tracks[index];
            if ((iterator->parentTrackId.isNotEmpty() && candidate.id == trackId)
                || (iterator->parentTrackId.isEmpty()
                    && (std::find(rootIdsToRemove.cbegin(),
                                  rootIdsToRemove.cend(),
                                  candidate.id) != rootIdsToRemove.cend()
                        || std::find(rootIdsToRemove.cbegin(),
                                     rootIdsToRemove.cend(),
                                     candidate.parentTrackId) != rootIdsToRemove.cend())))
                removedTracks.emplace_back(index, candidate);
        }
        capturedOriginal = true;
    }

    const auto oldSize = project.tracks.size();
    project.tracks.erase(std::remove_if(project.tracks.begin(),
                                        project.tracks.end(),
                                        [this](const auto& candidate)
    {
        return std::any_of(removedTracks.cbegin(),
                           removedTracks.cend(),
                           [&candidate](const auto& removed)
        {
            return removed.second.id == candidate.id;
        });
    }), project.tracks.end());

    if (project.tracks.size() == oldSize)
    {
        error = "The track group to remove no longer exists.";
        return false;
    }
    if (auto* parent = project.findTrack(affectedParentId))
    {
        if (parent->activeTakeTrackId == trackId)
            parent->activeTakeTrackId.clear();
        parent->compRegions.erase(
            std::remove_if(parent->compRegions.begin(),
                           parent->compRegions.end(),
                           [this](const auto& region)
                           {
                               return region.sourceTrackId == trackId;
                           }),
            parent->compRegions.end());
    }
    for (auto& group : project.editGroups)
    {
        group.trackIds.erase(
            std::remove_if(group.trackIds.begin(),
                           group.trackIds.end(),
                           [this](const auto& candidateId)
                           {
                               return std::any_of(
                                   removedTracks.cbegin(),
                                   removedTracks.cend(),
                                   [&candidateId](const auto& removed)
                                   {
                                       return removed.second.id == candidateId;
                                   });
                           }),
            group.trackIds.end());
        if (std::find(group.trackIds.cbegin(),
                      group.trackIds.cend(),
                      group.timingReferenceTrackId) == group.trackIds.cend()
            && !group.trackIds.empty())
            group.timingReferenceTrackId = group.trackIds.front();
    }
    project.editGroups.erase(
        std::remove_if(project.editGroups.begin(),
                       project.editGroups.end(),
                       [](const auto& group)
                       {
                           return group.trackIds.size() < 2;
                       }),
        project.editGroups.end());
    project.reampRoutes.erase(
        std::remove_if(project.reampRoutes.begin(),
                       project.reampRoutes.end(),
                       [this](const auto& route)
                       {
                           return std::any_of(
                               removedTracks.cbegin(),
                               removedTracks.cend(),
                               [&route](const auto& removed)
                               {
                                   return removed.second.id == route.sourceTrackId
                                       || removed.second.id == route.returnTrackId;
                               });
                       }),
        project.reampRoutes.end());
    return true;
}

void RemoveTrackCommand::undo(Project& project)
{
    for (const auto& [originalIndex, track] : removedTracks)
    {
        const auto index = std::min(originalIndex, project.tracks.size());
        project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(index), track);
    }
    if (auto* parent = project.findTrack(affectedParentId))
    {
        parent->activeTakeTrackId = oldActiveTakeId;
        parent->compRegions = oldCompRegions;
    }
    project.editGroups = oldEditGroups;
    project.reampRoutes = oldReampRoutes;
}

DuplicateTrackCommand::DuplicateTrackCommand(juce::String trackToDuplicate)
    : sourceTrackId(std::move(trackToDuplicate))
{
}

juce::String DuplicateTrackCommand::name() const
{
    return "Duplicate track";
}

bool DuplicateTrackCommand::perform(Project& project, juce::String& error)
{
    if (!createdDuplicate)
    {
        const auto* source = project.findTrack(sourceTrackId);
        if (source == nullptr)
        {
            error = "The track to duplicate no longer exists.";
            return false;
        }
        if (source->type == TrackType::master)
        {
            error = "The master track cannot be duplicated.";
            return false;
        }

        std::vector<juce::String> rootIds;
        if (source->parentTrackId.isEmpty())
        {
            rootIds.push_back(source->id);
            for (auto addedRoute = true; addedRoute;)
            {
                addedRoute = false;
                for (const auto& route : project.reampRoutes)
                {
                    if (!route.ownsReturnTrack)
                        continue;
                    const auto sourceIncluded = std::find(
                        rootIds.cbegin(),
                        rootIds.cend(),
                        route.sourceTrackId) != rootIds.cend();
                    const auto returnIncluded = std::find(
                        rootIds.cbegin(),
                        rootIds.cend(),
                        route.returnTrackId) != rootIds.cend();
                    if (sourceIncluded && !returnIncluded)
                    {
                        rootIds.push_back(route.returnTrackId);
                        addedRoute = true;
                    }
                }
            }
        }

        std::vector<Track> sourceTracks;
        std::vector<std::pair<juce::String, juce::String>> idMap;
        std::vector<juce::String> sendReturnTemplateIds;
        auto maximumSourceIndex = std::size_t { 0 };
        for (std::size_t index = 0; index < project.tracks.size(); ++index)
        {
            const auto& candidate = project.tracks[index];
            const auto include = source->parentTrackId.isNotEmpty()
                ? candidate.id == source->id
                : std::find(rootIds.cbegin(),
                            rootIds.cend(),
                            candidate.id) != rootIds.cend()
                    || std::find(rootIds.cbegin(),
                                 rootIds.cend(),
                                 candidate.parentTrackId) != rootIds.cend();
            if (!include)
                continue;

            sourceTracks.push_back(candidate);
            idMap.emplace_back(candidate.id, juce::Uuid().toString());
            maximumSourceIndex = std::max(maximumSourceIndex, index);
        }
        if (source->parentTrackId.isEmpty())
        {
            for (const auto& route : project.reampRoutes)
            {
                if (route.ownsReturnTrack
                    || std::find(rootIds.cbegin(),
                                 rootIds.cend(),
                                 route.sourceTrackId) == rootIds.cend()
                    || std::any_of(idMap.cbegin(),
                                   idMap.cend(),
                                   [&route](const auto& pair)
                                   {
                                       return pair.first == route.returnTrackId;
                                   }))
                    continue;

                const auto* returnTrack = project.findTrack(route.returnTrackId);
                if (returnTrack == nullptr
                    || returnTrack->parentTrackId.isNotEmpty()
                    || returnTrack->type == TrackType::master)
                    continue;
                sourceTracks.push_back(*returnTrack);
                idMap.emplace_back(returnTrack->id, juce::Uuid().toString());
                sendReturnTemplateIds.push_back(returnTrack->id);
                const auto returnIterator = std::find_if(
                    project.tracks.cbegin(),
                    project.tracks.cend(),
                    [returnTrack](const auto& candidate)
                    {
                        return candidate.id == returnTrack->id;
                    });
                maximumSourceIndex = std::max(
                    maximumSourceIndex,
                    static_cast<std::size_t>(
                        std::distance(project.tracks.cbegin(),
                                      returnIterator)));
            }
        }
        if (sourceTracks.empty())
        {
            error = "The track family to duplicate is unavailable.";
            return false;
        }

        const auto mappedId = [&idMap](const juce::String& originalId)
        {
            const auto mapping = std::find_if(
                idMap.cbegin(),
                idMap.cend(),
                [&originalId](const auto& pair)
                {
                    return pair.first == originalId;
                });
            return mapping != idMap.cend() ? mapping->second : originalId;
        };

        duplicatedTracks.reserve(sourceTracks.size());
        for (const auto& original : sourceTracks)
        {
            const auto sendReturnTemplate = std::find(
                sendReturnTemplateIds.cbegin(),
                sendReturnTemplateIds.cend(),
                original.id) != sendReturnTemplateIds.cend();
            auto duplicate = original;
            duplicate.id = mappedId(original.id);
            duplicate.armed = false;
            if (original.parentTrackId.isEmpty())
                duplicate.name = original.name
                    + (sendReturnTemplate ? " Send Return" : " Copy");
            else
                duplicate.parentTrackId = mappedId(original.parentTrackId);
            if (sendReturnTemplate)
            {
                duplicate.clips.clear();
                duplicate.activeTakeTrackId.clear();
                duplicate.compRegions.clear();
            }
            for (auto& clip : duplicate.clips)
                clip.id = juce::Uuid().toString();
            for (auto& insert : duplicate.inserts)
                insert.id = juce::Uuid().toString();

            if (original.parentTrackId.isEmpty())
            {
                duplicate.activeTakeTrackId = original.activeTakeTrackId.isNotEmpty()
                    ? mappedId(original.activeTakeTrackId)
                    : juce::String();
                for (auto& region : duplicate.compRegions)
                {
                    region.id = juce::Uuid().toString();
                    region.sourceTrackId = mappedId(region.sourceTrackId);
                }
            }
            duplicatedTracks.push_back(std::move(duplicate));
        }

        if (source->parentTrackId.isNotEmpty())
        {
            auto& duplicate = duplicatedTracks.front();
            duplicate.parentTrackId = source->parentTrackId;
            duplicate.versionNumber = std::accumulate(
                project.tracks.cbegin(),
                project.tracks.cend(),
                0,
                [source](int maximum, const auto& candidate)
                {
                    return candidate.parentTrackId == source->parentTrackId
                        ? std::max(maximum, candidate.versionNumber)
                        : maximum;
                })
                + 1;
            duplicate.name = "v" + juce::String(duplicate.versionNumber);
            const auto lastSibling = std::find_if(
                project.tracks.crbegin(),
                project.tracks.crend(),
                [source](const auto& candidate)
                {
                    return candidate.parentTrackId == source->parentTrackId;
                });
            insertionIndex = lastSibling != project.tracks.crend()
                ? static_cast<std::size_t>(
                      std::distance(project.tracks.cbegin(),
                                    lastSibling.base()))
                : maximumSourceIndex + 1;
        }
        else
        {
            insertionIndex = maximumSourceIndex + 1;
        }

        duplicatedRootTrackId = mappedId(source->id);
        for (const auto& route : project.reampRoutes)
        {
            const auto returnMapped = mappedId(route.returnTrackId)
                != route.returnTrackId;
            if (!returnMapped)
                continue;

            auto duplicate = route;
            duplicate.id = juce::Uuid().toString();
            duplicate.name += " Copy";
            duplicate.sourceTrackId = mappedId(route.sourceTrackId);
            duplicate.returnTrackId = mappedId(route.returnTrackId);
            duplicate.ownsReturnTrack = true;
            duplicatedRoutes.push_back(std::move(duplicate));
        }
        createdDuplicate = true;
    }

    if (std::any_of(duplicatedTracks.cbegin(),
                    duplicatedTracks.cend(),
                    [&project](const auto& track)
                    {
                        return project.findTrack(track.id) != nullptr;
                    }))
    {
        error = "A duplicated track already exists.";
        return false;
    }

    const auto index = std::min(insertionIndex, project.tracks.size());
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(index),
                          duplicatedTracks.begin(),
                          duplicatedTracks.end());
    project.reampRoutes.insert(project.reampRoutes.end(),
                               duplicatedRoutes.begin(),
                               duplicatedRoutes.end());
    return true;
}

void DuplicateTrackCommand::undo(Project& project)
{
    project.tracks.erase(std::remove_if(project.tracks.begin(),
                                        project.tracks.end(),
                                        [this](const auto& candidate)
    {
        return std::any_of(duplicatedTracks.cbegin(),
                           duplicatedTracks.cend(),
                           [&candidate](const auto& duplicate)
                           {
                               return candidate.id == duplicate.id;
                           });
    }), project.tracks.end());
    project.reampRoutes.erase(
        std::remove_if(project.reampRoutes.begin(),
                       project.reampRoutes.end(),
                       [this](const auto& candidate)
                       {
                           return std::any_of(
                               duplicatedRoutes.cbegin(),
                               duplicatedRoutes.cend(),
                               [&candidate](const auto& duplicate)
                               {
                                   return candidate.id == duplicate.id;
                               });
                       }),
        project.reampRoutes.end());
}

const juce::String& DuplicateTrackCommand::duplicatedTrackId() const noexcept
{
    return duplicatedRootTrackId;
}
}
