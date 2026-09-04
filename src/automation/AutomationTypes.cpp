#include "AutomationTypes.h"

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
const juce::DynamicObject* objectFor(const juce::var& value,
                                     juce::String& error,
                                     const juce::String& name)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        error = name + " must be a JSON object.";
    return object;
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

int integerProperty(const juce::DynamicObject& object,
                    const juce::Identifier& name,
                    int fallback)
{
    return static_cast<int>(numberProperty(object, name, fallback));
}

bool booleanProperty(const juce::DynamicObject& object,
                     const juce::Identifier& name,
                     bool fallback)
{
    const auto value = object.getProperty(name);
    return value.isBool() ? static_cast<bool>(value) : fallback;
}
}

juce::var AutomationTarget::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("type", automationTargetTypeToString(type));
    object->setProperty("trackId", trackId);
    object->setProperty("routeId", routeId);
    object->setProperty("insertId", insertId);
    object->setProperty("parameterId", parameterId);
    object->setProperty("parameterIndex", parameterIndex);
    return juce::var(object.release());
}

std::optional<AutomationTarget> AutomationTarget::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = objectFor(value, error, "Automation target");
    if (object == nullptr)
        return std::nullopt;
    const auto type = automationTargetTypeFromString(
        object->getProperty("type").toString());
    if (!type.has_value())
    {
        error = "Automation target type is unsupported.";
        return std::nullopt;
    }
    AutomationTarget target;
    target.type = *type;
    target.trackId = object->getProperty("trackId").toString();
    target.routeId = object->getProperty("routeId").toString();
    target.insertId = object->getProperty("insertId").toString();
    target.parameterId = object->getProperty("parameterId").toString();
    target.parameterIndex = integerProperty(*object, "parameterIndex", -1);
    if (target.trackId.isEmpty())
    {
        error = "Automation targets require a track ID.";
        return std::nullopt;
    }
    return target;
}

juce::var AutomationPoint::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("position", position);
    object->setProperty("value", value);
    return juce::var(object.release());
}

std::optional<AutomationPoint> AutomationPoint::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = objectFor(value, error, "Automation point");
    if (object == nullptr)
        return std::nullopt;
    AutomationPoint point;
    point.id = object->getProperty("id").toString();
    point.position = numberProperty(*object, "position", -1.0);
    point.value = numberProperty(*object, "value", 0.0);
    if (point.id.isEmpty()
        || point.position < 0.0
        || !std::isfinite(point.position)
        || !std::isfinite(point.value))
    {
        error = "Automation points require valid IDs, positions, and values.";
        return std::nullopt;
    }
    return point;
}

juce::var AutomationLane::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("target", target.toVar());
    object->setProperty("timebase", automationTimebaseToString(timebase));
    object->setProperty(
        "interpolation",
        automationInterpolationToString(interpolation));
    object->setProperty("trimOffset", trimOffset);
    object->setProperty("enabled", enabled);
    juce::Array<juce::var> pointValues;
    for (const auto& point : points)
        pointValues.add(point.toVar());
    object->setProperty("points", juce::var(pointValues));
    return juce::var(object.release());
}

std::optional<AutomationLane> AutomationLane::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = objectFor(value, error, "Automation lane");
    if (object == nullptr)
        return std::nullopt;
    auto target = AutomationTarget::fromVar(
        object->getProperty("target"),
        error);
    const auto timebase = automationTimebaseFromString(
        object->getProperty("timebase").toString());
    const auto interpolation = automationInterpolationFromString(
        object->getProperty("interpolation").toString());
    if (!target.has_value()
        || !timebase.has_value()
        || !interpolation.has_value())
        return std::nullopt;

    AutomationLane lane;
    lane.id = object->getProperty("id").toString();
    lane.name = object->getProperty("name").toString();
    lane.target = std::move(*target);
    lane.timebase = *timebase;
    lane.interpolation = *interpolation;
    lane.trimOffset = numberProperty(*object, "trimOffset", 0.0);
    lane.enabled = booleanProperty(*object, "enabled", true);
    const auto pointValues = object->getProperty("points");
    if (!pointValues.isArray())
    {
        error = "Automation lane points must be an array.";
        return std::nullopt;
    }
    for (const auto& pointValue : *pointValues.getArray())
    {
        auto point = AutomationPoint::fromVar(pointValue, error);
        if (!point.has_value())
            return std::nullopt;
        lane.points.push_back(std::move(*point));
    }
    std::stable_sort(
        lane.points.begin(),
        lane.points.end(),
        [](const auto& left, const auto& right)
        {
            return left.position < right.position;
        });
    if (lane.id.isEmpty()
        || lane.name.trim().isEmpty()
        || !std::isfinite(lane.trimOffset))
    {
        error = "Automation lanes require valid IDs, names, and trim values.";
        return std::nullopt;
    }
    return lane;
}

juce::String automationModeToString(AutomationMode value)
{
    switch (value)
    {
        case AutomationMode::read: return "read";
        case AutomationMode::touch: return "touch";
        case AutomationMode::latch: return "latch";
        case AutomationMode::write: return "write";
        case AutomationMode::trim: return "trim";
        case AutomationMode::preview: return "preview";
    }
    return "read";
}

std::optional<AutomationMode> automationModeFromString(
    const juce::String& value)
{
    if (value == "read") return AutomationMode::read;
    if (value == "touch") return AutomationMode::touch;
    if (value == "latch") return AutomationMode::latch;
    if (value == "write") return AutomationMode::write;
    if (value == "trim") return AutomationMode::trim;
    if (value == "preview") return AutomationMode::preview;
    return std::nullopt;
}

juce::String automationTimebaseToString(AutomationTimebase value)
{
    return value == AutomationTimebase::beats ? "beats" : "seconds";
}

std::optional<AutomationTimebase> automationTimebaseFromString(
    const juce::String& value)
{
    if (value == "seconds") return AutomationTimebase::seconds;
    if (value == "beats") return AutomationTimebase::beats;
    return std::nullopt;
}

juce::String automationInterpolationToString(
    AutomationInterpolation value)
{
    return value == AutomationInterpolation::step ? "step" : "linear";
}

std::optional<AutomationInterpolation> automationInterpolationFromString(
    const juce::String& value)
{
    if (value == "step") return AutomationInterpolation::step;
    if (value == "linear") return AutomationInterpolation::linear;
    return std::nullopt;
}

juce::String automationTargetTypeToString(AutomationTargetType value)
{
    switch (value)
    {
        case AutomationTargetType::trackVolume: return "trackVolume";
        case AutomationTargetType::trackPan: return "trackPan";
        case AutomationTargetType::trackMute: return "trackMute";
        case AutomationTargetType::trackPolarity: return "trackPolarity";
        case AutomationTargetType::vcaVolume: return "vcaVolume";
        case AutomationTargetType::sendGain: return "sendGain";
        case AutomationTargetType::sendPan: return "sendPan";
        case AutomationTargetType::sendMute: return "sendMute";
        case AutomationTargetType::controlRoomDim: return "controlRoomDim";
        case AutomationTargetType::pluginParameter: return "pluginParameter";
        case AutomationTargetType::deviceParameter: return "deviceParameter";
    }
    return "trackVolume";
}

std::optional<AutomationTargetType> automationTargetTypeFromString(
    const juce::String& value)
{
    if (value == "trackVolume") return AutomationTargetType::trackVolume;
    if (value == "trackPan") return AutomationTargetType::trackPan;
    if (value == "trackMute") return AutomationTargetType::trackMute;
    if (value == "trackPolarity") return AutomationTargetType::trackPolarity;
    if (value == "vcaVolume") return AutomationTargetType::vcaVolume;
    if (value == "sendGain") return AutomationTargetType::sendGain;
    if (value == "sendPan") return AutomationTargetType::sendPan;
    if (value == "sendMute") return AutomationTargetType::sendMute;
    if (value == "controlRoomDim") return AutomationTargetType::controlRoomDim;
    if (value == "pluginParameter") return AutomationTargetType::pluginParameter;
    if (value == "deviceParameter") return AutomationTargetType::deviceParameter;
    return std::nullopt;
}
}
