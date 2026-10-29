#include "ControlSurfaceProject.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace studio
{
namespace
{
float normalisedVolume(float decibels)
{
    return juce::jlimit(0.0f, 1.0f, (decibels + 60.0f) / 72.0f);
}
}

ControlSurfaceState controlSurfaceStateForProject(
    const Project& project, const juce::String& selected, bool playing, bool recording)
{
    ControlSurfaceState result;
    result.projectId = project.id;
    result.selectedTrackId = selected;
    result.playing = playing;
    result.recording = recording;
    result.loopEnabled = project.loopEnabled;
    result.metronomeEnabled = project.metronomeEnabled;
    for (const auto& track : project.tracks)
    {
        if (track.parentTrackId.isNotEmpty() || track.type == TrackType::folder
            || !project.isTrackVisible(track.id) || !project.isTrackActive(track.id))
            continue;
        ControlSurfaceTrack state { track.id, track.name, normalisedVolume(track.volumeDecibels),
            (track.pan + 1.0f) * 0.5f, track.armed, track.solo, track.muted };
        if (track.type == TrackType::master)
            result.master = std::move(state);
        else
            result.tracks.push_back(std::move(state));
    }
    return result;
}

std::optional<ControlSurfaceMixEdit> prepareControlSurfaceMixEdit(
    const Project& project, std::span<const ControlSurfaceEvent> events,
    double positionSeconds, juce::String& error)
{
    using Type = ControlSurfaceEvent::Type;
    if (!std::isfinite(positionSeconds))
    {
        error = "The control-surface transport position is invalid.";
        return std::nullopt;
    }
    std::map<std::pair<juce::String, Type>, ControlSurfaceEvent> latest;
    for (const auto& event : events)
    {
        const auto* track = project.findTrack(event.trackId);
        if (event.projectId != project.id || track == nullptr || track->parentTrackId.isNotEmpty()
            || track->type == TrackType::folder || (event.type != Type::volume && event.type != Type::pan)
            || !std::isfinite(event.value) || event.value < 0.0f || event.value > 1.0f)
        {
            error = "A control-surface mix target or value is no longer valid.";
            return std::nullopt;
        }
        latest[{ event.trackId, event.type }] = event;
    }
    ControlSurfaceMixEdit result;
    std::map<juce::String, std::pair<TrackMixState, TrackMixState>> mixes;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    const auto seconds = std::max(0.0, positionSeconds);
    for (const auto& [key, event] : latest)
    {
        const auto& track = *project.findTrack(key.first);
        const auto before = event.type == Type::volume
            ? normalisedVolume(track.volumeDecibels) : (track.pan + 1.0f) * 0.5f;
        const auto tolerance = event.type == Type::volume ? 0.5f / 16383.0f + 1e-7f : 1e-6f;
        if (std::abs(event.value - before) <= tolerance)
            continue;
        AutomationTarget target;
        target.trackId = track.id;
        target.type = event.type == Type::pan ? AutomationTargetType::trackPan
            : track.type == TrackType::vca ? AutomationTargetType::vcaVolume : AutomationTargetType::trackVolume;
        const auto name = event.type == Type::pan ? juce::String("Track pan")
            : track.type == TrackType::vca ? juce::String("VCA volume") : juce::String("Track volume");
        const auto preview = track.automationArmed && track.automationMode == AutomationMode::preview;
        if (preview)
            result.previews.push_back({ target, name, seconds, seconds, before, event.value });
        else
        {
            if (!mixes.contains(track.id))
            {
                const auto mix = TrackMixState::fromTrack(track);
                mixes.emplace(track.id, std::pair(mix, mix));
            }
            auto& after = mixes.at(track.id).second;
            if (event.type == Type::volume)
                after.volumeDecibels = event.value * 72.0f - 60.0f;
            else
                after.pan = event.value * 2.0f - 1.0f;
        }
        if (!track.automationArmed || track.automationMode == AutomationMode::read || preview)
            continue;
        const auto lane = std::find_if(project.automationLanes.begin(), project.automationLanes.end(),
            [&target](const auto& value) { return value.target == target; });
        const auto position = lane != project.automationLanes.end() && lane->timebase == AutomationTimebase::beats
            ? project.beatsAt(seconds) : seconds;
        const auto written = AutomationRecorder::writeGesture(
            lane != project.automationLanes.end() ? &*lane : nullptr, target, name, track.automationMode,
            { position, position, before, event.value });
        if (!written)
        {
            error = "The control-surface automation update could not be prepared.";
            return std::nullopt;
        }
        if (lane == project.automationLanes.end())
            commands.push_back(std::make_unique<AddAutomationLaneCommand>(*written));
        else
            commands.push_back(std::make_unique<SetAutomationLaneCommand>(*lane, *written));
    }
    for (const auto& [id, mix] : mixes)
        commands.push_back(std::make_unique<SetTrackMixCommand>(id, mix.first, mix.second));
    if (!commands.empty())
        result.command = std::make_unique<BatchProjectCommand>("Control-surface mix", std::move(commands));
    return result;
}
}
