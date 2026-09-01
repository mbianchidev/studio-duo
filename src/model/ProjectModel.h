#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include <optional>
#include <vector>

namespace studio
{
enum class TrackType
{
    audio,
    instrument,
    aux,
    bus,
    master
};

enum class PluginBridgeMode
{
    sandboxed,
    araCompatibility,
    trustedInProcess
};

enum class StretchMode
{
    drums,
    monophonic,
    polyphonic,
    mix
};

struct WarpMarker
{
    double timelineOffsetSeconds = 0.0;
    double sourceSeconds = 0.0;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<WarpMarker> fromVar(const juce::var& value,
                                             juce::String& error);
};

struct TempoChange
{
    double timeSeconds = 0.0;
    double bpm = 120.0;
    bool rampToNext = false;

    bool operator==(const TempoChange& other) const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<TempoChange> fromVar(const juce::var& value, juce::String& error);
};

struct MeterChange
{
    double timeSeconds = 0.0;
    int numerator = 4;
    int denominator = 4;

    bool operator==(const MeterChange& other) const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<MeterChange> fromVar(const juce::var& value, juce::String& error);
};

struct RecordingPlan
{
    double transportStartSeconds = 0.0;
    double captureStartSeconds = 0.0;
    double captureEndSeconds = -1.0;
    double transportEndSeconds = -1.0;
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
};

struct MusicalPosition
{
    int bar = 1;
    int beat = 1;
    int ticks = 0;
    MeterChange meter;
};

struct RecordingPass
{
    double timelineStartSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
    double durationSeconds = 0.0;
};

struct CompRegion
{
    juce::String id { juce::Uuid().toString() };
    juce::String sourceTrackId;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;

    [[nodiscard]] double endSeconds() const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<CompRegion> fromVar(const juce::var& value,
                                             juce::String& error);
};

struct EditGroup
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Edit group" };
    std::vector<juce::String> trackIds;
    juce::String timingReferenceTrackId;
    double quantizeStrength = 1.0;
    std::vector<double> protectedAnchorsSeconds;
    bool enabled = true;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<EditGroup> fromVar(const juce::var& value,
                                            juce::String& error);
};

enum class TonePathType
{
    hardware,
    plugin
};

struct ReampRoute
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Tone path" };
    TonePathType type = TonePathType::hardware;
    juce::String sourceTrackId;
    juce::String returnTrackId;
    int outputChannel = 2;
    int inputChannel = 0;
    int latencySamples = 0;
    int alignmentOffsetSamples = 0;
    bool polarityInverted = false;
    bool enabled = true;
    bool ownsReturnTrack = false;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<ReampRoute> fromVar(const juce::var& value,
                                             juce::String& error);
};

struct PluginInsert
{
    juce::String id { juce::Uuid().toString() };
    juce::String pluginIdentifier;
    juce::String name;
    juce::String manufacturer;
    juce::String format;
    juce::String version;
    juce::String fileOrIdentifier;
    juce::String stateFile;
    juce::String stateHash;
    PluginBridgeMode bridgeMode = PluginBridgeMode::sandboxed;
    int latencySamples = 0;
    double tailSeconds = 0.0;
    bool bypassed = false;
    bool missing = false;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<PluginInsert> fromVar(const juce::var& value, juce::String& error);
};

struct AudioClip
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Audio clip" };
    juce::File sourceFile;
    double startSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
    double sourceLengthSeconds = 4.0;
    double sourceRangeStartSeconds = 0.0;
    double sourceRangeEndSeconds = 0.0;
    double durationSeconds = 4.0;
    StretchMode stretchMode = StretchMode::polyphonic;
    double playbackRate = 1.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    bool polarityInverted = false;
    bool reversed = false;
    std::vector<WarpMarker> warpMarkers;
    std::vector<double> transientSourceSeconds;
    float gainDecibels = 0.0f;
    bool muted = false;
    juce::Colour colour { 0xffdd5b3f };

    [[nodiscard]] double endSeconds() const noexcept;
    [[nodiscard]] double sourceRangeEnd() const noexcept;
    [[nodiscard]] double recoverableStartSeconds() const noexcept;
    [[nodiscard]] double recoverableEndSeconds() const noexcept;
    [[nodiscard]] double sourceSecondsAt(double timelineOffsetSeconds) const noexcept;
    [[nodiscard]] double timelineOffsetForSourceSeconds(double sourceSeconds) const noexcept;
    [[nodiscard]] float envelopeGainAt(double timelineOffsetSeconds) const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<AudioClip> fromVar(const juce::var& value, juce::String& error);
};

struct Track
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Audio" };
    juce::String parentTrackId;
    int versionNumber = 0;
    bool versionsCollapsed = false;
    juce::String activeTakeTrackId;
    std::vector<CompRegion> compRegions;
    TrackType type = TrackType::audio;
    float volumeDecibels = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool solo = false;
    bool armed = false;
    int inputChannel = 0;
    bool stereoInput = false;
    bool inputMonitoring = false;
    juce::Colour colour { 0xffdd5b3f };
    std::vector<PluginInsert> inserts;
    std::vector<AudioClip> clips;

    [[nodiscard]] juce::var toVar() const;
    static std::optional<Track> fromVar(const juce::var& value, juce::String& error);
};

class Project
{
public:
    static constexpr int currentFormatVersion = 1;

    juce::String id { juce::Uuid().toString() };
    juce::String name { "Untitled" };
    double tempo = 120.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    std::vector<TempoChange> tempoChanges;
    std::vector<MeterChange> meterChanges;
    bool metronomeEnabled = true;
    int metronomeSubdivision = 1;
    int metronomeOutputChannel = 0;
    float metronomeLevel = 0.65f;
    float metronomeAccentLevel = 1.0f;
    bool punchEnabled = false;
    double punchInSeconds = 0.0;
    double punchOutSeconds = 8.0;
    int countInBars = 0;
    double preRollSeconds = 0.0;
    double postRollSeconds = 0.0;
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 8.0;
    std::vector<EditGroup> editGroups;
    std::vector<ReampRoute> reampRoutes;
    std::vector<Track> tracks;

    static Project createDefault();

    [[nodiscard]] Track* findTrack(const juce::String& trackId);
    [[nodiscard]] const Track* findTrack(const juce::String& trackId) const;
    [[nodiscard]] AudioClip* findClip(const juce::String& clipId);
    [[nodiscard]] const AudioClip* findClip(const juce::String& clipId) const;
    [[nodiscard]] Track* findTrackContainingClip(const juce::String& clipId);
    [[nodiscard]] const Track* findTrackContainingClip(const juce::String& clipId) const;
    [[nodiscard]] std::vector<juce::String> armedAudioParentTrackIds() const;
    [[nodiscard]] juce::String activeTakeTrackId(const juce::String& parentTrackId) const;
    [[nodiscard]] juce::String rootTrackId(const juce::String& trackId) const;
    [[nodiscard]] const EditGroup* editGroupForTrack(const juce::String& trackId) const;
    [[nodiscard]] const ReampRoute* reampRouteForReturn(
        const juce::String& trackId) const;
    [[nodiscard]] double tempoAt(double seconds) const noexcept;
    [[nodiscard]] MeterChange meterAt(double seconds) const noexcept;
    [[nodiscard]] double beatsAt(double seconds) const noexcept;
    [[nodiscard]] double secondsAtBeat(double beats) const noexcept;
    [[nodiscard]] MusicalPosition musicalPositionAt(double seconds) const noexcept;
    [[nodiscard]] RecordingPlan recordingPlan(double cursorSeconds) const noexcept;
    [[nodiscard]] double lengthSeconds() const noexcept;
    [[nodiscard]] bool hasActivePluginInserts() const noexcept;
    [[nodiscard]] juce::var toVar() const;

    static std::optional<Project> fromVar(const juce::var& value, juce::String& error);
};

juce::String trackTypeToString(TrackType type);
std::optional<TrackType> trackTypeFromString(const juce::String& value);
juce::String pluginBridgeModeToString(PluginBridgeMode mode);
std::optional<PluginBridgeMode> pluginBridgeModeFromString(const juce::String& value);
juce::String stretchModeToString(StretchMode mode);
std::optional<StretchMode> stretchModeFromString(const juce::String& value);
juce::String tonePathTypeToString(TonePathType type);
std::optional<TonePathType> tonePathTypeFromString(const juce::String& value);
std::vector<CompRegion> replaceCompRegion(const std::vector<CompRegion>& existing,
                                          CompRegion replacement);
std::vector<RecordingPass> recordingPasses(double capturedDurationSeconds,
                                            const RecordingPlan& plan);
}
