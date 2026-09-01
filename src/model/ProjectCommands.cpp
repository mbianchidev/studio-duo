#include "ProjectCommands.h"

#include <algorithm>
#include <numeric>

namespace studio
{
namespace
{
bool hasMainAudioOutput(TrackType type)
{
    return type == TrackType::audio
        || type == TrackType::instrument
        || type == TrackType::aux
        || type == TrackType::bus;
}
}

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
        if (track.outputTrackId.isNotEmpty())
        {
            error = "Version lanes follow their parent track output.";
            return false;
        }
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
    if (hasMainAudioOutput(track.type))
    {
        if (!project.validateTrackOutput(track.id, track.outputTrackId, error))
        {
            project.tracks.erase(project.tracks.begin()
                                 + static_cast<std::ptrdiff_t>(insertionIndex));
            return false;
        }
        if (!outputRoute.has_value())
        {
            RoutingConnection route;
            route.name = "Main output";
            route.kind = RouteKind::mainOutput;
            route.sourceTrackId = track.id;
            route.destination.type = RouteEndpointType::track;
            route.destination.trackId = project.resolvedOutputTrackId(track);
            outputRoute = std::move(route);
        }
        project.routingConnections.push_back(*outputRoute);
    }
    if (!project.validateRoutingGraph(error))
    {
        project.routingConnections.erase(
            std::remove_if(
                project.routingConnections.begin(),
                project.routingConnections.end(),
                [this](const auto& connection)
                {
                    return outputRoute.has_value()
                        && connection.id == outputRoute->id;
                }),
            project.routingConnections.end());
        project.tracks.erase(project.tracks.begin()
                             + static_cast<std::ptrdiff_t>(insertionIndex));
        return false;
    }
    return true;
}

void AddTrackCommand::undo(Project& project)
{
    project.tracks.erase(std::remove_if(project.tracks.begin(), project.tracks.end(), [this](const auto& candidate)
    {
        return candidate.id == track.id;
    }), project.tracks.end());
    if (outputRoute.has_value())
    {
        project.routingConnections.erase(
            std::remove_if(
                project.routingConnections.begin(),
                project.routingConnections.end(),
                [this](const auto& connection)
                {
                    return connection.id == outputRoute->id;
                }),
            project.routingConnections.end());
    }
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

TrackRoutingState TrackRoutingState::fromTrack(const Track& track)
{
    return {
        track.folderTrackId,
        track.controlledTrackIds,
        track.channelLayout,
        track.polarityInverted,
        track.soloSafe,
        track.hardwareOutputChannel,
        track.controlRoomDimDecibels,
        track.controlRoomDimmed,
        track.controlRoomMono
    };
}

SetTrackRoutingStateCommand::SetTrackRoutingStateCommand(
    juce::String trackToChange,
    TrackRoutingState before,
    TrackRoutingState after)
    : trackId(std::move(trackToChange)),
      oldState(std::move(before)),
      newState(std::move(after))
{
}

juce::String SetTrackRoutingStateCommand::name() const
{
    return "Change track routing settings";
}

bool SetTrackRoutingStateCommand::perform(Project& project,
                                          juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The track to change no longer exists.";
        return false;
    }

    apply(*track, newState);
    if (!project.validateRoutingGraph(error))
    {
        apply(*track, oldState);
        return false;
    }
    return true;
}

void SetTrackRoutingStateCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        apply(*track, oldState);
}

void SetTrackRoutingStateCommand::apply(
    Track& track,
    const TrackRoutingState& state)
{
    track.folderTrackId = state.folderTrackId;
    track.controlledTrackIds = state.controlledTrackIds;
    track.channelLayout = state.channelLayout;
    track.polarityInverted = state.polarityInverted;
    track.soloSafe = state.soloSafe;
    track.hardwareOutputChannel = state.hardwareOutputChannel;
    track.controlRoomDimDecibels = state.controlRoomDimDecibels;
    track.controlRoomDimmed = state.controlRoomDimmed;
    track.controlRoomMono = state.controlRoomMono;
}

SetAutomationLaneCommand::SetAutomationLaneCommand(
    AutomationLane before,
    AutomationLane after)
    : oldLane(std::move(before)),
      newLane(std::move(after))
{
}

juce::String SetAutomationLaneCommand::name() const
{
    return "Change automation lane";
}

bool SetAutomationLaneCommand::perform(Project& project,
                                       juce::String& error)
{
    if (oldLane.id != newLane.id)
    {
        error = "Automation lane updates cannot change IDs.";
        return false;
    }
    juce::String validationError;
    if (!AutomationLane::fromVar(
            newLane.toVar(),
            validationError)
             .has_value())
    {
        error = validationError;
        return false;
    }
    const auto lane = std::find_if(
        project.automationLanes.begin(),
        project.automationLanes.end(),
        [this](const auto& candidate)
        {
            return candidate.id == newLane.id;
        });
    if (lane == project.automationLanes.end())
    {
        error = "The automation lane no longer exists.";
        return false;
    }
    *lane = newLane;
    return true;
}

void SetAutomationLaneCommand::undo(Project& project)
{
    const auto lane = std::find_if(
        project.automationLanes.begin(),
        project.automationLanes.end(),
        [this](const auto& candidate)
        {
            return candidate.id == oldLane.id;
        });
    if (lane != project.automationLanes.end())
        *lane = oldLane;
}

AddAutomationLaneCommand::AddAutomationLaneCommand(
    AutomationLane laneToAdd)
    : lane(std::move(laneToAdd))
{
}

juce::String AddAutomationLaneCommand::name() const
{
    return "Add automation lane";
}

bool AddAutomationLaneCommand::perform(Project& project,
                                       juce::String& error)
{
    if (std::any_of(
            project.automationLanes.cbegin(),
            project.automationLanes.cend(),
            [this](const auto& candidate)
            {
                return candidate.id == lane.id;
            }))
    {
        error = "An automation lane with the same ID already exists.";
        return false;
    }
    juce::String validationError;
    if (!AutomationLane::fromVar(lane.toVar(), validationError).has_value()
        || project.findTrack(lane.target.trackId) == nullptr)
    {
        error = validationError.isNotEmpty()
            ? validationError
            : "The automation target track is unavailable.";
        return false;
    }
    insertionIndex = std::min(
        insertionIndex,
        project.automationLanes.size());
    if (insertionIndex == 0 && !project.automationLanes.empty())
        insertionIndex = project.automationLanes.size();
    project.automationLanes.insert(
        project.automationLanes.begin()
            + static_cast<std::ptrdiff_t>(insertionIndex),
        lane);
    return true;
}

void AddAutomationLaneCommand::undo(Project& project)
{
    project.automationLanes.erase(
        std::remove_if(
            project.automationLanes.begin(),
            project.automationLanes.end(),
            [this](const auto& candidate)
            {
                return candidate.id == lane.id;
            }),
        project.automationLanes.end());
}

RemoveAutomationLaneCommand::RemoveAutomationLaneCommand(
    juce::String laneToRemove)
    : laneId(std::move(laneToRemove))
{
}

juce::String RemoveAutomationLaneCommand::name() const
{
    return "Remove automation lane";
}

bool RemoveAutomationLaneCommand::perform(Project& project,
                                          juce::String& error)
{
    const auto lane = std::find_if(
        project.automationLanes.begin(),
        project.automationLanes.end(),
        [this](const auto& candidate)
        {
            return candidate.id == laneId;
        });
    if (lane == project.automationLanes.end())
    {
        error = "The automation lane no longer exists.";
        return false;
    }
    if (!capturedOriginal)
    {
        removalIndex = static_cast<std::size_t>(
            std::distance(project.automationLanes.begin(), lane));
        removedLane = *lane;
        capturedOriginal = true;
    }
    project.automationLanes.erase(lane);
    return true;
}

void RemoveAutomationLaneCommand::undo(Project& project)
{
    const auto index = std::min(
        removalIndex,
        project.automationLanes.size());
    project.automationLanes.insert(
        project.automationLanes.begin()
            + static_cast<std::ptrdiff_t>(index),
        removedLane);
}

SetTrackAutomationModeCommand::SetTrackAutomationModeCommand(
    juce::String trackToChange,
    AutomationMode mode,
    bool armed)
    : trackId(std::move(trackToChange)),
      newMode(mode),
      newArmed(armed)
{
}

juce::String SetTrackAutomationModeCommand::name() const
{
    return "Change automation mode";
}

bool SetTrackAutomationModeCommand::perform(Project& project,
                                            juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The automation track no longer exists.";
        return false;
    }
    if (!capturedOriginal)
    {
        oldMode = track->automationMode;
        oldArmed = track->automationArmed;
        capturedOriginal = true;
    }
    track->automationMode = newMode;
    track->automationArmed = newArmed;
    return true;
}

void SetTrackAutomationModeCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
    {
        track->automationMode = oldMode;
        track->automationArmed = oldArmed;
    }
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

SetTrackOutputCommand::SetTrackOutputCommand(juce::String sourceTrackId,
                                             juce::String destinationTrackId)
    : trackId(std::move(sourceTrackId)),
      newOutputTrackId(std::move(destinationTrackId))
{
}

juce::String SetTrackOutputCommand::name() const
{
    return "Change track output";
}

bool SetTrackOutputCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The track to route no longer exists.";
        return false;
    }
    if (!project.validateTrackOutput(trackId, newOutputTrackId, error))
        return false;

    if (!capturedOriginal)
    {
        oldOutputTrackId = track->outputTrackId;
        const auto route = std::find_if(
            project.routingConnections.cbegin(),
            project.routingConnections.cend(),
            [this](const auto& connection)
            {
                return connection.kind == RouteKind::mainOutput
                    && connection.sourceTrackId == trackId;
            });
        if (route != project.routingConnections.cend())
            oldRoute = *route;
        newRoute = oldRoute.value_or(RoutingConnection {});
        newRoute.name = "Main output";
        newRoute.kind = RouteKind::mainOutput;
        newRoute.tap = RouteTap::postFader;
        newRoute.sourceTrackId = trackId;
        newRoute.destination.type = RouteEndpointType::track;
        capturedOriginal = true;
    }

    newRoute.destination.trackId = newOutputTrackId.isNotEmpty()
        ? newOutputTrackId
        : project.masterTrackId();
    auto* route = project.findRoutingConnection(newRoute.id);
    if (route != nullptr)
        *route = newRoute;
    else
        project.routingConnections.push_back(newRoute);
    track->outputTrackId = newOutputTrackId;
    if (!project.validateRoutingGraph(error))
    {
        track->outputTrackId = oldOutputTrackId;
        project.routingConnections.erase(
            std::remove_if(
                project.routingConnections.begin(),
                project.routingConnections.end(),
                [this](const auto& connection)
                {
                    return connection.id == newRoute.id;
                }),
            project.routingConnections.end());
        if (oldRoute.has_value())
            project.routingConnections.push_back(*oldRoute);
        return false;
    }
    return true;
}

void SetTrackOutputCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        track->outputTrackId = oldOutputTrackId;
    project.routingConnections.erase(
        std::remove_if(
            project.routingConnections.begin(),
            project.routingConnections.end(),
            [this](const auto& connection)
            {
                return connection.id == newRoute.id;
            }),
        project.routingConnections.end());
    if (oldRoute.has_value())
        project.routingConnections.push_back(*oldRoute);
}

AddRoutingConnectionCommand::AddRoutingConnectionCommand(
    RoutingConnection connectionToAdd)
    : connection(std::move(connectionToAdd))
{
}

juce::String AddRoutingConnectionCommand::name() const
{
    return "Add routing connection";
}

bool AddRoutingConnectionCommand::perform(Project& project, juce::String& error)
{
    if (project.findRoutingConnection(connection.id) != nullptr)
    {
        error = "A routing connection with the same ID already exists.";
        return false;
    }

    if (!capturedIndex)
    {
        insertionIndex = project.routingConnections.size();
        capturedIndex = true;
    }
    const auto index = std::min(insertionIndex,
                                project.routingConnections.size());
    project.routingConnections.insert(
        project.routingConnections.begin() + static_cast<std::ptrdiff_t>(index),
        connection);
    if (!project.validateRoutingGraph(error))
    {
        project.routingConnections.erase(
            project.routingConnections.begin() + static_cast<std::ptrdiff_t>(index));
        return false;
    }
    return true;
}

void AddRoutingConnectionCommand::undo(Project& project)
{
    project.routingConnections.erase(
        std::remove_if(
            project.routingConnections.begin(),
            project.routingConnections.end(),
            [this](const auto& candidate)
            {
                return candidate.id == connection.id;
            }),
        project.routingConnections.end());
}

UpdateRoutingConnectionCommand::UpdateRoutingConnectionCommand(
    RoutingConnection before,
    RoutingConnection after)
    : oldConnection(std::move(before)),
      newConnection(std::move(after))
{
}

juce::String UpdateRoutingConnectionCommand::name() const
{
    return "Change routing connection";
}

bool UpdateRoutingConnectionCommand::perform(Project& project,
                                             juce::String& error)
{
    if (oldConnection.id != newConnection.id)
    {
        error = "A routing connection update cannot change its ID.";
        return false;
    }

    auto* connection = project.findRoutingConnection(newConnection.id);
    if (connection == nullptr)
    {
        error = "The routing connection no longer exists.";
        return false;
    }

    const auto current = *connection;
    *connection = newConnection;
    if (!project.validateRoutingGraph(error))
    {
        *connection = current;
        return false;
    }
    return true;
}

void UpdateRoutingConnectionCommand::undo(Project& project)
{
    if (auto* connection = project.findRoutingConnection(oldConnection.id))
        *connection = oldConnection;
}

RemoveRoutingConnectionCommand::RemoveRoutingConnectionCommand(
    juce::String connectionToRemove)
    : connectionId(std::move(connectionToRemove))
{
}

juce::String RemoveRoutingConnectionCommand::name() const
{
    return "Remove routing connection";
}

bool RemoveRoutingConnectionCommand::perform(Project& project,
                                             juce::String& error)
{
    const auto iterator = std::find_if(
        project.routingConnections.begin(),
        project.routingConnections.end(),
        [this](const auto& connection)
        {
            return connection.id == connectionId;
        });
    if (iterator == project.routingConnections.end())
    {
        error = "The routing connection no longer exists.";
        return false;
    }

    if (!capturedOriginal)
    {
        removalIndex = static_cast<std::size_t>(
            std::distance(project.routingConnections.begin(), iterator));
        removedConnection = *iterator;
        for (std::size_t index = 0;
             index < project.automationLanes.size();
             ++index)
        {
            if (project.automationLanes[index].target.routeId == connectionId)
                removedAutomationLanes.emplace_back(
                    index,
                    project.automationLanes[index]);
        }
        capturedOriginal = true;
    }
    project.routingConnections.erase(iterator);
    project.automationLanes.erase(
        std::remove_if(
            project.automationLanes.begin(),
            project.automationLanes.end(),
            [this](const auto& lane)
            {
                return lane.target.routeId == connectionId;
            }),
        project.automationLanes.end());
    return true;
}

void RemoveRoutingConnectionCommand::undo(Project& project)
{
    const auto index = std::min(removalIndex,
                                project.routingConnections.size());
    project.routingConnections.insert(
        project.routingConnections.begin() + static_cast<std::ptrdiff_t>(index),
        removedConnection);
    for (const auto& [originalIndex, lane] : removedAutomationLanes)
    {
        const auto laneIndex = std::min(
            originalIndex,
            project.automationLanes.size());
        project.automationLanes.insert(
            project.automationLanes.begin()
                + static_cast<std::ptrdiff_t>(laneIndex),
            lane);
    }
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
        for (std::size_t index = 0;
             index < project.automationLanes.size();
             ++index)
        {
            if (project.automationLanes[index].target.insertId == insertId)
                removedAutomationLanes.emplace_back(
                    index,
                    project.automationLanes[index]);
        }
        capturedOriginal = true;
    }
    track->inserts.erase(iterator);
    project.automationLanes.erase(
        std::remove_if(
            project.automationLanes.begin(),
            project.automationLanes.end(),
            [this](const auto& lane)
            {
                return lane.target.insertId == insertId;
            }),
        project.automationLanes.end());
    return true;
}

void RemovePluginInsertCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
    {
        const auto index = std::min(removalIndex, track->inserts.size());
        track->inserts.insert(track->inserts.begin() + static_cast<std::ptrdiff_t>(index), removedInsert);
    }
    for (const auto& [originalIndex, lane] : removedAutomationLanes)
    {
        const auto index = std::min(
            originalIndex,
            project.automationLanes.size());
        project.automationLanes.insert(
            project.automationLanes.begin()
                + static_cast<std::ptrdiff_t>(index),
            lane);
    }
}

ReplacePluginInsertCommand::ReplacePluginInsertCommand(
    juce::String sourceTrackId,
    juce::String insertToReplace,
    PluginInsert replacement)
    : trackId(std::move(sourceTrackId)),
      insertId(std::move(insertToReplace)),
      newInsert(std::move(replacement))
{
}

juce::String ReplacePluginInsertCommand::name() const
{
    return "Replace plugin insert";
}

bool ReplacePluginInsertCommand::perform(Project& project,
                                         juce::String& error)
{
    auto* insert = find(project);
    if (insert == nullptr)
    {
        error = "The plugin insert to replace no longer exists.";
        return false;
    }
    if (newInsert.pluginIdentifier.isEmpty()
        || newInsert.name.trim().isEmpty())
    {
        error = "The replacement plugin is invalid.";
        return false;
    }

    if (!capturedOriginal)
    {
        oldInsert = *insert;
        newInsert.id = oldInsert.id;
        newInsert.stateFile = oldInsert.stateFile;
        newInsert.stateHash = oldInsert.stateHash;
        newInsert.bypassed = oldInsert.bypassed;
        newInsert.bridgeMode = oldInsert.bridgeMode;
        newInsert.recoveryDisabled = false;
        newInsert.missing = false;
        capturedOriginal = true;
    }
    *insert = newInsert;
    return true;
}

void ReplacePluginInsertCommand::undo(Project& project)
{
    if (auto* insert = find(project))
        *insert = oldInsert;
}

PluginInsert* ReplacePluginInsertCommand::find(Project& project) const
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return nullptr;
    const auto iterator = std::find_if(
        track->inserts.begin(),
        track->inserts.end(),
        [this](const auto& candidate)
        {
            return candidate.id == insertId;
        });
    return iterator == track->inserts.end() ? nullptr : &*iterator;
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

SetPluginBridgeModeCommand::SetPluginBridgeModeCommand(
    juce::String sourceTrackId,
    juce::String insertToChange,
    PluginBridgeMode mode)
    : trackId(std::move(sourceTrackId)),
      insertId(std::move(insertToChange)),
      newMode(mode)
{
}

juce::String SetPluginBridgeModeCommand::name() const
{
    return "Change plugin isolation mode";
}

bool SetPluginBridgeModeCommand::perform(Project& project,
                                         juce::String& error)
{
    auto* insert = find(project);
    if (insert == nullptr)
    {
        error = "The plugin insert no longer exists.";
        return false;
    }
    if (newMode == PluginBridgeMode::araCompatibility
        && !insert->araCapable)
    {
        error = "This plugin does not advertise an ARA extension.";
        return false;
    }

    if (!capturedOriginal)
    {
        oldMode = insert->bridgeMode;
        oldRecoveryDisabled = insert->recoveryDisabled;
        capturedOriginal = true;
    }
    insert->bridgeMode = newMode;
    insert->recoveryDisabled = false;
    return true;
}

void SetPluginBridgeModeCommand::undo(Project& project)
{
    if (auto* insert = find(project))
    {
        insert->bridgeMode = oldMode;
        insert->recoveryDisabled = oldRecoveryDisabled;
    }
}

PluginInsert* SetPluginBridgeModeCommand::find(Project& project) const
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return nullptr;
    const auto iterator = std::find_if(
        track->inserts.begin(),
        track->inserts.end(),
        [this](const auto& candidate)
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
        oldRoutingConnections = project.routingConnections;
        oldAutomationLanes = project.automationLanes;
        for (const auto& candidate : project.tracks)
        {
            oldTrackOutputs.emplace_back(candidate.id, candidate.outputTrackId);
            oldTrackRoutingStates.emplace_back(
                candidate.id,
                TrackRoutingState::fromTrack(candidate));
        }

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
    for (auto& candidate : project.tracks)
    {
        if (std::any_of(removedTracks.cbegin(),
                        removedTracks.cend(),
                        [&candidate](const auto& removed)
                        {
                            return removed.second.id == candidate.outputTrackId;
                        }))
            candidate.outputTrackId.clear();
        if (std::any_of(
                removedTracks.cbegin(),
                removedTracks.cend(),
                [&candidate](const auto& removed)
                {
                    return removed.second.id == candidate.folderTrackId;
                }))
            candidate.folderTrackId.clear();
        candidate.controlledTrackIds.erase(
            std::remove_if(
                candidate.controlledTrackIds.begin(),
                candidate.controlledTrackIds.end(),
                [this](const auto& controlledId)
                {
                    return std::any_of(
                        removedTracks.cbegin(),
                        removedTracks.cend(),
                        [&controlledId](const auto& removed)
                        {
                            return removed.second.id == controlledId;
                        });
                }),
            candidate.controlledTrackIds.end());
    }
    const auto removedTrack = [this](const juce::String& candidateId)
    {
        return std::any_of(
            removedTracks.cbegin(),
            removedTracks.cend(),
            [&candidateId](const auto& removed)
            {
                return removed.second.id == candidateId;
            });
    };
    const auto masterId = project.masterTrackId();
    for (auto iterator = project.routingConnections.begin();
         iterator != project.routingConnections.end();)
    {
        if (removedTrack(iterator->sourceTrackId))
        {
            iterator = project.routingConnections.erase(iterator);
            continue;
        }

        const auto destinationRemoved =
            (iterator->destination.type == RouteEndpointType::track
             || iterator->destination.type
                    == RouteEndpointType::pluginSidechain)
            && removedTrack(iterator->destination.trackId);
        if (!destinationRemoved)
        {
            ++iterator;
            continue;
        }

        if (iterator->kind == RouteKind::mainOutput)
        {
            iterator->destination.type = RouteEndpointType::track;
            iterator->destination.trackId = masterId;
            iterator->destination.insertId.clear();
            ++iterator;
        }
        else
        {
            iterator = project.routingConnections.erase(iterator);
        }
    }
    if (!project.validateRoutingGraph(error))
        return false;
    project.automationLanes.erase(
        std::remove_if(
            project.automationLanes.begin(),
            project.automationLanes.end(),
            [&project, &removedTrack](const auto& lane)
            {
                return removedTrack(lane.target.trackId)
                    || (lane.target.routeId.isNotEmpty()
                        && project.findRoutingConnection(
                               lane.target.routeId) == nullptr);
            }),
        project.automationLanes.end());
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
    project.routingConnections = oldRoutingConnections;
    project.automationLanes = oldAutomationLanes;
    for (const auto& [restoredTrackId, outputTrackId] : oldTrackOutputs)
        if (auto* track = project.findTrack(restoredTrackId))
            track->outputTrackId = outputTrackId;
    for (const auto& [restoredTrackId, state] : oldTrackRoutingStates)
        if (auto* track = project.findTrack(restoredTrackId))
        {
            track->folderTrackId = state.folderTrackId;
            track->controlledTrackIds = state.controlledTrackIds;
            track->channelLayout = state.channelLayout;
            track->polarityInverted = state.polarityInverted;
            track->soloSafe = state.soloSafe;
            track->hardwareOutputChannel = state.hardwareOutputChannel;
            track->controlRoomDimDecibels = state.controlRoomDimDecibels;
            track->controlRoomDimmed = state.controlRoomDimmed;
            track->controlRoomMono = state.controlRoomMono;
        }
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
        std::vector<std::pair<juce::String, juce::String>> insertIdMap;
        std::vector<std::pair<juce::String, juce::String>> routeIdMap;
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
            duplicate.outputTrackId = mappedId(original.outputTrackId);
            duplicate.folderTrackId = mappedId(original.folderTrackId);
            for (auto& controlledId : duplicate.controlledTrackIds)
                controlledId = mappedId(controlledId);
            if (sendReturnTemplate)
            {
                duplicate.clips.clear();
                duplicate.activeTakeTrackId.clear();
                duplicate.compRegions.clear();
            }
            for (auto& clip : duplicate.clips)
                clip.id = juce::Uuid().toString();
            for (auto& insert : duplicate.inserts)
            {
                const auto originalInsertId = insert.id;
                insert.id = juce::Uuid().toString();
                insertIdMap.emplace_back(originalInsertId, insert.id);
            }

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
        const auto mappedInsertId = [&insertIdMap](const juce::String& originalId)
        {
            const auto mapping = std::find_if(
                insertIdMap.cbegin(),
                insertIdMap.cend(),
                [&originalId](const auto& pair)
                {
                    return pair.first == originalId;
                });
            return mapping != insertIdMap.cend() ? mapping->second : originalId;
        };
        for (const auto& connection : project.routingConnections)
        {
            if (mappedId(connection.sourceTrackId) == connection.sourceTrackId)
                continue;

            auto duplicate = connection;
            duplicate.id = juce::Uuid().toString();
            duplicate.name += " Copy";
            duplicate.sourceTrackId = mappedId(connection.sourceTrackId);
            duplicate.destination.trackId = mappedId(
                connection.destination.trackId);
            duplicate.destination.insertId = mappedInsertId(
                connection.destination.insertId);
            routeIdMap.emplace_back(connection.id, duplicate.id);
            duplicatedConnections.push_back(std::move(duplicate));
        }
        const auto mappedRouteId = [&routeIdMap](
                                       const juce::String& originalId)
        {
            const auto mapping = std::find_if(
                routeIdMap.cbegin(),
                routeIdMap.cend(),
                [&originalId](const auto& pair)
                {
                    return pair.first == originalId;
                });
            return mapping != routeIdMap.cend()
                ? mapping->second
                : originalId;
        };
        for (const auto& lane : project.automationLanes)
        {
            if (mappedId(lane.target.trackId) == lane.target.trackId)
                continue;
            auto duplicate = lane;
            duplicate.id = juce::Uuid().toString();
            duplicate.name += " Copy";
            duplicate.target.trackId = mappedId(lane.target.trackId);
            duplicate.target.routeId = mappedRouteId(lane.target.routeId);
            duplicate.target.insertId = mappedInsertId(
                lane.target.insertId);
            for (auto& point : duplicate.points)
                point.id = juce::Uuid().toString();
            duplicatedAutomationLanes.push_back(std::move(duplicate));
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
    project.routingConnections.insert(project.routingConnections.end(),
                                      duplicatedConnections.begin(),
                                      duplicatedConnections.end());
    project.automationLanes.insert(
        project.automationLanes.end(),
        duplicatedAutomationLanes.begin(),
        duplicatedAutomationLanes.end());
    if (!project.validateRoutingGraph(error))
    {
        undo(project);
        return false;
    }
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
    project.routingConnections.erase(
        std::remove_if(
            project.routingConnections.begin(),
            project.routingConnections.end(),
            [this](const auto& candidate)
            {
                return std::any_of(
                    duplicatedConnections.cbegin(),
                    duplicatedConnections.cend(),
                    [&candidate](const auto& duplicate)
                    {
                        return candidate.id == duplicate.id;
                    });
            }),
        project.routingConnections.end());
    project.automationLanes.erase(
        std::remove_if(
            project.automationLanes.begin(),
            project.automationLanes.end(),
            [this](const auto& candidate)
            {
                return std::any_of(
                    duplicatedAutomationLanes.cbegin(),
                    duplicatedAutomationLanes.cend(),
                    [&candidate](const auto& duplicate)
                    {
                        return candidate.id == duplicate.id;
                    });
            }),
        project.automationLanes.end());
}

const juce::String& DuplicateTrackCommand::duplicatedTrackId() const noexcept
{
    return duplicatedRootTrackId;
}
}
