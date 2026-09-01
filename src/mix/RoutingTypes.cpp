#include "RoutingTypes.h"

#include <cmath>

namespace studio
{
namespace
{
const juce::DynamicObject* requireObject(const juce::var& value,
                                         juce::String& error,
                                         const juce::String& type)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        error = type + " must be a JSON object.";
    return object;
}

int integerProperty(const juce::DynamicObject& object,
                    const juce::Identifier& name,
                    int fallback)
{
    const auto value = object.getProperty(name);
    return value.isInt() || value.isInt64() || value.isDouble()
        ? static_cast<int>(value)
        : fallback;
}

double numberProperty(const juce::DynamicObject& object,
                      const juce::Identifier& name,
                      double fallback)
{
    const auto value = object.getProperty(name);
    return value.isInt() || value.isInt64() || value.isDouble()
        ? static_cast<double>(value)
        : fallback;
}

bool booleanProperty(const juce::DynamicObject& object,
                     const juce::Identifier& name,
                     bool fallback)
{
    const auto value = object.getProperty(name);
    return value.isBool() ? static_cast<bool>(value) : fallback;
}
}

juce::var RouteEndpoint::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("type", routeEndpointTypeToString(type));
    object->setProperty("trackId", trackId);
    object->setProperty("insertId", insertId);
    object->setProperty("busIndex", busIndex);
    object->setProperty("firstChannel", firstChannel);
    object->setProperty("channels", channels);
    return juce::var(object.release());
}

std::optional<RouteEndpoint> RouteEndpoint::fromVar(const juce::var& value,
                                                    juce::String& error)
{
    const auto* object = requireObject(value, error, "Route endpoint");
    if (object == nullptr)
        return std::nullopt;

    const auto type = routeEndpointTypeFromString(
        object->getProperty("type").toString());
    if (!type.has_value())
    {
        error = "Route endpoint contains an unsupported type.";
        return std::nullopt;
    }

    RouteEndpoint endpoint;
    endpoint.type = *type;
    endpoint.trackId = object->getProperty("trackId").toString();
    endpoint.insertId = object->getProperty("insertId").toString();
    endpoint.busIndex = std::max(0, integerProperty(*object, "busIndex", 0));
    endpoint.firstChannel = std::max(
        0,
        integerProperty(*object, "firstChannel", 0));
    endpoint.channels = juce::jlimit(
        1,
        16,
        integerProperty(*object, "channels", 2));

    if ((endpoint.type == RouteEndpointType::track
         || endpoint.type == RouteEndpointType::pluginSidechain)
        && endpoint.trackId.isEmpty())
    {
        error = "Track route endpoints require a destination track ID.";
        return std::nullopt;
    }
    if (endpoint.type == RouteEndpointType::pluginSidechain
        && endpoint.insertId.isEmpty())
    {
        error = "Sidechain route endpoints require a plugin insert ID.";
        return std::nullopt;
    }
    return endpoint;
}

juce::var RoutingConnection::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("signalType", signalTypeToString(signalType));
    object->setProperty("kind", routeKindToString(kind));
    object->setProperty("tap", routeTapToString(tap));
    object->setProperty("sourceTrackId", sourceTrackId);
    object->setProperty("destination", destination.toVar());
    object->setProperty("gainDecibels", gainDecibels);
    object->setProperty("pan", pan);
    object->setProperty("muted", muted);
    object->setProperty("enabled", enabled);
    return juce::var(object.release());
}

std::optional<RoutingConnection> RoutingConnection::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Routing connection");
    if (object == nullptr)
        return std::nullopt;

    const auto signalType = signalTypeFromString(
        object->getProperty("signalType").toString());
    const auto kind = routeKindFromString(object->getProperty("kind").toString());
    const auto tap = routeTapFromString(object->getProperty("tap").toString());
    if (!signalType.has_value() || !kind.has_value() || !tap.has_value())
    {
        error = "Routing connection contains an unsupported type.";
        return std::nullopt;
    }

    auto destination = RouteEndpoint::fromVar(
        object->getProperty("destination"),
        error);
    if (!destination.has_value())
        return std::nullopt;

    RoutingConnection connection;
    connection.id = object->getProperty("id").toString();
    connection.name = object->getProperty("name").toString();
    connection.signalType = *signalType;
    connection.kind = *kind;
    connection.tap = *tap;
    connection.sourceTrackId = object->getProperty("sourceTrackId").toString();
    connection.destination = std::move(*destination);
    connection.gainDecibels = static_cast<float>(
        numberProperty(*object, "gainDecibels", 0.0));
    connection.pan = juce::jlimit(
        -1.0f,
        1.0f,
        static_cast<float>(numberProperty(*object, "pan", 0.0)));
    connection.muted = booleanProperty(*object, "muted", false);
    connection.enabled = booleanProperty(*object, "enabled", true);

    if (connection.id.isEmpty()
        || connection.name.trim().isEmpty()
        || connection.sourceTrackId.isEmpty()
        || !std::isfinite(connection.gainDecibels)
        || !std::isfinite(connection.pan))
    {
        error = "Routing connections require valid IDs, names, sources, and levels.";
        return std::nullopt;
    }
    return connection;
}

juce::String signalTypeToString(SignalType value)
{
    switch (value)
    {
        case SignalType::audio: return "audio";
        case SignalType::midi: return "midi";
    }
    return "audio";
}

std::optional<SignalType> signalTypeFromString(const juce::String& value)
{
    if (value == "audio")
        return SignalType::audio;
    if (value == "midi")
        return SignalType::midi;
    return std::nullopt;
}

juce::String channelLayoutToString(ChannelLayout value)
{
    switch (value)
    {
        case ChannelLayout::mono: return "mono";
        case ChannelLayout::stereo: return "stereo";
    }
    return "stereo";
}

std::optional<ChannelLayout> channelLayoutFromString(const juce::String& value)
{
    if (value == "mono")
        return ChannelLayout::mono;
    if (value == "stereo")
        return ChannelLayout::stereo;
    return std::nullopt;
}

juce::String routeTapToString(RouteTap value)
{
    switch (value)
    {
        case RouteTap::preFader: return "preFader";
        case RouteTap::postFader: return "postFader";
    }
    return "postFader";
}

std::optional<RouteTap> routeTapFromString(const juce::String& value)
{
    if (value == "preFader")
        return RouteTap::preFader;
    if (value == "postFader")
        return RouteTap::postFader;
    return std::nullopt;
}

juce::String routeKindToString(RouteKind value)
{
    switch (value)
    {
        case RouteKind::mainOutput: return "mainOutput";
        case RouteKind::send: return "send";
        case RouteKind::sidechain: return "sidechain";
        case RouteKind::hardwareOutput: return "hardwareOutput";
        case RouteKind::controlRoom: return "controlRoom";
    }
    return "send";
}

std::optional<RouteKind> routeKindFromString(const juce::String& value)
{
    if (value == "mainOutput")
        return RouteKind::mainOutput;
    if (value == "send")
        return RouteKind::send;
    if (value == "sidechain")
        return RouteKind::sidechain;
    if (value == "hardwareOutput")
        return RouteKind::hardwareOutput;
    if (value == "controlRoom")
        return RouteKind::controlRoom;
    return std::nullopt;
}

juce::String routeEndpointTypeToString(RouteEndpointType value)
{
    switch (value)
    {
        case RouteEndpointType::track: return "track";
        case RouteEndpointType::pluginSidechain: return "pluginSidechain";
        case RouteEndpointType::hardwareOutput: return "hardwareOutput";
    }
    return "track";
}

std::optional<RouteEndpointType> routeEndpointTypeFromString(
    const juce::String& value)
{
    if (value == "track")
        return RouteEndpointType::track;
    if (value == "pluginSidechain")
        return RouteEndpointType::pluginSidechain;
    if (value == "hardwareOutput")
        return RouteEndpointType::hardwareOutput;
    return std::nullopt;
}
}
