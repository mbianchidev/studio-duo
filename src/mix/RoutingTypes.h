#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <optional>

namespace studio
{
enum class SignalType
{
    audio,
    midi
};

enum class ChannelLayout
{
    mono,
    stereo
};

enum class RouteTap
{
    preFader,
    postFader
};

enum class RouteKind
{
    mainOutput,
    send,
    sidechain,
    hardwareOutput,
    controlRoom
};

enum class RouteEndpointType
{
    track,
    pluginSidechain,
    hardwareOutput
};

struct RouteEndpoint
{
    RouteEndpointType type = RouteEndpointType::track;
    juce::String trackId;
    juce::String insertId;
    int busIndex = 0;
    int firstChannel = 0;
    int channels = 2;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<RouteEndpoint> fromVar(const juce::var& value,
                                                juce::String& error);
};

struct RoutingConnection
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Route" };
    SignalType signalType = SignalType::audio;
    RouteKind kind = RouteKind::send;
    RouteTap tap = RouteTap::postFader;
    juce::String sourceTrackId;
    RouteEndpoint destination;
    float gainDecibels = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool enabled = true;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<RoutingConnection> fromVar(const juce::var& value,
                                                    juce::String& error);
};

juce::String signalTypeToString(SignalType value);
std::optional<SignalType> signalTypeFromString(const juce::String& value);
juce::String channelLayoutToString(ChannelLayout value);
std::optional<ChannelLayout> channelLayoutFromString(const juce::String& value);
juce::String routeTapToString(RouteTap value);
std::optional<RouteTap> routeTapFromString(const juce::String& value);
juce::String routeKindToString(RouteKind value);
std::optional<RouteKind> routeKindFromString(const juce::String& value);
juce::String routeEndpointTypeToString(RouteEndpointType value);
std::optional<RouteEndpointType> routeEndpointTypeFromString(
    const juce::String& value);
}
