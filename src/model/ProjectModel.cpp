#include "ProjectModel.h"

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
const juce::DynamicObject* requireObject(const juce::var& value, juce::String& error, const juce::String& context)
{
    if (const auto* object = value.getDynamicObject())
        return object;

    error = context + " must be a JSON object.";
    return nullptr;
}

double numberProperty(const juce::DynamicObject& object, const juce::Identifier& name, double fallback)
{
    const auto value = object.getProperty(name);
    return value.isDouble() || value.isInt() || value.isInt64() ? static_cast<double>(value) : fallback;
}

int integerProperty(const juce::DynamicObject& object, const juce::Identifier& name, int fallback)
{
    const auto value = object.getProperty(name);
    return value.isInt() || value.isInt64() ? static_cast<int>(value) : fallback;
}

bool booleanProperty(const juce::DynamicObject& object, const juce::Identifier& name, bool fallback)
{
    const auto value = object.getProperty(name);
    return value.isBool() ? static_cast<bool>(value) : fallback;
}

juce::Colour colourProperty(const juce::DynamicObject& object,
                            const juce::Identifier& name,
                            juce::Colour fallback)
{
    const auto value = object.getProperty(name);
    return value.isString() ? juce::Colour::fromString(value.toString()) : fallback;
}
}

juce::var PluginInsert::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("pluginIdentifier", pluginIdentifier);
    object->setProperty("name", name);
    object->setProperty("manufacturer", manufacturer);
    object->setProperty("format", format);
    object->setProperty("version", version);
    object->setProperty("fileOrIdentifier", fileOrIdentifier);
    object->setProperty("stateFile", stateFile);
    object->setProperty("stateHash", stateHash);
    object->setProperty("bridgeMode", pluginBridgeModeToString(bridgeMode));
    object->setProperty("latencySamples", latencySamples);
    object->setProperty("tailSeconds", tailSeconds);
    object->setProperty("bypassed", bypassed);
    object->setProperty("missing", missing);
    return juce::var(object.release());
}

std::optional<PluginInsert> PluginInsert::fromVar(const juce::var& value, juce::String& error)
{
    const auto* object = requireObject(value, error, "Plugin insert");
    if (object == nullptr)
        return std::nullopt;

    PluginInsert insert;
    insert.id = object->getProperty("id").toString();
    insert.pluginIdentifier = object->getProperty("pluginIdentifier").toString();
    insert.name = object->getProperty("name").toString();
    insert.manufacturer = object->getProperty("manufacturer").toString();
    insert.format = object->getProperty("format").toString();
    insert.version = object->getProperty("version").toString();
    insert.fileOrIdentifier = object->getProperty("fileOrIdentifier").toString();
    insert.stateFile = object->getProperty("stateFile").toString();
    insert.stateHash = object->getProperty("stateHash").toString();
    insert.latencySamples = juce::jmax(0, integerProperty(*object, "latencySamples", 0));
    insert.tailSeconds = std::max(0.0, numberProperty(*object, "tailSeconds", 0.0));
    insert.bypassed = booleanProperty(*object, "bypassed", false);
    insert.missing = booleanProperty(*object, "missing", false);

    const auto bridgeMode = pluginBridgeModeFromString(object->getProperty("bridgeMode").toString());
    if (!bridgeMode.has_value())
    {
        error = "Plugin insert contains an unsupported bridge mode.";
        return std::nullopt;
    }
    insert.bridgeMode = *bridgeMode;

    if (insert.id.isEmpty() || insert.pluginIdentifier.isEmpty() || insert.name.isEmpty())
    {
        error = "Plugin insert ID, plugin identifier, and name cannot be empty.";
        return std::nullopt;
    }

    if (insert.stateFile.contains("..") || juce::File::isAbsolutePath(insert.stateFile))
    {
        error = "Plugin insert contains an unsafe state path.";
        return std::nullopt;
    }

    return insert;
}

double AudioClip::endSeconds() const noexcept
{
    return startSeconds + durationSeconds;
}

juce::var AudioClip::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("sourceFile", sourceFile.getFullPathName());
    object->setProperty("startSeconds", startSeconds);
    object->setProperty("sourceOffsetSeconds", sourceOffsetSeconds);
    object->setProperty("sourceLengthSeconds", sourceLengthSeconds);
    object->setProperty("durationSeconds", durationSeconds);
    object->setProperty("gainDecibels", gainDecibels);
    object->setProperty("muted", muted);
    object->setProperty("colour", colour.toString());
    return juce::var(object.release());
}

std::optional<AudioClip> AudioClip::fromVar(const juce::var& value, juce::String& error)
{
    const auto* object = requireObject(value, error, "Clip");
    if (object == nullptr)
        return std::nullopt;

    AudioClip clip;
    clip.id = object->getProperty("id").toString();
    clip.name = object->getProperty("name").toString();
    clip.sourceFile = juce::File(object->getProperty("sourceFile").toString());
    clip.startSeconds = numberProperty(*object, "startSeconds", 0.0);
    clip.sourceOffsetSeconds = numberProperty(*object, "sourceOffsetSeconds", 0.0);
    clip.durationSeconds = numberProperty(*object, "durationSeconds", 0.0);
    clip.sourceLengthSeconds = numberProperty(*object,
                                              "sourceLengthSeconds",
                                              clip.sourceOffsetSeconds + clip.durationSeconds);
    clip.gainDecibels = static_cast<float>(numberProperty(*object, "gainDecibels", 0.0));
    clip.muted = booleanProperty(*object, "muted", false);
    clip.colour = colourProperty(*object, "colour", juce::Colour(0xffdd5b3f));

    if (clip.id.isEmpty() || clip.durationSeconds <= 0.0 || clip.startSeconds < 0.0
        || clip.sourceOffsetSeconds < 0.0
        || clip.sourceLengthSeconds < clip.sourceOffsetSeconds + clip.durationSeconds)
    {
        error = "Clip contains an invalid ID or time range.";
        return std::nullopt;
    }

    return clip;
}

juce::var Track::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("parentTrackId", parentTrackId);
    object->setProperty("versionNumber", versionNumber);
    object->setProperty("versionsCollapsed", versionsCollapsed);
    object->setProperty("type", trackTypeToString(type));
    object->setProperty("volumeDecibels", volumeDecibels);
    object->setProperty("pan", pan);
    object->setProperty("muted", muted);
    object->setProperty("solo", solo);
    object->setProperty("armed", armed);
    object->setProperty("inputChannel", inputChannel);
    object->setProperty("stereoInput", stereoInput);
    object->setProperty("inputMonitoring", inputMonitoring);
    object->setProperty("colour", colour.toString());

    juce::Array<juce::var> insertValues;
    insertValues.ensureStorageAllocated(static_cast<int>(inserts.size()));
    for (const auto& insert : inserts)
        insertValues.add(insert.toVar());
    object->setProperty("inserts", juce::var(insertValues));

    juce::Array<juce::var> clipValues;
    clipValues.ensureStorageAllocated(static_cast<int>(clips.size()));
    for (const auto& clip : clips)
        clipValues.add(clip.toVar());

    object->setProperty("clips", juce::var(clipValues));
    return juce::var(object.release());
}

std::optional<Track> Track::fromVar(const juce::var& value, juce::String& error)
{
    const auto* object = requireObject(value, error, "Track");
    if (object == nullptr)
        return std::nullopt;

    Track track;
    track.id = object->getProperty("id").toString();
    track.name = object->getProperty("name").toString();
    track.parentTrackId = object->getProperty("parentTrackId").toString();
    track.versionNumber = juce::jmax(0, integerProperty(*object, "versionNumber", 0));
    track.versionsCollapsed = booleanProperty(*object, "versionsCollapsed", false);

    const auto type = trackTypeFromString(object->getProperty("type").toString());
    if (!type.has_value())
    {
        error = "Track contains an unsupported type.";
        return std::nullopt;
    }

    track.type = *type;
    track.volumeDecibels = static_cast<float>(numberProperty(*object, "volumeDecibels", 0.0));
    track.pan = juce::jlimit(-1.0f, 1.0f, static_cast<float>(numberProperty(*object, "pan", 0.0)));
    track.muted = booleanProperty(*object, "muted", false);
    track.solo = booleanProperty(*object, "solo", false);
    track.armed = booleanProperty(*object, "armed", false);
    track.inputChannel = juce::jmax(0, integerProperty(*object, "inputChannel", 0));
    track.stereoInput = booleanProperty(*object, "stereoInput", false);
    track.inputMonitoring = booleanProperty(*object, "inputMonitoring", false);
    track.colour = colourProperty(*object, "colour", juce::Colour(0xffdd5b3f));

    const auto insertValues = object->getProperty("inserts");
    if (!insertValues.isVoid())
    {
        if (!insertValues.isArray())
        {
            error = "Track inserts must be a JSON array.";
            return std::nullopt;
        }

        for (const auto& insertValue : *insertValues.getArray())
        {
            auto insert = PluginInsert::fromVar(insertValue, error);
            if (!insert.has_value())
                return std::nullopt;

            track.inserts.push_back(std::move(*insert));
        }
    }

    const auto clipValues = object->getProperty("clips");
    if (!clipValues.isArray())
    {
        error = "Track clips must be a JSON array.";
        return std::nullopt;
    }

    for (const auto& clipValue : *clipValues.getArray())
    {
        auto clip = AudioClip::fromVar(clipValue, error);
        if (!clip.has_value())
            return std::nullopt;

        track.clips.push_back(std::move(*clip));
    }

    if (track.id.isEmpty())
    {
        error = "Track ID cannot be empty.";
        return std::nullopt;
    }

    return track;
}

Project Project::createDefault()
{
    Project project;

    Track rhythmLeft;
    rhythmLeft.name = "Rhythm L";
    rhythmLeft.colour = juce::Colour(0xffdd5b3f);
    rhythmLeft.armed = true;

    Track rhythmRight;
    rhythmRight.name = "Rhythm R";
    rhythmRight.colour = juce::Colour(0xffd98f39);

    Track master;
    master.name = "Master";
    master.type = TrackType::master;
    master.colour = juce::Colour(0xff78c6a3);

    project.tracks = { std::move(rhythmLeft), std::move(rhythmRight), std::move(master) };
    return project;
}

Track* Project::findTrack(const juce::String& trackId)
{
    const auto iterator = std::find_if(tracks.begin(), tracks.end(), [&trackId](const auto& track)
    {
        return track.id == trackId;
    });
    return iterator == tracks.end() ? nullptr : &*iterator;
}

const Track* Project::findTrack(const juce::String& trackId) const
{
    const auto iterator = std::find_if(tracks.cbegin(), tracks.cend(), [&trackId](const auto& track)
    {
        return track.id == trackId;
    });
    return iterator == tracks.cend() ? nullptr : &*iterator;
}

AudioClip* Project::findClip(const juce::String& clipId)
{
    for (auto& track : tracks)
        if (const auto iterator = std::find_if(track.clips.begin(), track.clips.end(), [&clipId](const auto& clip)
            {
                return clip.id == clipId;
            }); iterator != track.clips.end())
            return &*iterator;

    return nullptr;
}

const AudioClip* Project::findClip(const juce::String& clipId) const
{
    for (const auto& track : tracks)
        if (const auto iterator = std::find_if(track.clips.cbegin(), track.clips.cend(), [&clipId](const auto& clip)
            {
                return clip.id == clipId;
            }); iterator != track.clips.cend())
            return &*iterator;

    return nullptr;
}

Track* Project::findTrackContainingClip(const juce::String& clipId)
{
    const auto iterator = std::find_if(tracks.begin(), tracks.end(), [&clipId](const auto& track)
    {
        return std::any_of(track.clips.cbegin(), track.clips.cend(), [&clipId](const auto& clip)
        {
            return clip.id == clipId;
        });
    });
    return iterator == tracks.end() ? nullptr : &*iterator;
}

const Track* Project::findTrackContainingClip(const juce::String& clipId) const
{
    const auto iterator = std::find_if(tracks.cbegin(), tracks.cend(), [&clipId](const auto& track)
    {
        return std::any_of(track.clips.cbegin(), track.clips.cend(), [&clipId](const auto& clip)
        {
            return clip.id == clipId;
        });
    });
    return iterator == tracks.cend() ? nullptr : &*iterator;
}

double Project::lengthSeconds() const noexcept
{
    double length = 8.0;
    for (const auto& track : tracks)
        for (const auto& clip : track.clips)
            length = std::max(length, clip.endSeconds());

    return length;
}

bool Project::hasActivePluginInserts() const noexcept
{
    return std::any_of(tracks.cbegin(), tracks.cend(), [](const auto& track)
    {
        return std::any_of(track.inserts.cbegin(), track.inserts.cend(), [](const auto& insert)
        {
            return !insert.bypassed && !insert.missing;
        });
    });
}

juce::var Project::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("formatVersion", currentFormatVersion);
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("tempo", tempo);
    object->setProperty("timeSignatureNumerator", timeSignatureNumerator);
    object->setProperty("timeSignatureDenominator", timeSignatureDenominator);
    object->setProperty("loopEnabled", loopEnabled);
    object->setProperty("loopStartSeconds", loopStartSeconds);
    object->setProperty("loopEndSeconds", loopEndSeconds);

    juce::Array<juce::var> trackValues;
    trackValues.ensureStorageAllocated(static_cast<int>(tracks.size()));
    for (const auto& track : tracks)
        trackValues.add(track.toVar());

    object->setProperty("tracks", juce::var(trackValues));
    return juce::var(object.release());
}

std::optional<Project> Project::fromVar(const juce::var& value, juce::String& error)
{
    const auto* object = requireObject(value, error, "Project");
    if (object == nullptr)
        return std::nullopt;

    const auto version = integerProperty(*object, "formatVersion", 0);
    if (version != currentFormatVersion)
    {
        error = "Unsupported Studio Duo project format version " + juce::String(version) + ".";
        return std::nullopt;
    }

    Project project;
    project.id = object->getProperty("id").toString();
    project.name = object->getProperty("name").toString();
    project.tempo = juce::jlimit(20.0, 400.0, numberProperty(*object, "tempo", 120.0));
    project.timeSignatureNumerator = juce::jlimit(1, 32, integerProperty(*object, "timeSignatureNumerator", 4));
    project.timeSignatureDenominator = integerProperty(*object, "timeSignatureDenominator", 4);
    project.loopEnabled = booleanProperty(*object, "loopEnabled", false);
    project.loopStartSeconds = std::max(0.0, numberProperty(*object, "loopStartSeconds", 0.0));
    project.loopEndSeconds = std::max(project.loopStartSeconds, numberProperty(*object, "loopEndSeconds", 8.0));

    const auto trackValues = object->getProperty("tracks");
    if (!trackValues.isArray())
    {
        error = "Project tracks must be a JSON array.";
        return std::nullopt;
    }

    for (const auto& trackValue : *trackValues.getArray())
    {
        auto track = Track::fromVar(trackValue, error);
        if (!track.has_value())
            return std::nullopt;

        project.tracks.push_back(std::move(*track));
    }

    if (project.id.isEmpty() || project.name.isEmpty())
    {
        error = "Project ID and name cannot be empty.";
        return std::nullopt;
    }

    return project;
}

juce::String trackTypeToString(TrackType type)
{
    switch (type)
    {
        case TrackType::audio: return "audio";
        case TrackType::instrument: return "instrument";
        case TrackType::aux: return "aux";
        case TrackType::bus: return "bus";
        case TrackType::master: return "master";
    }

    return "audio";
}

std::optional<TrackType> trackTypeFromString(const juce::String& value)
{
    if (value == "audio") return TrackType::audio;
    if (value == "instrument") return TrackType::instrument;
    if (value == "aux") return TrackType::aux;
    if (value == "bus") return TrackType::bus;
    if (value == "master") return TrackType::master;
    return std::nullopt;
}

juce::String pluginBridgeModeToString(PluginBridgeMode mode)
{
    switch (mode)
    {
        case PluginBridgeMode::sandboxed: return "sandboxed";
        case PluginBridgeMode::araCompatibility: return "araCompatibility";
        case PluginBridgeMode::trustedInProcess: return "trustedInProcess";
    }

    return "sandboxed";
}

std::optional<PluginBridgeMode> pluginBridgeModeFromString(const juce::String& value)
{
    if (value == "sandboxed") return PluginBridgeMode::sandboxed;
    if (value == "araCompatibility") return PluginBridgeMode::araCompatibility;
    if (value == "trustedInProcess") return PluginBridgeMode::trustedInProcess;
    return std::nullopt;
}
}
