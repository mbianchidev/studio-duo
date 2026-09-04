#include "SoloResolver.h"

#include "model/ProjectModel.h"

#include <algorithm>

namespace studio
{
namespace
{
bool contains(const std::vector<juce::String>& values,
              const juce::String& value)
{
    return std::find(values.cbegin(), values.cend(), value) != values.cend();
}

void addUnique(std::vector<juce::String>& values, const juce::String& value)
{
    if (value.isNotEmpty() && !contains(values, value))
        values.push_back(value);
}

bool isMixTrack(const Track& track)
{
    return track.parentTrackId.isEmpty()
        && (track.type == TrackType::audio
            || track.type == TrackType::instrument
            || track.type == TrackType::aux
            || track.type == TrackType::bus
            || track.type == TrackType::controlRoom);
}

bool folderMuted(const Project& project, const Track& track)
{
    auto folderId = track.folderTrackId;
    std::vector<juce::String> visited;
    while (folderId.isNotEmpty())
    {
        if (contains(visited, folderId))
            return true;
        addUnique(visited, folderId);
        const auto* folder = project.findTrack(folderId);
        if (folder == nullptr)
            return true;
        if (folder->muted)
            return true;
        folderId = folder->folderTrackId;
    }
    return false;
}

bool vcaMuted(const Project& project, const juce::String& trackId)
{
    return std::any_of(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [&trackId](const auto& track)
        {
            return track.type == TrackType::vca
                && track.muted
                && contains(track.controlledTrackIds, trackId);
        });
}

struct Edge
{
    juce::String source;
    juce::String destination;
    bool audible = true;
};
}

bool SoloResolution::isAudible(const juce::String& trackId) const
{
    return contains(audibleTrackIds, trackId);
}

bool SoloResolution::isProcessing(const juce::String& trackId) const
{
    return contains(processingTrackIds, trackId);
}

std::optional<SoloResolution> SoloResolver::resolve(const Project& project,
                                                    juce::String& error)
{
    if (!project.validateRoutingGraph(error))
        return std::nullopt;

    std::vector<Edge> edges;
    for (const auto& track : project.tracks)
    {
        if (!isMixTrack(track)
            || track.type == TrackType::controlRoom)
            continue;
        const auto destinationId = project.resolvedOutputTrackId(track);
        if (destinationId != project.masterTrackId())
            edges.push_back({ track.id, destinationId, true });
    }
    for (const auto& connection : project.routingConnections)
    {
        if (!connection.enabled
            || connection.signalType != SignalType::audio
            || connection.kind == RouteKind::mainOutput
            || connection.kind == RouteKind::hardwareOutput
            || connection.destination.trackId.isEmpty())
            continue;
        edges.push_back({
            connection.sourceTrackId,
            connection.destination.trackId,
            connection.kind != RouteKind::sidechain
        });
    }

    const auto anySolo = std::any_of(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [](const auto& track)
        {
            return track.type != TrackType::master && track.solo;
        });

    SoloResolution resolution;
    std::vector<juce::String> soloAnchors;
    std::vector<juce::String> soloSafeTracks;
    for (const auto& track : project.tracks)
    {
        if (!isMixTrack(track))
        {
            if (track.parentTrackId.isNotEmpty() && track.solo)
                addUnique(soloAnchors, track.parentTrackId);
            continue;
        }

        if (!anySolo)
            addUnique(resolution.audibleTrackIds, track.id);
        else if (track.solo)
            addUnique(soloAnchors, track.id);
        if (track.soloSafe)
            addUnique(soloSafeTracks, track.id);
    }

    if (anySolo)
    {
        for (const auto& control : project.tracks)
        {
            if (!control.solo)
                continue;
            if (control.type == TrackType::folder)
            {
                for (const auto& track : project.tracks)
                {
                    auto folderId = track.folderTrackId;
                    while (folderId.isNotEmpty())
                    {
                        if (folderId == control.id)
                        {
                            addUnique(soloAnchors, track.id);
                            break;
                        }
                        const auto* folder = project.findTrack(folderId);
                        folderId = folder != nullptr
                            ? folder->folderTrackId
                            : juce::String();
                    }
                }
            }
            else if (control.type == TrackType::vca)
            {
                for (const auto& controlledId : control.controlledTrackIds)
                    addUnique(soloAnchors, controlledId);
            }
        }

        auto upstream = soloAnchors;
        for (auto changed = true; changed;)
        {
            changed = false;
            for (const auto& edge : edges)
            {
                if (!edge.audible)
                    continue;
                if (contains(upstream, edge.destination)
                    && !contains(upstream, edge.source))
                {
                    addUnique(upstream, edge.source);
                    changed = true;
                }
            }
        }

        resolution.audibleTrackIds = upstream;
        for (const auto& trackId : soloSafeTracks)
            addUnique(resolution.audibleTrackIds, trackId);
        for (auto changed = true; changed;)
        {
            changed = false;
            for (const auto& edge : edges)
            {
                if (!edge.audible)
                    continue;
                if (contains(resolution.audibleTrackIds, edge.source)
                    && !contains(resolution.audibleTrackIds,
                                 edge.destination))
                {
                    addUnique(resolution.audibleTrackIds,
                              edge.destination);
                    changed = true;
                }
            }
        }
    }

    resolution.processingTrackIds = resolution.audibleTrackIds;
    for (auto changed = true; changed;)
    {
        changed = false;
        for (const auto& edge : edges)
        {
            if (contains(resolution.processingTrackIds, edge.destination)
                && !contains(resolution.processingTrackIds, edge.source))
            {
                addUnique(resolution.processingTrackIds, edge.source);
                changed = true;
            }
            if (edge.audible
                && contains(resolution.processingTrackIds, edge.source)
                && !contains(resolution.processingTrackIds, edge.destination))
            {
                addUnique(resolution.processingTrackIds, edge.destination);
                changed = true;
            }
        }
    }

    resolution.audibleTrackIds.erase(
        std::remove_if(
            resolution.audibleTrackIds.begin(),
            resolution.audibleTrackIds.end(),
            [&project](const auto& trackId)
            {
                const auto* track = project.findTrack(trackId);
                return track == nullptr
                    || track->muted
                    || folderMuted(project, *track)
                    || vcaMuted(project, trackId);
            }),
        resolution.audibleTrackIds.end());
    return resolution;
}
}
