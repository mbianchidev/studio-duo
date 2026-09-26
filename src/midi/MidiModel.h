#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace studio
{
enum class MidiExpressionType
{
    pitchBend,
    pressure,
    channelPressure,
    timbre,
    controller
};

enum class MidiEditorMode
{
    pianoRoll,
    drums
};

enum class CymbalState
{
    none,
    edge,
    bow,
    bell,
    choke,
    open,
    closed,
    pedal
};

struct MidiExpressionPoint
{
    juce::String id { juce::Uuid().toString() };
    MidiExpressionType type = MidiExpressionType::pressure;
    double offsetBeats = 0.0;
    double value = 0.0;
    int controller = -1;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiExpressionPoint> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MidiNote
{
    juce::String id { juce::Uuid().toString() };
    int pitch = 60;
    int channel = 1;
    double startBeats = 0.0;
    double timingOffsetBeats = 0.0;
    double durationBeats = 0.25;
    int velocity = 100;
    int releaseVelocity = 64;
    double probability = 1.0;
    juce::String drumMapEntryId;
    juce::String articulation;
    juce::String chokeGroup;
    CymbalState cymbalState = CymbalState::none;
    int footControlValue = -1;
    int roundRobinHint = -1;
    std::vector<MidiExpressionPoint> expressions;

    [[nodiscard]] double actualStartBeats() const noexcept;
    [[nodiscard]] double endBeats() const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiNote> fromVar(const juce::var& value,
                                           juce::String& error);
};

struct DrumPadBinding
{
    int noteNumber = 36;
    int keyCode = 'Z';

    bool operator==(const DrumPadBinding&) const = default;
};

inline constexpr std::size_t drumPadCount = 12;

struct MidiClip
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "MIDI clip" };
    double startBeats = 0.0;
    double durationBeats = 4.0;
    MidiEditorMode editorMode = MidiEditorMode::pianoRoll;
    juce::String drumMapId;
    std::uint64_t humanizeSeed = 0;
    int humanizeTimingTicks = 0;
    int humanizeVelocity = 0;
    bool muted = false;
    std::vector<DrumPadBinding> drumPadBindings;
    std::vector<MidiNote> notes;

    [[nodiscard]] double endBeats() const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiClip> fromVar(const juce::var& value,
                                           juce::String& error);
};

struct DrumMapEntry
{
    juce::String id { juce::Uuid().toString() };
    int noteNumber = 36;
    juce::String name { "Kick" };
    juce::String articulation { "center" };
    juce::String chokeGroup;
    CymbalState cymbalState = CymbalState::none;
    int footControlCC = -1;
    std::vector<int> roundRobinNotes;
    juce::String outputGroup { "Main" };

    [[nodiscard]] juce::var toVar() const;
    static std::optional<DrumMapEntry> fromVar(const juce::var& value,
                                               juce::String& error);
};

struct DrumMap
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Drum map" };
    juce::String source;
    std::vector<DrumMapEntry> entries;

    [[nodiscard]] const DrumMapEntry* entryForPitch(int pitch) const noexcept;
    [[nodiscard]] const DrumMapEntry* entryForId(
        const juce::String& entryId) const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<DrumMap> fromVar(const juce::var& value,
                                          juce::String& error);
};

struct MidiPatternEvent
{
    juce::String id { juce::Uuid().toString() };
    juce::String drumMapEntryId;
    int pitch = 36;
    double offsetBeats = 0.0;
    double durationBeats = 0.125;
    int velocity = 100;
    double probability = 1.0;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiPatternEvent> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MidiPatternAlias
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Pattern" };
    double lengthBeats = 4.0;
    std::vector<MidiPatternEvent> events;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiPatternAlias> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MidiRoutingTemplateOutput
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Output" };
    juce::String destinationTrackId;
    int midiChannel = 1;
    std::vector<int> pitches;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiRoutingTemplateOutput> fromVar(
        const juce::var& value,
        juce::String& error);
};

struct MidiRoutingTemplate
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "MIDI routing" };
    std::vector<MidiRoutingTemplateOutput> outputs;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<MidiRoutingTemplate> fromVar(
        const juce::var& value,
        juce::String& error);
};

[[nodiscard]] juce::String midiExpressionTypeToString(
    MidiExpressionType type);
[[nodiscard]] std::optional<MidiExpressionType> midiExpressionTypeFromString(
    const juce::String& value);
[[nodiscard]] juce::String midiEditorModeToString(MidiEditorMode mode);
[[nodiscard]] std::optional<MidiEditorMode> midiEditorModeFromString(
    const juce::String& value);
[[nodiscard]] juce::String cymbalStateToString(CymbalState state);
[[nodiscard]] std::optional<CymbalState> cymbalStateFromString(
    const juce::String& value);

[[nodiscard]] DrumMap createDefaultMetalDrumMap();
[[nodiscard]] bool isDrumPadKey(int keyCode) noexcept;
[[nodiscard]] std::vector<DrumPadBinding> defaultDrumPadBindings(
    const DrumMap* map = nullptr);
[[nodiscard]] std::vector<MidiPatternAlias> createDefaultMetalPatterns(
    const DrumMap& map);
[[nodiscard]] MidiRoutingTemplate createDefaultMetalRoutingTemplate(
    const DrumMap& map);
}
