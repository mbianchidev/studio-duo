#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <concepts>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>

namespace studio
{
template <std::floating_point Number>
juce::String formatRoundTripNumber(Number value)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<Number>::max_digits10) << value;
    return juce::String(output.str());
}

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
