#pragma once

#include "AutomationTypes.h"

#include <optional>

namespace studio
{
struct AutomationGesture
{
    double startPosition = 0.0;
    double endPosition = 0.0;
    double startValue = 0.0;
    double endValue = 0.0;
};

class AutomationRecorder
{
public:
    static AutomationLane applyGesture(const AutomationLane& lane,
                                       AutomationMode mode,
                                       AutomationGesture gesture);
    static std::optional<AutomationLane> writeGesture(
        const AutomationLane* lane,
        AutomationTarget target,
        juce::String name,
        AutomationMode mode,
        AutomationGesture gesture);
};
}
