#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <locale>
#include <optional>
#include <sstream>

namespace studio
{
inline std::optional<double> parseFiniteNumber(const juce::String& text)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return std::nullopt;

    std::istringstream input(trimmed.toStdString());
    input.imbue(std::locale::classic());
    double value = 0.0;
    input >> std::noskipws >> value;
    if (input.fail() || !input.eof() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

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
}
