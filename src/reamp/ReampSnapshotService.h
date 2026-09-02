#pragma once

#include "model/ProjectModel.h"

namespace studio
{
class ReampSnapshotService
{
public:
    static std::optional<ToneSnapshot> capture(
        const Project& project,
        const juce::String& routeId,
        juce::String name,
        juce::String& error);
    static juce::String staleReason(const Project& project,
                                    const ToneSnapshot& snapshot);
    static juce::String sourceFingerprint(const Project& project,
                                          const juce::String& trackId);
    static juce::String chainFingerprint(const Project& project,
                                         const juce::String& routeId);
};

class MixerSnapshotService
{
public:
    static std::optional<MixerSnapshot> capture(
        const Project& project,
        const std::vector<juce::String>& trackIds,
        juce::String name,
        juce::String& error);
};
}
