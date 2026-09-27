#pragma once

#include "MidiCaptureBuffer.h"
#include "MidiModel.h"
#include "model/ProjectModel.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace studio
{
enum class MidiEditorLane
{
    velocity,
    timing,
    duration,
    probability,
    expression
};

enum class MidiEntryTool
{
    flam,
    roll,
    gravityBlast,
    blastBeat,
    doubleKick
};

struct MidiEntryRequest
{
    double startBeats = 0.0;
    double lengthBeats = 4.0;
    double stepBeats = 0.25;
    double noteDurationBeats = 0.125;
    double flamSpacingBeats = 1.0 / 32.0;
    int pitch = 38;
    int velocity = 108;
};

struct MidiHumanizeSettings
{
    std::uint64_t seed = 1;
    int maximumTimingTicks = 12;
    int maximumVelocityChange = 8;
};

struct MidiCaptureConversion
{
    MidiClip clip;
    std::uint64_t ignoredEvents = 0;
    std::uint64_t unmatchedNoteOffs = 0;
};

[[nodiscard]] std::uint64_t deterministicMidiValue(
    std::uint64_t seed,
    const juce::String& stableId,
    std::uint64_t stream = 0) noexcept;
[[nodiscard]] bool midiNoteShouldPlay(const MidiClip& clip,
                                      const MidiNote& note) noexcept;

[[nodiscard]] MidiClip humanizeMidiClip(
    const MidiClip& source,
    const MidiHumanizeSettings& settings);
[[nodiscard]] std::vector<MidiNote> expandMidiPattern(
    const MidiPatternAlias& pattern,
    const DrumMap* drumMap,
    double startBeats,
    int repetitions);
[[nodiscard]] std::vector<MidiNote> generateMidiEntryTool(
    MidiEntryTool tool,
    const MidiEntryRequest& request,
    const DrumMap* drumMap);

[[nodiscard]] MidiNote createMidiNote(
    const MidiClip& clip,
    double startBeats,
    int pitch,
    double durationBeats,
    int velocity,
    const DrumMap* drumMap);
void applyDrumMapMetadata(MidiNote& note,
                          const DrumMap* drumMap,
                          std::optional<std::size_t> roundRobinIndex = std::nullopt);
[[nodiscard]] bool moveMidiNotes(
    MidiClip& clip,
    const std::vector<juce::String>& noteIds,
    double deltaBeats,
    int deltaPitch,
    const DrumMap* drumMap = nullptr);
[[nodiscard]] bool resizeMidiNotes(
    MidiClip& clip,
    const std::vector<juce::String>& noteIds,
    double deltaBeats,
    double minimumDurationBeats);
[[nodiscard]] bool deleteMidiNotes(
    MidiClip& clip,
    const std::vector<juce::String>& noteIds);
[[nodiscard]] bool setMidiLaneValue(
    MidiClip& clip,
    const std::vector<juce::String>& noteIds,
    MidiEditorLane lane,
    double normalizedValue,
    double expressionBeat,
    MidiExpressionType expressionType);

void regenerateMidiClipIds(MidiClip& clip);
void sortMidiNotes(MidiClip& clip);

[[nodiscard]] std::optional<MidiCaptureConversion>
convertCapturedMidiToClip(
    const Project& project,
    const CapturedMidiWindow& captured,
    std::int64_t captureStartStreamSample,
    double timelineStartSeconds,
    double captureDurationSeconds,
    double sampleRate,
    const DrumMap* drumMap,
    juce::String clipName,
    juce::String& error,
    const juce::String& targetTrackId = {});
}
