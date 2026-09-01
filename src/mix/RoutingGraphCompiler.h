#pragma once

#include "RoutingTypes.h"
#include "model/ProjectModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>
#include <vector>

namespace studio
{
struct CompiledRoutingTrack
{
    juce::String id;
    TrackType type = TrackType::audio;
    ChannelLayout channelLayout = ChannelLayout::stereo;
    float vcaGain = 1.0f;
    bool polarityInverted = false;
    bool audible = true;
    bool processing = true;
};

struct CompiledRoutingConnection
{
    juce::String id;
    RouteKind kind = RouteKind::send;
    RouteTap tap = RouteTap::postFader;
    juce::String sourceTrackId;
    juce::String destinationTrackId;
    juce::String destinationInsertId;
    int sourceTrackIndex = -1;
    int destinationTrackIndex = -1;
    int destinationBusIndex = 0;
    int hardwareFirstChannel = 0;
    int hardwareChannels = 0;
    float gain = 1.0f;
    float pan = 0.0f;
};

struct CompiledRoutingGraph
{
    juce::String masterTrackId;
    std::vector<CompiledRoutingTrack> tracks;
    std::vector<CompiledRoutingConnection> routes;

    [[nodiscard]] const CompiledRoutingTrack* findTrack(
        const juce::String& trackId) const;
};

class RoutingGraphCompiler
{
public:
    static std::optional<CompiledRoutingGraph> compile(
        const Project& project,
        juce::String& error);
};
}
