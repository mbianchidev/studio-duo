#pragma once

#include "AutomationTypes.h"

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
};
}
