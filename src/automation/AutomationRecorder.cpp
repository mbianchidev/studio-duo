#include "AutomationRecorder.h"

#include "model/ProjectCommands.h"

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
double valueAt(const AutomationLane& lane, double position)
{
    if (lane.points.empty())
        return 0.0;
    if (position <= lane.points.front().position)
        return lane.points.front().value;
    if (position >= lane.points.back().position)
        return lane.points.back().value;
    const auto upper = std::upper_bound(
        lane.points.cbegin(),
        lane.points.cend(),
        position,
        [](double value, const auto& point)
        {
            return value < point.position;
        });
    const auto& right = *upper;
    const auto& left = *std::prev(upper);
    if (lane.interpolation == AutomationInterpolation::step
        || right.position <= left.position)
        return left.value;
    const auto progress = (position - left.position)
        / (right.position - left.position);
    return left.value + (right.value - left.value) * progress;
}

void addPoint(AutomationLane& lane, double position, double value)
{
    lane.points.push_back({
        juce::Uuid().toString(),
        std::max(0.0, position),
        value
    });
}

void normalize(AutomationLane& lane)
{
    std::stable_sort(
        lane.points.begin(),
        lane.points.end(),
        [](const auto& left, const auto& right)
        {
            return left.position < right.position;
        });
    std::vector<AutomationPoint> unique;
    for (const auto& point : lane.points)
    {
        if (!unique.empty()
            && std::abs(unique.back().position - point.position)
                < 0.000000001)
            unique.back() = point;
        else
            unique.push_back(point);
    }
    lane.points = std::move(unique);
}
}

AutomationLane AutomationRecorder::applyGesture(
    const AutomationLane& lane,
    AutomationMode mode,
    AutomationGesture gesture)
{
    if (mode == AutomationMode::read
        || mode == AutomationMode::preview)
        return lane;

    auto result = lane;
    if (gesture.endPosition < gesture.startPosition)
    {
        std::swap(gesture.startPosition, gesture.endPosition);
        std::swap(gesture.startValue, gesture.endValue);
    }
    if (mode == AutomationMode::trim)
    {
        result.trimOffset += gesture.endValue - gesture.startValue;
        return result;
    }

    const auto returnValue = valueAt(lane, gesture.endPosition);
    const auto resumePoint = std::find_if(
        lane.points.cbegin(),
        lane.points.cend(),
        [&gesture](const auto& point)
        {
            return point.position > gesture.endPosition;
        });
    result.points.erase(
        std::remove_if(
            result.points.begin(),
            result.points.end(),
            [&gesture](const auto& point)
            {
                return point.position >= gesture.startPosition
                    && point.position <= gesture.endPosition;
            }),
        result.points.end());
    addPoint(result, gesture.startPosition, gesture.startValue);
    addPoint(result, gesture.endPosition, gesture.endValue);
    if (mode == AutomationMode::touch)
    {
        addPoint(result,
                 gesture.endPosition + 0.000001,
                 returnValue);
    }
    else if (mode == AutomationMode::latch
             && resumePoint != lane.points.cend())
    {
        addPoint(
            result,
            std::max(
                gesture.endPosition,
                resumePoint->position - 0.000001),
            gesture.endValue);
    }
    normalize(result);
    return result;
}

std::optional<AutomationLane> AutomationRecorder::writeGesture(
    const AutomationLane* lane,
    AutomationTarget target,
    juce::String name,
    AutomationMode mode,
    AutomationGesture gesture)
{
    if (mode == AutomationMode::read
        || mode == AutomationMode::preview)
        return std::nullopt;

    if (lane != nullptr)
        return applyGesture(*lane, mode, gesture);

    AutomationLane created;
    created.name = std::move(name);
    created.target = std::move(target);
    created.points.push_back({
        juce::Uuid().toString(),
        std::max(0.0, gesture.startPosition),
        gesture.startValue
    });
    return applyGesture(created, mode, gesture);
}

juce::Result AutomationPreview::stage(std::vector<AutomationPreviewTarget> targetsToStage)
{
    return stageTargets(std::move(targetsToStage), false);
}

juce::Result AutomationPreview::stageReset(std::vector<AutomationPreviewTarget> targetsToStage)
{
    return stageTargets(std::move(targetsToStage), true);
}

juce::Result AutomationPreview::stageTargets(
    std::vector<AutomationPreviewTarget> targetsToStage, bool changedOnly)
{
    if (targetsToStage.empty())
        return juce::Result::ok();
    for (auto iterator = targetsToStage.begin(); iterator != targetsToStage.end(); ++iterator)
    {
        if (!std::isfinite(iterator->startSeconds) || iterator->startSeconds < 0.0
            || !std::isfinite(iterator->endSeconds) || iterator->endSeconds < 0.0
            || !std::isfinite(iterator->startValue) || !std::isfinite(iterator->endValue))
            return juce::Result::fail("The automation preview contains an invalid gesture.");
        if (std::any_of(targetsToStage.begin(), iterator, [&iterator](const auto& candidate)
            { return candidate.target == iterator->target; }))
            return juce::Result::fail("The automation preview contains a duplicate target.");
        iterator->startValue = juce::jlimit(0.0, 1.0, iterator->startValue);
        iterator->endValue = juce::jlimit(0.0, 1.0, iterator->endValue);
    }
    if (changedOnly)
        targetsToStage.erase(
            std::remove_if(targetsToStage.begin(), targetsToStage.end(), [this](const auto& target)
                {
                    // Live parameters are reverted in Preview; reset also replaces pending offsets.
                    return std::abs(target.startValue - target.endValue) < 0.000001
                        && std::none_of(pendingTargets.cbegin(), pendingTargets.cend(),
                            [&target](const auto& pending)
                            {
                                return pending.target == target.target
                                    && std::abs(pending.endValue - target.endValue) >= 0.000001;
                            });
                }),
            targetsToStage.end());
    if (targetsToStage.empty())
        return juce::Result::ok();
    pendingTargets = std::move(targetsToStage);
    return juce::Result::ok();
}

juce::Result AutomationPreview::commit(Project& project, CommandStack& history)
{
    if (empty())
        return juce::Result::fail("No automation preview is ready.");
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& preview : pendingTargets)
    {
        const auto lane = std::find_if(
            project.automationLanes.cbegin(), project.automationLanes.cend(),
            [&preview](const auto& candidate) { return candidate.target == preview.target; });
        const auto timebase = lane != project.automationLanes.cend()
            ? lane->timebase : AutomationTimebase::seconds;
        const auto positionFor = [&project, timebase](double seconds)
        {
            return timebase == AutomationTimebase::beats ? project.beatsAt(seconds) : seconds;
        };
        const auto written = AutomationRecorder::writeGesture(
            lane != project.automationLanes.cend() ? &*lane : nullptr,
            preview.target, preview.laneName, AutomationMode::write,
            { positionFor(preview.startSeconds), positionFor(preview.endSeconds),
              preview.startValue, preview.endValue });
        if (!written.has_value())
            return juce::Result::fail("The automation preview could not be prepared.");
        if (lane == project.automationLanes.cend())
            commands.push_back(std::make_unique<AddAutomationLaneCommand>(*written));
        else
            commands.push_back(std::make_unique<SetAutomationLaneCommand>(*lane, *written));
    }
    auto command = commands.size() == 1
        ? std::move(commands.front())
        : std::make_unique<BatchProjectCommand>("Commit automation preview", std::move(commands));
    juce::String error;
    if (!history.perform(std::move(command), project, error))
        return juce::Result::fail(error);
    reset();
    return juce::Result::ok();
}

void AutomationPreview::reset()
{
    pendingTargets.clear();
}

bool AutomationPreview::empty() const noexcept
{
    return pendingTargets.empty();
}

const std::vector<AutomationPreviewTarget>& AutomationPreview::targets() const noexcept
{
    return pendingTargets;
}
}
