#include "AutomationScheduler.h"

#include "model/ProjectModel.h"

#include <algorithm>
#include <cmath>

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
    for (const auto& point : lane.points)
    {
        const auto seconds = lane.timebase == AutomationTimebase::beats
            ? project.secondsAtBeat(point.position)
            : point.position;
        compiled.points.push_back({
            static_cast<std::int64_t>(
                std::llround(seconds * sampleRate)),
            point.value
        });
    }
    std::stable_sort(
        compiled.points.begin(),
        compiled.points.end(),
        [](const auto& left, const auto& right)
        {
            return left.sample < right.sample;
        });
    std::vector<CompiledAutomationPoint> unique;
    for (const auto& point : compiled.points)
    {
        if (!unique.empty() && unique.back().sample == point.sample)
            unique.back() = point;
        else
            unique.push_back(point);
    }
    compiled.points = std::move(unique);
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
        events.reserve(static_cast<std::size_t>(blockSamples));
        for (int sample = 0; sample < blockSamples; ++sample)
            events.push_back({
                sample,
                lane.valueAt(blockStartSample + sample)
            });
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
