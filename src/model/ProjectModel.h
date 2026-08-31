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
    double durationSeconds = 4.0;
    float gainDecibels = 0.0f;
    bool muted = false;
    juce::Colour colour { 0xffdd5b3f };

    [[nodiscard]] double endSeconds() const noexcept;
    [[nodiscard]] juce::var toVar() const;
    static std::optional<AudioClip> fromVar(const juce::var& value, juce::String& error);
};

struct Track
{
    juce::String id { juce::Uuid().toString() };
    juce::String name { "Audio" };
    TrackType type = TrackType::audio;
    float volumeDecibels = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool solo = false;
    bool armed = false;
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
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 8.0;
    std::vector<Track> tracks;

    static Project createDefault();

    [[nodiscard]] Track* findTrack(const juce::String& trackId);
    [[nodiscard]] const Track* findTrack(const juce::String& trackId) const;
    [[nodiscard]] AudioClip* findClip(const juce::String& clipId);
    [[nodiscard]] const AudioClip* findClip(const juce::String& clipId) const;
    [[nodiscard]] Track* findTrackContainingClip(const juce::String& clipId);
    [[nodiscard]] const Track* findTrackContainingClip(const juce::String& clipId) const;
    [[nodiscard]] double lengthSeconds() const noexcept;
    [[nodiscard]] juce::var toVar() const;

    static std::optional<Project> fromVar(const juce::var& value, juce::String& error);
};

juce::String trackTypeToString(TrackType type);
std::optional<TrackType> trackTypeFromString(const juce::String& value);
juce::String pluginBridgeModeToString(PluginBridgeMode mode);
std::optional<PluginBridgeMode> pluginBridgeModeFromString(const juce::String& value);
}
