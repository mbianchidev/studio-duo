#include "ArrangementEditing.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace studio
{
TrackLifecycleState TrackLifecycleState::fromTrack(const Track& track)
{
    return { track.active, track.hidden, track.folderCollapsed,
             track.frozenTrackId, track.freezeSourceTrackId };
}

SetTrackLifecycleCommand::SetTrackLifecycleCommand(
    juce::String id, TrackLifecycleState oldState, TrackLifecycleState newState,
    juce::String label)
    : trackId(std::move(id)), commandName(std::move(label)),
      before(std::move(oldState)), after(std::move(newState))
{
}

juce::String SetTrackLifecycleCommand::name() const { return commandName; }

bool SetTrackLifecycleCommand::perform(Project& project, juce::String& error)
{
    auto* track = project.findTrack(trackId);
    if (track == nullptr)
    {
        error = "The track is no longer available.";
        return false;
    }
    if (track->type == TrackType::master
        && (!after.active || after.hidden || after.frozenTrackId.isNotEmpty() || after.freezeSourceTrackId.isNotEmpty()))
    {
        error = "The master track cannot be deactivated, hidden or frozen.";
        return false;
    }
    if (before.active != after.active && project.isTrackFrozen(trackId))
    {
        error = "Unfreeze the track before changing its activation.";
        return false;
    }
    if (after.frozenTrackId == trackId || after.freezeSourceTrackId == trackId)
    {
        error = "A frozen track cannot reference itself.";
        return false;
    }
    apply(*track, after);
    return true;
}

void SetTrackLifecycleCommand::undo(Project& project)
{
    if (auto* track = project.findTrack(trackId))
        apply(*track, before);
}

void SetTrackLifecycleCommand::apply(Track& track, const TrackLifecycleState& state)
{
    track.active = state.active;
    track.hidden = state.hidden;
    track.folderCollapsed = state.folderCollapsed;
    track.frozenTrackId = state.frozenTrackId;
    track.freezeSourceTrackId = state.freezeSourceTrackId;
}

std::unique_ptr<ProjectCommand> ArrangementEditing::groupTracks(
    const Project& project, const std::vector<juce::String>& ids,
    bool busFolder, juce::String& error)
{
    error.clear();
    if (ids.empty() || !project.canAddTracks(1, error))
    {
        if (error.isEmpty())
            error = "Select at least one track to group.";
        return {};
    }
    std::vector<const Track*> members;
    for (const auto& track : project.tracks)
        if (std::find(ids.cbegin(), ids.cend(), track.id) != ids.cend())
            members.push_back(&track);
    if (members.size() != ids.size())
    {
        error = "The group selection contains duplicate or unavailable tracks.";
        return {};
    }
    const auto parentId = members.front()->folderTrackId;
    const auto midiGroup = busFolder && std::all_of(members.begin(), members.end(),
        [](const auto* track) { return track->type == TrackType::midi; });
    if (busFolder && !midiGroup && std::any_of(members.begin(), members.end(),
        [](const auto* track) { return track->type == TrackType::midi; }))
    {
        error = "Create an instrument bus folder for MIDI-only tracks, an audio bus folder for audio/instrument tracks, "
                "or a regular folder for a mixed MIDI/audio selection.";
        return {};
    }
    juce::String outputId;
    for (const auto* track : members)
    {
        if (track->type == TrackType::master || track->type == TrackType::controlRoom
            || track->parentTrackId.isNotEmpty() || track->folderTrackId != parentId
            || project.isTrackFrozen(track->id)
            || (busFolder && (track->type == TrackType::folder || track->type == TrackType::vca)))
        {
            error = "Group editable root tracks in the same folder; unfreeze frozen tracks first.";
            return {};
        }
        if (busFolder && track->type != TrackType::midi)
        {
            const auto destination = project.resolvedOutputTrackId(*track);
            if (outputId.isNotEmpty() && outputId != destination)
            {
                error = "Bus-folder members must share an output to preserve their routing.";
                return {};
            }
            outputId = destination;
        }
    }
    Track folder;
    folder.type = midiGroup ? TrackType::instrument : busFolder ? TrackType::bus : TrackType::folder;
    folder.name = midiGroup ? "Instrument bus folder" : busFolder ? "Bus folder" : "Folder";
    folder.folderTrackId = parentId;
    folder.outputTrackId = busFolder ? outputId : juce::String();
    folder.colour = members.front()->colour;
    const auto folderId = folder.id;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    commands.push_back(std::make_unique<AddTrackCommand>(folder, members.front()->id));
    for (const auto* track : members)
    {
        const auto before = TrackRoutingState::fromTrack(*track);
        auto after = before;
        after.folderTrackId = folderId;
        commands.push_back(std::make_unique<SetTrackRoutingStateCommand>(track->id, before, after));
        if (busFolder && track->type != TrackType::midi)
            commands.push_back(std::make_unique<SetTrackOutputCommand>(track->id, folderId));
        if (midiGroup)
        {
            RoutingConnection route;
            route.name = "Folder MIDI input";
            route.sourceTrackId = track->id;
            route.destination.trackId = folderId;
            route.signalType = SignalType::midi;
            route.kind = RouteKind::send;
            commands.push_back(std::make_unique<AddRoutingConnectionCommand>(std::move(route)));
        }
    }
    return std::make_unique<BatchProjectCommand>(
        busFolder ? "Create bus folder" : "Create folder", std::move(commands));
}

std::optional<AudioClip> ArrangementEditing::slipClip(
    const AudioClip& clip, double delta, juce::String& error)
{
    if (!std::isfinite(delta))
    {
        error = "The slip offset must be finite.";
        return std::nullopt;
    }
    const auto span = std::abs(clip.sourceSecondsAt(clip.durationSeconds) - clip.sourceSecondsAt(0.0));
    const auto minimum = clip.sourceRangeStartSeconds - clip.sourceOffsetSeconds;
    const auto maximum = clip.sourceRangeEnd() - clip.sourceOffsetSeconds - span;
    if (maximum < minimum)
    {
        error = "The clip has no valid recoverable source range.";
        return std::nullopt;
    }
    delta = juce::jlimit(minimum, maximum, delta);
    auto result = clip;
    result.sourceOffsetSeconds += delta;
    for (auto& marker : result.warpMarkers)
        marker.sourceSeconds += delta;
    return result;
}

std::optional<AudioClip> ArrangementEditing::stretchClip(
    const AudioClip& clip, double duration, juce::String& error)
{
    const auto rate = clip.playbackRate * clip.durationSeconds / duration;
    if (!std::isfinite(duration) || duration <= 0.0 || !std::isfinite(rate)
        || rate < 0.25 || rate > 4.0)
    {
        error = "Stretching must retain a positive duration and a playback rate between 0.25x and 4x.";
        return std::nullopt;
    }
    auto result = clip;
    const auto scale = duration / clip.durationSeconds;
    result.durationSeconds = duration;
    result.playbackRate = rate;
    result.fadeInSeconds *= scale;
    result.fadeOutSeconds *= scale;
    for (auto& marker : result.warpMarkers)
        marker.timelineOffsetSeconds *= scale;
    return result;
}

std::vector<const Track*> ArrangementEditing::visibleTracks(const Project& project)
{
    std::map<juce::String, std::vector<const Track*>> children;
    for (const auto& track : project.tracks)
        children[track.parentTrackId.isNotEmpty() ? track.parentTrackId : track.folderTrackId].push_back(&track);
    std::vector<const Track*> pending, result;
    const auto appendChildren = [&](const juce::String& id, const Track* parent)
    {
        const auto found = children.find(id);
        if (found == children.end())
            return;
        for (auto item = found->second.rbegin(); item != found->second.rend(); ++item)
        {
            const auto* child = *item;
            if (parent != nullptr && (child->parentTrackId.isNotEmpty()
                    ? parent->versionsCollapsed : parent->folderCollapsed))
                continue;
            pending.push_back(child);
        }
    };
    appendChildren({}, nullptr);
    std::set<juce::String> visited;
    result.reserve(project.tracks.size());
    while (!pending.empty())
    {
        const auto* track = pending.back();
        pending.pop_back();
        if (track->hidden || !visited.insert(track->id).second)
            continue;
        result.push_back(track);
        appendChildren(track->id, track);
    }
    return result;
}

std::vector<juce::String> ArrangementEditing::trackSubtreeIds(
    const Project& project, const std::vector<juce::String>& roots)
{
    const std::set<juce::String> rootIds(roots.begin(), roots.end());
    std::vector<juce::String> result;
    for (const auto& track : project.tracks)
    {
        const auto* parent = &track;
        for (std::size_t depth = 0; parent != nullptr && depth <= project.tracks.size(); ++depth)
        {
            if (rootIds.contains(parent->id))
            {
                result.push_back(track.id);
                break;
            }
            parent = project.findTrack(parent->parentTrackId.isNotEmpty()
                ? parent->parentTrackId : parent->folderTrackId);
        }
    }
    return result;
}
}
