#pragma once

#include "AutomationTypes.h"

#include <optional>

namespace studio
{
class CommandStack;
class Project;

struct AutomationGesture
{
    double startPosition = 0.0;
    double endPosition = 0.0;
    double startValue = 0.0;
    double endValue = 0.0;
};

struct AutomationPreviewTarget
{
    AutomationTarget target;
    juce::String laneName;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double startValue = 0.0;
    double endValue = 0.0;
};

class AutomationPreview
{
public:
    juce::Result stage(std::vector<AutomationPreviewTarget> targets);
    juce::Result stageReset(std::vector<AutomationPreviewTarget> targets);
    juce::Result commit(Project& project, CommandStack& history);
    void reset();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const std::vector<AutomationPreviewTarget>& targets() const noexcept;

private:
    juce::Result stageTargets(std::vector<AutomationPreviewTarget> targets, bool changedOnly);
    std::vector<AutomationPreviewTarget> pendingTargets;
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
