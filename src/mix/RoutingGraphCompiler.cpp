#include "RoutingGraphCompiler.h"

#include "SoloResolver.h"

#include <algorithm>

namespace studio
{
namespace
{
int trackIndex(const CompiledRoutingGraph& graph, const juce::String& trackId)
{
    const auto iterator = std::find_if(
        graph.tracks.cbegin(),
        graph.tracks.cend(),
        [&trackId](const auto& track)
        {
            return track.id == trackId;
        });
    return iterator == graph.tracks.cend()
        ? -1
        : static_cast<int>(std::distance(graph.tracks.cbegin(), iterator));
}

float vcaGainFor(const Project& project, const juce::String& trackId)
{
    auto gain = 1.0f;
    for (const auto& track : project.tracks)
    {
        if (track.type != TrackType::vca
            || std::find(track.controlledTrackIds.cbegin(),
                         track.controlledTrackIds.cend(),
                         trackId) == track.controlledTrackIds.cend())
            continue;

        if (track.muted)
            return 0.0f;
        gain *= juce::Decibels::decibelsToGain(track.volumeDecibels);
    }
    return gain;
}

CompiledRoutingConnection compileConnection(
    const RoutingConnection& connection,
    const CompiledRoutingGraph& graph)
{
    CompiledRoutingConnection compiled;
    compiled.id = connection.id;
    compiled.kind = connection.kind;
    compiled.tap = connection.tap;
    compiled.sourceTrackId = connection.sourceTrackId;
    compiled.destinationTrackId = connection.destination.trackId;
    compiled.destinationInsertId = connection.destination.insertId;
    compiled.sourceTrackIndex = connection.sourceTrackId == graph.masterTrackId
        ? -1
        : trackIndex(graph, connection.sourceTrackId);
    compiled.destinationTrackIndex = trackIndex(
        graph,
        connection.destination.trackId);
    compiled.destinationBusIndex = connection.destination.busIndex;
    compiled.hardwareFirstChannel = connection.destination.firstChannel;
    compiled.hardwareChannels =
        connection.destination.type == RouteEndpointType::hardwareOutput
        ? connection.destination.channels
        : 0;
    compiled.gain = connection.muted
        ? 0.0f
        : juce::Decibels::decibelsToGain(connection.gainDecibels);
    compiled.pan = connection.pan;
    return compiled;
}
}

const CompiledRoutingTrack* CompiledRoutingGraph::findTrack(
    const juce::String& trackId) const
{
    const auto iterator = std::find_if(
        tracks.cbegin(),
        tracks.cend(),
        [&trackId](const auto& track)
        {
            return track.id == trackId;
        });
    return iterator == tracks.cend() ? nullptr : &*iterator;
}

std::optional<CompiledRoutingGraph> RoutingGraphCompiler::compile(
    const Project& project,
    juce::String& error)
{
    const auto order = project.routingGraphOrder(error);
    if (!order.has_value())
        return std::nullopt;
    const auto solo = SoloResolver::resolve(project, error);
    if (!solo.has_value())
        return std::nullopt;

    CompiledRoutingGraph graph;
    graph.masterTrackId = project.masterTrackId();
    graph.tracks.reserve(order->size());
    for (const auto& trackId : *order)
    {
        const auto* track = project.findTrack(trackId);
        if (track == nullptr)
        {
            error = "A compiled routing track became unavailable.";
            return std::nullopt;
        }
        graph.tracks.push_back({
            track->id,
            track->type,
            track->channelLayout,
            vcaGainFor(project, track->id),
            track->polarityInverted,
            solo->isAudible(track->id),
            solo->isProcessing(track->id)
        });
    }

    for (const auto& track : project.tracks)
    {
        if (track.parentTrackId.isNotEmpty()
            || track.type == TrackType::master
            || track.type == TrackType::folder
            || track.type == TrackType::vca
            || track.type == TrackType::midi
            || track.type == TrackType::controlRoom)
            continue;

        const auto explicitRoute = std::find_if(
            project.routingConnections.cbegin(),
            project.routingConnections.cend(),
            [&track](const auto& connection)
            {
                return connection.enabled
                    && connection.signalType == SignalType::audio
                    && connection.kind == RouteKind::mainOutput
                    && connection.sourceTrackId == track.id;
            });
        if (explicitRoute != project.routingConnections.cend())
        {
            graph.routes.push_back(compileConnection(*explicitRoute, graph));
            continue;
        }

        RoutingConnection implicit;
        implicit.id = "main:" + track.id;
        implicit.name = "Main output";
        implicit.kind = RouteKind::mainOutput;
        implicit.sourceTrackId = track.id;
        implicit.destination.type = RouteEndpointType::track;
        implicit.destination.trackId = project.resolvedOutputTrackId(track);
        graph.routes.push_back(compileConnection(implicit, graph));
    }

    for (const auto& connection : project.routingConnections)
    {
        if (!connection.enabled
            || connection.signalType != SignalType::audio
            || connection.kind == RouteKind::mainOutput)
            continue;
        graph.routes.push_back(compileConnection(connection, graph));
    }
    return graph;
}
}
