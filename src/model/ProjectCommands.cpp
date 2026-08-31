#include "ProjectCommands.h"

#include <algorithm>

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

MoveClipCommand::MoveClipCommand(juce::String clipToMove, double destinationSeconds)
    : clipId(std::move(clipToMove)),
      newStartSeconds(std::max(0.0, destinationSeconds))
{
}

juce::String MoveClipCommand::name() const
{
    return "Move clip";
}

bool MoveClipCommand::perform(Project& project, juce::String& error)
{
    auto* clip = project.findClip(clipId);
    if (clip == nullptr)
    {
        error = "The clip to move no longer exists.";
        return false;
    }

    if (!capturedOriginal)
    {
        oldStartSeconds = clip->startSeconds;
        capturedOriginal = true;
    }

    clip->startSeconds = newStartSeconds;
    return true;
}

void MoveClipCommand::undo(Project& project)
{
    if (auto* clip = project.findClip(clipId))
        clip->startSeconds = oldStartSeconds;
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
    auto leftSide = original;
    leftSide.durationSeconds = leftDuration;
    rightSide.startSeconds = splitSeconds;
    rightSide.sourceOffsetSeconds = original.sourceOffsetSeconds + leftDuration;
    rightSide.durationSeconds = original.durationSeconds - leftDuration;

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

TrackMixState TrackMixState::fromTrack(const Track& track)
{
    return { track.volumeDecibels, track.pan, track.muted, track.solo, track.armed };
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
}
