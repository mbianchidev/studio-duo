#pragma once

#include "ProjectModel.h"

namespace studio
{
struct LoopRangeSettings
{
    bool enabled = false;
    double startSeconds = 0.0;
    double endSeconds = 8.0;
};

class TransportEditing
{
public:
    static juce::Result validateLoopRange(
        const LoopRangeSettings& settings,
        double sampleRate);
    static juce::String musicalPositionText(
        const Project& project,
        double seconds);
    static std::optional<double> secondsAtMusicalPosition(
        const Project& project,
        const juce::String& position,
        juce::String& error);
    static std::optional<juce::Range<double>> markerRange(
        const Project& project,
        const juce::String& startMarkerId,
        const juce::String& endMarkerId,
        juce::String& error);
};
}
