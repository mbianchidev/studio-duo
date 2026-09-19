#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

namespace studio
{
struct MasteringSourceMix
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Main" };
    juce::File file;
    juce::String sourceHash;
    double durationSeconds = 0.0;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MasteringSourceMix> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MasteringTrack
{
    juce::String id { juce::Uuid().toString() };
    juce::String title { "Untitled track" };
    juce::String artist;
    juce::String songwriter;
    juce::String isrc;
    double gapBeforeSeconds = 0.0;
    double overlapPreviousSeconds = 0.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    double gainDecibels = 0.0;
    std::vector<double> indexMarkersSeconds;
    std::vector<MasteringSourceMix> sources;
    juce::String selectedSourceId;

    [[nodiscard]] const MasteringSourceMix* selectedSource() const noexcept;
    [[nodiscard]] MasteringSourceMix* selectedSource() noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<MasteringTrack> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MasteringReference
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Reference" };
    juce::File file;
    juce::String sourceHash;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MasteringReference> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MasteringTrackPlacement
{
    juce::String trackId;
    juce::String sourceId;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;

    [[nodiscard]] double endSeconds() const noexcept;
};

struct MasteringAlbum
{
    juce::String title;
    juce::String artist;
    juce::String songwriter;
    juce::String label;
    juce::String catalogNumber;
    juce::String mcn;
    juce::String releaseDate;
    juce::String genre;
    juce::String copyright;
    double outputGainDecibels = 0.0;
    std::vector<MasteringTrack> tracks;
    std::vector<MasteringReference> references;

    [[nodiscard]] std::vector<MasteringTrackPlacement> placements() const;
    [[nodiscard]] double durationSeconds() const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<MasteringAlbum> fromVar(
        const juce::var& value,
        juce::String& error);
};
}
