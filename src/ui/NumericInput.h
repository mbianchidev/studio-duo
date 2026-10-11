#pragma once

#include "util/NumberParsing.h"

#include <cmath>
#include <optional>

namespace studio
{
inline std::optional<double> parseDecibels(
    const juce::String& text)
{
    auto normalized = text.trim();
    if (normalized.endsWithIgnoreCase("db"))
        normalized = normalized.dropLastCharacters(2).trim();
    return parseFiniteNumber(normalized);
}

inline std::optional<float> parseTrackDecibels(
    const juce::String& text)
{
    const auto parsed = parseDecibels(text);
    if (!parsed.has_value()
        || *parsed < -60.0
        || *parsed > 12.0)
        return std::nullopt;
    return static_cast<float>(
        std::round(*parsed * 10.0) / 10.0);
}

inline juce::String formatDecibels(double value)
{
    if (std::abs(value) < 0.05)
        value = 0.0;
    return juce::String(value > 0.0 ? "+" : "")
        + juce::String(value, 1)
        + " dB";
}
}
