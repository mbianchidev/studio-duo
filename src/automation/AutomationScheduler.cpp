#include "AutomationScheduler.h"

#include "model/ProjectModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace studio
{
double CompiledAutomationLane::valueAt(std::int64_t sample) const noexcept
{
    if (points.empty())
        return trimOffset;
    if (sample <= points.front().sample)
        return points.front().value + trimOffset;
    if (sample >= points.back().sample)
        return points.back().value + trimOffset;

    const auto upper = std::upper_bound(
        points.cbegin(),
        points.cend(),
        sample,
        [](std::int64_t position, const auto& point)
        {
            return position < point.sample;
        });
    const auto& right = *upper;
    const auto& left = *std::prev(upper);
    if (interpolation == AutomationInterpolation::step
        || right.sample <= left.sample)
        return left.value + trimOffset;
    const auto progress =
        static_cast<double>(sample - left.sample)
        / static_cast<double>(right.sample - left.sample);
    return left.value
        + (right.value - left.value) * progress
        + trimOffset;
}

std::optional<CompiledAutomationLane> AutomationScheduler::compile(
    const Project& project,
    const AutomationLane& lane,
    double sampleRate,
    juce::String& error)
{
    if (sampleRate <= 0.0 || !lane.enabled)
    {
        error = "Automation requires an enabled lane and positive sample rate.";
        return std::nullopt;
    }
    if (project.findTrack(lane.target.trackId) == nullptr)
    {
        error = "Automation target track is unavailable.";
        return std::nullopt;
    }
    if ((lane.target.type == AutomationTargetType::sendGain
         || lane.target.type == AutomationTargetType::sendPan
         || lane.target.type == AutomationTargetType::sendMute)
        && project.findRoutingConnection(lane.target.routeId) == nullptr)
    {
        error = "Automation target route is unavailable.";
        return std::nullopt;
    }
    if ((lane.target.type == AutomationTargetType::pluginParameter
         || lane.target.type == AutomationTargetType::deviceParameter)
        && (lane.target.insertId.isEmpty()
            || lane.target.parameterIndex < 0))
    {
        error = "Plugin automation requires an insert and parameter index.";
        return std::nullopt;
    }

    CompiledAutomationLane compiled;
    compiled.target = lane.target;
    compiled.interpolation = lane.interpolation;
    compiled.trimOffset = lane.trimOffset;
    struct TimedPoint
    {
        double seconds = 0.0;
        double value = 0.0;
    };
    std::vector<TimedPoint> timedPoints;
    timedPoints.reserve(lane.points.size());
    for (const auto& point : lane.points)
    {
        const auto seconds = lane.timebase == AutomationTimebase::beats
            ? project.secondsAtBeat(point.position)
            : point.position;
        timedPoints.push_back({ seconds, point.value });
    }
    std::stable_sort(
        timedPoints.begin(),
        timedPoints.end(),
        [](const auto& left, const auto& right)
        {
            return left.seconds < right.seconds;
        });
    auto previousSeconds = -1.0;
    for (const auto& point : timedPoints)
    {
        auto sample = static_cast<std::int64_t>(
            std::llround(point.seconds * sampleRate));
        const auto samePosition = std::abs(
            point.seconds - previousSeconds)
            <= std::numeric_limits<double>::epsilon()
                * std::max({
                    1.0,
                    std::abs(point.seconds),
                    std::abs(previousSeconds)
                });
        if (!compiled.points.empty()
            && !samePosition
            && sample <= compiled.points.back().sample)
        {
            sample = compiled.points.back().sample + 1;
        }
        if (!compiled.points.empty() && samePosition)
        {
            compiled.points.back().value = point.value;
        }
        else
        {
            compiled.points.push_back({ sample, point.value });
        }
        previousSeconds = point.seconds;
    }
    return compiled;
}

std::vector<CompiledAutomationEvent> AutomationScheduler::eventsForBlock(
    const CompiledAutomationLane& lane,
    std::int64_t blockStartSample,
    int blockSamples)
{
    std::vector<CompiledAutomationEvent> events;
    if (blockSamples <= 0)
        return events;
    if (lane.interpolation == AutomationInterpolation::linear)
    {
        auto segmentStart = 0;
        const auto appendSegment = [&](int segmentEnd)
        {
            if (segmentEnd <= segmentStart)
                return;
            events.push_back({
                segmentStart,
                lane.valueAt(blockStartSample + segmentStart),
                segmentEnd,
                lane.valueAt(blockStartSample + segmentEnd),
                true
            });
            segmentStart = segmentEnd;
        };
        const auto blockEnd = blockStartSample + blockSamples;
        for (const auto& point : lane.points)
        {
            if (point.sample <= blockStartSample
                || point.sample >= blockEnd)
                continue;
            appendSegment(static_cast<int>(
                point.sample - blockStartSample));
        }
        appendSegment(blockSamples);
        return events;
    }

    events.push_back({ 0, lane.valueAt(blockStartSample) });
    const auto blockEnd = blockStartSample + blockSamples;
    for (const auto& point : lane.points)
    {
        if (point.sample <= blockStartSample || point.sample >= blockEnd)
            continue;
        events.push_back({
            static_cast<int>(point.sample - blockStartSample),
            point.value + lane.trimOffset
        });
    }
    return events;
}
}
