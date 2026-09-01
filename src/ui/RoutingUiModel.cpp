#include "RoutingUiModel.h"

#include <algorithm>

namespace studio
{
std::vector<RoutingDestinationItem> RoutingUiModel::sendDestinations(
    const Project& project,
    const juce::String& sourceTrackId)
{
    std::vector<RoutingDestinationItem> result;
    for (const auto& track : project.tracks)
    {
        if (track.parentTrackId.isNotEmpty()
            || track.id == sourceTrackId
            || (track.type != TrackType::aux
                && track.type != TrackType::bus))
            continue;

        RoutingConnection candidate;
        candidate.name = "Send";
        candidate.kind = RouteKind::send;
        candidate.sourceTrackId = sourceTrackId;
        candidate.destination.type = RouteEndpointType::track;
        candidate.destination.trackId = track.id;
        auto copy = project;
        copy.routingConnections.push_back(std::move(candidate));
        juce::String error;
        if (!copy.validateRoutingGraph(error))
            continue;
        result.push_back({
            track.id,
            {},
            (track.type == TrackType::aux ? "Aux: " : "Bus: ") + track.name
        });
    }
    return result;
}

std::vector<RoutingDestinationItem> RoutingUiModel::sidechainDestinations(
    const Project& project,
    const juce::String& sourceTrackId)
{
    std::vector<RoutingDestinationItem> result;
    for (const auto& track : project.tracks)
    {
        if (track.parentTrackId.isNotEmpty() || track.id == sourceTrackId)
            continue;
        for (const auto& insert : track.inserts)
        {
            RoutingConnection candidate;
            candidate.name = "Sidechain";
            candidate.kind = RouteKind::sidechain;
            candidate.sourceTrackId = sourceTrackId;
            candidate.destination.type = RouteEndpointType::pluginSidechain;
            candidate.destination.trackId = track.id;
            candidate.destination.insertId = insert.id;
            auto copy = project;
            copy.routingConnections.push_back(std::move(candidate));
            juce::String error;
            if (!copy.validateRoutingGraph(error))
                continue;
            result.push_back({
                track.id,
                insert.id,
                track.name + ": " + insert.name
            });
        }
    }
    return result;
}

juce::String RoutingUiModel::summary(
    const Project& project,
    const RoutingConnection& connection)
{
    juce::String destination;
    if (connection.destination.type == RouteEndpointType::hardwareOutput)
    {
        destination = "Hardware "
            + juce::String(connection.destination.firstChannel + 1)
            + "-"
            + juce::String(connection.destination.firstChannel
                           + connection.destination.channels);
    }
    else
    {
        const auto* track = project.findTrack(connection.destination.trackId);
        destination = track != nullptr ? track->name : juce::String("Missing");
        if (connection.destination.type == RouteEndpointType::pluginSidechain)
        {
            const auto insert = track != nullptr
                ? std::find_if(
                      track->inserts.cbegin(),
                      track->inserts.cend(),
                      [&connection](const auto& candidate)
                      {
                          return candidate.id
                              == connection.destination.insertId;
                      })
                : std::vector<PluginInsert>::const_iterator {};
            destination << " / "
                        << (track != nullptr
                                    && insert != track->inserts.cend()
                                ? insert->name
                                : juce::String("Missing insert"));
        }
    }

    const auto tap = juce::String(
        connection.tap == RouteTap::preFader ? "Pre" : "Post");
    return tap
        + " "
        + routeKindToString(connection.kind)
        + " -> "
        + destination
        + "  "
        + juce::String(connection.gainDecibels, 1)
        + " dB";
}
}
