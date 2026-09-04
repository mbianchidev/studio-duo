#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

namespace studio
{
enum class AutomationMode
{
    read,
    touch,
    latch,
    write,
    trim,
    preview
};

enum class AutomationTimebase
{
    seconds,
    beats
};

enum class AutomationInterpolation
{
    step,
    linear
};

enum class AutomationTargetType
{
    trackVolume,
    trackPan,
    trackMute,
    trackPolarity,
    vcaVolume,
    sendGain,
    sendPan,
    sendMute,
    controlRoomDim,
    pluginParameter,
    deviceParameter
};

struct AutomationTarget
{
    AutomationTargetType type = AutomationTargetType::trackVolume;
    juce::String trackId;
    juce::String routeId;
    juce::String insertId;
    juce::String parameterId;
    int parameterIndex = -1;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<AutomationTarget> fromVar(const juce::var& value,
                                                   juce::String& error);
};

struct AutomationPoint
{
    juce::String id { juce::Uuid().toString() };
    double position = 0.0;
    double value = 0.0;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<AutomationPoint> fromVar(const juce::var& value,
                                                  juce::String& error);
};

struct AutomationLane
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Automation" };
    AutomationTarget target;
    AutomationTimebase timebase = AutomationTimebase::seconds;
    AutomationInterpolation interpolation =
        AutomationInterpolation::linear;
    std::vector<AutomationPoint> points;
    double trimOffset = 0.0;
    bool enabled = true;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<AutomationLane> fromVar(const juce::var& value,
                                                 juce::String& error);
};

juce::String automationModeToString(AutomationMode value);
std::optional<AutomationMode> automationModeFromString(
    const juce::String& value);
juce::String automationTimebaseToString(AutomationTimebase value);
std::optional<AutomationTimebase> automationTimebaseFromString(
    const juce::String& value);
juce::String automationInterpolationToString(
    AutomationInterpolation value);
std::optional<AutomationInterpolation> automationInterpolationFromString(
    const juce::String& value);
juce::String automationTargetTypeToString(AutomationTargetType value);
std::optional<AutomationTargetType> automationTargetTypeFromString(
    const juce::String& value);
}
