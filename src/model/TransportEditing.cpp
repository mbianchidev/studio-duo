#include "TransportEditing.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace studio
{
juce::Result TransportEditing::validateLoopRange(
    const LoopRangeSettings& settings, double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return juce::Result::fail("A finite positive playback sample rate is required.");
    if (!std::isfinite(settings.startSeconds) || !std::isfinite(settings.endSeconds)
        || settings.startSeconds < 0.0 || settings.endSeconds <= settings.startSeconds)
        return juce::Result::fail("Loop end must be after a finite, non-negative start.");
    const auto start = settings.startSeconds * sampleRate;
    const auto end = settings.endSeconds * sampleRate;
    const auto maximum = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()), 0.0);
    if (!std::isfinite(start) || !std::isfinite(end) || end > maximum)
        return juce::Result::fail("Loop boundaries exceed the supported audio sample clock.");
    if (std::llround(end) <= std::llround(start))
        return juce::Result::fail("The loop must contain at least one sample at the playback sample rate.");
    return juce::Result::ok();
}

juce::String TransportEditing::musicalPositionText(const Project& project, double seconds)
{
    juce::String error;
    if (!std::isfinite(seconds) || seconds < 0.0 || !project.validateTransport(error))
        return {};
    const auto beats = project.beatsAt(seconds);
    if (!std::isfinite(beats)
        || beats > static_cast<double>(std::numeric_limits<int>::max() - 2) / 8.0)
        return {};
    const auto position = project.musicalPositionAt(seconds);
    return juce::String(position.bar) + ":" + juce::String(position.beat) + ":"
        + juce::String(position.ticks).paddedLeft('0', 3);
}

std::optional<double> TransportEditing::secondsAtMusicalPosition(
    const Project& project, const juce::String& position, juce::String& error)
{
    error.clear();
    const auto text = position.trim();
    if (text.isEmpty() || text.length() > 64)
    {
        error = "Enter bar, bar:beat, or bar:beat:tick.";
        return std::nullopt;
    }
    std::array<int, 3> fields { 1, 1, 0 };
    auto start = 0;
    auto field = std::size_t { 0 };
    for (;;)
    {
        const auto separator = text.indexOfChar(start, ':');
        const auto token = (separator < 0 ? text.substring(start)
                                         : text.substring(start, separator)).trim().toStdString();
        if (field >= fields.size())
        {
            error = "Musical positions have at most three fields: bar:beat:tick.";
            return std::nullopt;
        }
        const auto parsed = std::from_chars(
            token.data(), token.data() + token.size(), fields[field]);
        if (parsed.ec != std::errc()
            || parsed.ptr != token.data() + token.size())
        {
            error = "Bar, beat and tick must be whole numbers, without units or other text.";
            return std::nullopt;
        }
        ++field;
        if (separator < 0)
            break;
        start = separator + 1;
    }
    if (fields[0] < 1 || fields[0] > std::numeric_limits<int>::max() - 2
        || fields[1] < 1 || fields[1] > 32 || fields[2] < 0 || fields[2] >= 960)
    {
        error = "Bars and beats start at 1; ticks must be between 0 and 959.";
        return std::nullopt;
    }
    if (!project.validateTransport(error))
        return std::nullopt;

    auto current = MeterChange { 0.0, project.timeSignatureNumerator, project.timeSignatureDenominator };
    auto completedBars = 0.0;
    std::optional<double> segmentEnd;
    const auto requestedBar = static_cast<double>(fields[0] - 1);
    for (const auto& change : project.meterChanges)
    {
        if (change.timeSeconds <= current.timeSeconds + 0.0000001)
        {
            current = change;
            continue;
        }
        const auto metricBeats = (project.beatsAt(change.timeSeconds)
                                  - project.beatsAt(current.timeSeconds))
            * static_cast<double>(current.denominator) / 4.0;
        const auto bars = std::ceil(metricBeats / static_cast<double>(current.numerator) - 0.0000001);
        if (requestedBar < completedBars + bars)
        {
            segmentEnd = change.timeSeconds;
            break;
        }
        completedBars += bars;
        current = change;
    }
    if (fields[1] > current.numerator)
    {
        error = "The selected beat is outside the time signature of this bar.";
        return std::nullopt;
    }
    const auto metricOffset = (requestedBar - completedBars) * current.numerator
        + static_cast<double>(fields[1] - 1) + static_cast<double>(fields[2]) / 960.0;
    const auto targetBeats = project.beatsAt(current.timeSeconds)
        + metricOffset * 4.0 / static_cast<double>(current.denominator);
    const auto seconds = project.secondsAtBeat(targetBeats);
    if (!std::isfinite(seconds) || seconds < current.timeSeconds - 1.0e-9
        || (segmentEnd.has_value() && seconds >= *segmentEnd - 1.0e-9))
    {
        error = "That musical position is replaced by a time-signature change.";
        return std::nullopt;
    }
    const auto resolved = project.musicalPositionAt(seconds);
    if (resolved.bar != fields[0] || resolved.beat != fields[1] || resolved.ticks != fields[2])
    {
        error = "That musical position cannot be represented by the current tempo and meter maps.";
        return std::nullopt;
    }
    return seconds;
}

std::optional<juce::Range<double>> TransportEditing::markerRange(
    const Project& project, const juce::String& startMarkerId,
    const juce::String& endMarkerId, juce::String& error)
{
    error.clear();
    const auto* start = project.findMarker(startMarkerId);
    const auto* end = project.findMarker(endMarkerId);
    if (start == nullptr || end == nullptr || startMarkerId.isEmpty() || endMarkerId.isEmpty())
    {
        error = "Choose existing start and end markers. A selected marker is missing.";
        return std::nullopt;
    }
    if (!std::isfinite(start->timeSeconds) || !std::isfinite(end->timeSeconds)
        || start->timeSeconds < 0.0 || end->timeSeconds <= start->timeSeconds)
    {
        error = "The end marker must be after a finite, non-negative start marker.";
        return std::nullopt;
    }
    return juce::Range<double>(start->timeSeconds, end->timeSeconds);
}
}
