#include "AutomationRecorder.h"

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
                 gesture.endPosition + 0.000000001,
                 returnValue);
    }
    normalize(result);
    return result;
}
}
