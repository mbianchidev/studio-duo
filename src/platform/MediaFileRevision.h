#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace studio
{
struct MediaFileRevision
{
    juce::String key;
    bool reusable = false;

    bool operator==(const MediaFileRevision&) const = default;
};

// Native identities stay in memory; they are not diagnostic/export metadata.
std::optional<MediaFileRevision> readMediaFileRevision(
    const juce::File& file, juce::String& error);
}
