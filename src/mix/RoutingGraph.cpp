#include "RoutingGraph.h"

#include "model/ProjectModel.h"

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
bool isAudioNode(const Track& track)
{
    return track.parentTrackId.isEmpty()
        && (track.type == TrackType::audio
            || track.type == TrackType::instrument
            || track.type == TrackType::aux
            || track.type == TrackType::bus
            || track.type == TrackType::controlRoom);
}

bool hasInsert(const Track& track, const juce::String& insertId)
{
    return std::any_of(
        track.inserts.cbegin(),
        track.inserts.cend(),
        [&insertId](const auto& insert)
        {
            return insert.id == insertId;
        });
}

bool validateTrackRoles(const Project& project, juce::String& error)
{
    std::vector<juce::String> ids;
    auto masterCount = 0;
    auto controlRoomCount = 0;
    for (const auto& track : project.tracks)
    {
        if (track.id.isEmpty()
            || std::find(ids.cbegin(), ids.cend(), track.id) != ids.cend())
        {
            error = "Track IDs must be non-empty and unique.";
            return false;
        }
        ids.push_back(track.id);
        masterCount += track.type == TrackType::master ? 1 : 0;
        controlRoomCount += track.type == TrackType::controlRoom ? 1 : 0;
        if (track.hardwareOutputChannel < 0
            || !std::isfinite(track.controlRoomDimDecibels))
        {
            error = "Track hardware and control-room settings are invalid.";
            return false;
        }
    }
    if (masterCount != 1)
    {
        error = "A project must contain exactly one master track.";
        return false;
    }
    if (controlRoomCount > 1)
    {
        error = "A project can contain only one control-room track.";
        return false;
    }

    for (const auto& track : project.tracks)
    {
        std::vector<juce::String> visited { track.id };
        auto folderId = track.folderTrackId;
        while (folderId.isNotEmpty())
        {
            if (std::find(visited.cbegin(), visited.cend(), folderId)
                != visited.cend())
            {
                error = "Folder hierarchy cannot contain a cycle.";
                return false;
            }
            const auto* folder = project.findTrack(folderId);
            if (folder == nullptr
                || folder->type != TrackType::folder
                || folder->parentTrackId.isNotEmpty())
            {
                error = "Tracks can belong only to available root folders.";
                return false;
            }
            visited.push_back(folderId);
            folderId = folder->folderTrackId;
        }

        if (track.type != TrackType::vca)
        {
            if (!track.controlledTrackIds.empty())
            {
                error = "Only VCA tracks can control other tracks.";
                return false;
            }
            continue;
        }

        std::vector<juce::String> controlled;
        for (const auto& controlledId : track.controlledTrackIds)
        {
            const auto* controlledTrack = project.findTrack(controlledId);
            if (controlledTrack == nullptr
                || controlledTrack->parentTrackId.isNotEmpty()
                || controlledTrack->type == TrackType::master
                || controlledTrack->type == TrackType::folder
                || controlledTrack->type == TrackType::vca
                || controlledTrack->type == TrackType::controlRoom
                || std::find(controlled.cbegin(),
                             controlled.cend(),
                             controlledId) != controlled.cend())
            {
                error = "VCA assignments require unique controllable root tracks.";
                return false;
            }
            controlled.push_back(controlledId);
        }
    }
    return true;
}

bool validateConnection(const Project& project,
                        const RoutingConnection& connection,
                        juce::String& error)
{
    const auto* source = project.findTrack(connection.sourceTrackId);
    if (connection.id.isEmpty()
        || connection.name.trim().isEmpty()
        || source == nullptr
        || source->parentTrackId.isNotEmpty()
        || !std::isfinite(connection.gainDecibels)
        || !std::isfinite(connection.pan))
    {
        error = "Routing connections require valid IDs, names, sources, and levels.";
        return false;
    }

    if (connection.signalType == SignalType::midi)
    {
        if (connection.destination.type != RouteEndpointType::track
            || project.findTrack(connection.destination.trackId) == nullptr)
        {
            error = "MIDI routes require an available destination track.";
            return false;
        }
        return true;
    }

    if (!isAudioNode(*source) && source->type != TrackType::master)
    {
        error = "The selected source track cannot produce audio routes.";
        return false;
    }

    if (connection.kind == RouteKind::hardwareOutput)
    {
        if (connection.destination.type != RouteEndpointType::hardwareOutput
            || connection.destination.firstChannel < 0
            || connection.destination.channels < 1)
        {
            error = "Hardware routes require a valid output channel map.";
            return false;
        }
        return true;
    }

    const auto* destination = project.findTrack(connection.destination.trackId);
    if (destination == nullptr || destination->parentTrackId.isNotEmpty())
    {
        error = "A routing connection references an unavailable destination.";
        return false;
    }

    switch (connection.kind)
    {
        case RouteKind::mainOutput:
            if (source->type == TrackType::master
                || source->type == TrackType::controlRoom
                || connection.destination.type != RouteEndpointType::track
                || (destination->type != TrackType::bus
                    && destination->type != TrackType::master))
            {
                error = "Main outputs must route a mix track to a bus or the master.";
                return false;
            }
            break;
        case RouteKind::send:
            if (source->type == TrackType::master
                || connection.destination.type != RouteEndpointType::track
                || (destination->type != TrackType::aux
                    && destination->type != TrackType::bus))
            {
                error = "Sends must route a mix track to an aux or bus.";
                return false;
            }
            break;
        case RouteKind::sidechain:
            if (source->type == TrackType::master
                || connection.destination.type
                    != RouteEndpointType::pluginSidechain
                || !hasInsert(*destination, connection.destination.insertId))
            {
                error = "Sidechains require an available destination insert.";
                return false;
            }
            break;
        case RouteKind::controlRoom:
            if (connection.destination.type != RouteEndpointType::track
                || destination->type != TrackType::controlRoom)
            {
                error = "Control-room routes require a control-room destination.";
                return false;
            }
            break;
        case RouteKind::hardwareOutput:
            break;
    }

    return true;
}

std::vector<juce::String> audioNodeIds(const Project& project)
{
    std::vector<juce::String> result;
    for (const auto& track : project.tracks)
        if (isAudioNode(track) && track.type != TrackType::master)
            result.push_back(track.id);
    return result;
}
}

bool RoutingGraph::validate(const Project& project, juce::String& error)
{
    return order(project, error).has_value();
}

std::optional<std::vector<juce::String>> RoutingGraph::order(
    const Project& project,
    juce::String& error)
{
    const auto masterId = project.masterTrackId();
    if (masterId.isEmpty())
    {
        error = "The project does not contain a master track.";
        return std::nullopt;
    }
    if (!validateTrackRoles(project, error))
        return std::nullopt;

    std::vector<juce::String> routeIds;
    std::vector<juce::String> mainSources;
    for (const auto& connection : project.routingConnections)
    {
        if (std::find(routeIds.cbegin(), routeIds.cend(), connection.id)
            != routeIds.cend())
        {
            error = "Routing connection IDs must be unique.";
            return std::nullopt;
        }
        routeIds.push_back(connection.id);

        if (!validateConnection(project, connection, error))
            return std::nullopt;
        if (connection.kind == RouteKind::mainOutput)
        {
            if (std::find(mainSources.cbegin(),
                          mainSources.cend(),
                          connection.sourceTrackId)
                != mainSources.cend())
            {
                error = "A track can have only one main output.";
                return std::nullopt;
            }
            mainSources.push_back(connection.sourceTrackId);
        }
    }

    const auto nodes = audioNodeIds(project);
    std::vector<int> incoming(nodes.size(), 0);
    std::vector<std::vector<std::size_t>> outgoing(nodes.size());
    const auto addEdge = [&](const juce::String& sourceId,
                             const juce::String& destinationId)
    {
        if (destinationId == masterId)
            return true;
        const auto source = std::find(nodes.cbegin(), nodes.cend(), sourceId);
        const auto destination = std::find(
            nodes.cbegin(),
            nodes.cend(),
            destinationId);
        if (source == nodes.cend() || destination == nodes.cend())
            return false;
        const auto sourceIndex = static_cast<std::size_t>(
            std::distance(nodes.cbegin(), source));
        const auto destinationIndex = static_cast<std::size_t>(
            std::distance(nodes.cbegin(), destination));
        outgoing[sourceIndex].push_back(destinationIndex);
        ++incoming[destinationIndex];
        return true;
    };

    for (const auto& trackId : nodes)
    {
        const auto* track = project.findTrack(trackId);
        if (track == nullptr || track->type == TrackType::controlRoom)
            continue;
        if (!addEdge(trackId, project.resolvedOutputTrackId(*track)))
        {
            error = "A track routes to an unavailable destination.";
            return std::nullopt;
        }
    }

    for (const auto& connection : project.routingConnections)
    {
        if (!connection.enabled
            || connection.signalType != SignalType::audio
            || connection.kind == RouteKind::mainOutput
            || connection.kind == RouteKind::hardwareOutput)
            continue;
        if (!addEdge(connection.sourceTrackId, connection.destination.trackId))
        {
            error = "A routing connection references an unavailable audio node.";
            return std::nullopt;
        }
    }

    std::vector<juce::String> result;
    result.reserve(nodes.size());
    std::vector<bool> emitted(nodes.size(), false);
    while (result.size() < nodes.size())
    {
        auto progress = false;
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            if (emitted[index] || incoming[index] != 0)
                continue;

            emitted[index] = true;
            progress = true;
            result.push_back(nodes[index]);
            for (const auto destination : outgoing[index])
                --incoming[destination];
        }

        if (!progress)
        {
            error = "Track routing cannot contain a cycle.";
            return std::nullopt;
        }
    }
    return result;
}
}
