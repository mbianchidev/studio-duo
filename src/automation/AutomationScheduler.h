#pragma once

#include "AutomationTypes.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace studio
{
class Project;

struct CompiledAutomationPoint
{
    std::int64_t sample = 0;
    double value = 0.0;
};

struct CompiledAutomationLane
{
    AutomationTarget target;
    AutomationInterpolation interpolation =
        AutomationInterpolation::linear;
    std::vector<CompiledAutomationPoint> points;
    double trimOffset = 0.0;

    [[nodiscard]] double valueAt(std::int64_t sample) const noexcept;
};

struct CompiledAutomationEvent
{
    int sampleOffset = 0;
    double value = 0.0;
    int rampEndOffset = 0;
    double rampEndValue = 0.0;
    bool ramp = false;
};

class AutomationScheduler
{
public:
    static std::optional<CompiledAutomationLane> compile(
        const Project& project,
        const AutomationLane& lane,
        double sampleRate,
        juce::String& error);
    static std::vector<CompiledAutomationEvent> eventsForBlock(
        const CompiledAutomationLane& lane,
        std::int64_t blockStartSample,
        int blockSamples);
};
}
