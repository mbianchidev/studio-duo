#pragma once

#include "ProjectModel.h"

namespace studio
{
struct LinkedClipSelection
{
    std::vector<juce::String> clipIds;
    std::size_t expectedClipCount = 0;

    [[nodiscard]] bool isLinked() const noexcept
    {
        return expectedClipCount > 1;
    }

    [[nodiscard]] bool isComplete() const noexcept
    {
        return expectedClipCount > 0
            && clipIds.size() == expectedClipCount;
    }
};

[[nodiscard]] const AudioClip* activeClipAt(
    const Project& project,
    const juce::String& parentTrackId,
    double seconds);

[[nodiscard]] LinkedClipSelection linkedClipsAt(
    const Project& project,
    const juce::String& clipId,
    double seconds);

[[nodiscard]] double clampLinkedMoveDelta(
    const Project& project,
    const LinkedClipSelection& selection,
    double requestedDelta);
}
