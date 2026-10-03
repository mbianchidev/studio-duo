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
}
