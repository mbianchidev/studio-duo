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

bool validMeterDenominator(int denominator)
{
    return denominator == 1
        || denominator == 2
        || denominator == 4
        || denominator == 8
        || denominator == 16
        || denominator == 32;
}
}

juce::var TempoChange::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("timeSeconds", timeSeconds);
    object->setProperty("bpm", bpm);
    object->setProperty("rampToNext", rampToNext);
    return juce::var(object.release());
}

bool TempoChange::operator==(const TempoChange& other) const noexcept
{
    return std::abs(timeSeconds - other.timeSeconds) < 0.0000001
        && std::abs(bpm - other.bpm) < 0.0000001
        && rampToNext == other.rampToNext;
}

std::optional<TempoChange> TempoChange::fromVar(const juce::var& value,
                                                juce::String& error)
{
    const auto* object = requireObject(value, error, "Tempo change");
    if (object == nullptr)
        return std::nullopt;

    TempoChange change;
    change.timeSeconds = numberProperty(*object, "timeSeconds", 0.0);
    change.bpm = numberProperty(*object, "bpm", 120.0);
    change.rampToNext = booleanProperty(*object, "rampToNext", false);
    if (change.timeSeconds < 0.0 || change.bpm < 20.0 || change.bpm > 400.0)
    {
        error = "Tempo changes require a non-negative time and a tempo from 20 to 400 BPM.";
        return std::nullopt;
    }
    return change;
}

juce::var MeterChange::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("timeSeconds", timeSeconds);
    object->setProperty("numerator", numerator);
    object->setProperty("denominator", denominator);
    return juce::var(object.release());
}

bool MeterChange::operator==(const MeterChange& other) const noexcept
{
    return std::abs(timeSeconds - other.timeSeconds) < 0.0000001
        && numerator == other.numerator
        && denominator == other.denominator;
}

std::optional<MeterChange> MeterChange::fromVar(const juce::var& value,
                                                juce::String& error)
{
    const auto* object = requireObject(value, error, "Meter change");
    if (object == nullptr)
        return std::nullopt;

    MeterChange change;
    change.timeSeconds = numberProperty(*object, "timeSeconds", 0.0);
    change.numerator = integerProperty(*object, "numerator", 4);
    change.denominator = integerProperty(*object, "denominator", 4);
    if (change.timeSeconds < 0.0
        || change.numerator < 1
        || change.numerator > 32
        || !validMeterDenominator(change.denominator))
    {
        error = "Meter changes require a valid position and a supported time signature.";
        return std::nullopt;
    }
    return change;
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

double AudioClip::sourceRangeEnd() const noexcept
{
    return sourceRangeEndSeconds > 0.0
        ? sourceRangeEndSeconds
        : sourceLengthSeconds;
}

double AudioClip::recoverableStartSeconds() const noexcept
{
    return startSeconds - (sourceOffsetSeconds - sourceRangeStartSeconds);
}

double AudioClip::recoverableEndSeconds() const noexcept
{
    return startSeconds + (sourceRangeEnd() - sourceOffsetSeconds);
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
    object->setProperty("sourceRangeStartSeconds", sourceRangeStartSeconds);
    object->setProperty("sourceRangeEndSeconds", sourceRangeEnd());
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
    clip.sourceRangeStartSeconds = numberProperty(*object, "sourceRangeStartSeconds", 0.0);
    clip.sourceRangeEndSeconds = numberProperty(*object,
                                                "sourceRangeEndSeconds",
                                                clip.sourceLengthSeconds);
    clip.gainDecibels = static_cast<float>(numberProperty(*object, "gainDecibels", 0.0));
    clip.muted = booleanProperty(*object, "muted", false);
    clip.colour = colourProperty(*object, "colour", juce::Colour(0xffdd5b3f));

    if (clip.id.isEmpty() || clip.durationSeconds <= 0.0 || clip.startSeconds < 0.0
        || clip.sourceOffsetSeconds < 0.0
        || clip.sourceRangeStartSeconds < 0.0
        || clip.sourceRangeStartSeconds > clip.sourceOffsetSeconds + 0.0001
        || clip.sourceRangeStartSeconds >= clip.sourceRangeEnd() - 0.0001
        || clip.sourceRangeEnd() > clip.sourceLengthSeconds + 0.0001
        || clip.sourceRangeEnd() < clip.sourceOffsetSeconds + clip.durationSeconds - 0.0001)
    {
        error = "Clip contains an invalid ID or time range.";
        return std::nullopt;
    }

    return clip;
}

double CompRegion::endSeconds() const noexcept
{
    return startSeconds + durationSeconds;
}

juce::var CompRegion::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("sourceTrackId", sourceTrackId);
    object->setProperty("startSeconds", startSeconds);
    object->setProperty("durationSeconds", durationSeconds);
    return juce::var(object.release());
}

std::optional<CompRegion> CompRegion::fromVar(const juce::var& value,
                                              juce::String& error)
{
    const auto* object = requireObject(value, error, "Comp region");
    if (object == nullptr)
        return std::nullopt;

    CompRegion region;
    region.id = object->getProperty("id").toString();
    region.sourceTrackId = object->getProperty("sourceTrackId").toString();
    region.startSeconds = numberProperty(*object, "startSeconds", 0.0);
    region.durationSeconds = numberProperty(*object, "durationSeconds", 0.0);
    if (region.id.isEmpty()
        || region.sourceTrackId.isEmpty()
        || region.startSeconds < 0.0
        || region.durationSeconds <= 0.0)
    {
        error = "Comp regions require IDs and a positive timeline range.";
        return std::nullopt;
    }
    return region;
}

juce::var Track::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("parentTrackId", parentTrackId);
    object->setProperty("versionNumber", versionNumber);
    object->setProperty("versionsCollapsed", versionsCollapsed);
    object->setProperty("activeTakeTrackId", activeTakeTrackId);
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

    juce::Array<juce::var> compValues;
    compValues.ensureStorageAllocated(static_cast<int>(compRegions.size()));
    for (const auto& region : compRegions)
        compValues.add(region.toVar());
    object->setProperty("compRegions", juce::var(compValues));
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
    track.activeTakeTrackId = object->getProperty("activeTakeTrackId").toString();

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

    const auto compValues = object->getProperty("compRegions");
    if (!compValues.isVoid())
    {
        if (!compValues.isArray())
        {
            error = "Track comp regions must be a JSON array.";
            return std::nullopt;
        }
        for (const auto& compValue : *compValues.getArray())
        {
            auto region = CompRegion::fromVar(compValue, error);
            if (!region.has_value())
                return std::nullopt;
            track.compRegions.push_back(std::move(*region));
        }
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

std::vector<juce::String> Project::armedAudioParentTrackIds() const
{
    std::vector<juce::String> parentIds;
    for (const auto& track : tracks)
    {
        if (track.type != TrackType::audio || !track.armed)
            continue;

        const auto parentId = track.parentTrackId.isNotEmpty()
            ? track.parentTrackId
            : track.id;
        const auto* parent = findTrack(parentId);
        if (parent == nullptr || parent->type != TrackType::audio)
            continue;

        if (std::find(parentIds.cbegin(), parentIds.cend(), parentId) == parentIds.cend())
            parentIds.push_back(parentId);
    }
    return parentIds;
}

juce::String Project::activeTakeTrackId(const juce::String& parentTrackId) const
{
    const auto* parent = findTrack(parentTrackId);
    if (parent == nullptr)
        return {};
    if (parent->activeTakeTrackId.isNotEmpty())
    {
        const auto* active = findTrack(parent->activeTakeTrackId);
        if (active != nullptr && active->parentTrackId == parentTrackId)
            return active->id;
    }

    const Track* latest = nullptr;
    for (const auto& track : tracks)
    {
        if (track.parentTrackId != parentTrackId)
            continue;
        if (latest == nullptr || track.versionNumber > latest->versionNumber)
            latest = &track;
    }
    return latest != nullptr ? latest->id : juce::String();
}

double Project::tempoAt(double seconds) const noexcept
{
    if (tempoChanges.empty())
        return tempo;

    const auto position = std::max(0.0, seconds);
    if (position < tempoChanges.front().timeSeconds)
        return tempo;
    auto index = std::size_t { 0 };
    while (index + 1 < tempoChanges.size()
           && tempoChanges[index + 1].timeSeconds <= position)
        ++index;

    const auto& current = tempoChanges[index];
    if (!current.rampToNext || index + 1 >= tempoChanges.size())
        return current.bpm;

    const auto& next = tempoChanges[index + 1];
    const auto duration = next.timeSeconds - current.timeSeconds;
    if (duration <= 0.0)
        return next.bpm;
    const auto progress = juce::jlimit(0.0,
                                       1.0,
                                       (position - current.timeSeconds) / duration);
    return current.bpm + (next.bpm - current.bpm) * progress;
}

MeterChange Project::meterAt(double seconds) const noexcept
{
    if (meterChanges.empty())
        return { 0.0, timeSignatureNumerator, timeSignatureDenominator };

    const auto position = std::max(0.0, seconds);
    auto current = MeterChange { 0.0,
                                 timeSignatureNumerator,
                                 timeSignatureDenominator };
    for (const auto& change : meterChanges)
    {
        if (change.timeSeconds > position)
            break;
        current = change;
    }
    return current;
}

double Project::beatsAt(double seconds) const noexcept
{
    const auto target = std::max(0.0, seconds);
    if (tempoChanges.empty())
        return target * tempo / 60.0;

    auto beats = 0.0;
    if (tempoChanges.front().timeSeconds > 0.0)
    {
        const auto initialEnd = std::min(target, tempoChanges.front().timeSeconds);
        beats += initialEnd * tempo / 60.0;
        if (target <= tempoChanges.front().timeSeconds)
            return beats;
    }

    for (std::size_t index = 0; index < tempoChanges.size(); ++index)
    {
        const auto& current = tempoChanges[index];
        if (target <= current.timeSeconds)
            return beats;
        const auto segmentEnd = index + 1 < tempoChanges.size()
            ? tempoChanges[index + 1].timeSeconds
            : target;
        const auto end = std::min(target, segmentEnd);
        if (end <= current.timeSeconds)
            continue;

        const auto elapsed = end - current.timeSeconds;
        if (current.rampToNext && index + 1 < tempoChanges.size())
        {
            const auto duration = tempoChanges[index + 1].timeSeconds
                - current.timeSeconds;
            const auto slope = duration > 0.0
                ? (tempoChanges[index + 1].bpm - current.bpm) / duration
                : 0.0;
            beats += (current.bpm * elapsed + 0.5 * slope * elapsed * elapsed) / 60.0;
        }
        else
        {
            beats += current.bpm * elapsed / 60.0;
        }

        if (target <= segmentEnd)
            return beats;
    }
    return beats;
}

double Project::secondsAtBeat(double beats) const noexcept
{
    const auto target = std::max(0.0, beats);
    if (target == 0.0)
        return 0.0;

    auto upper = std::max(1.0, target * 60.0 / std::max(20.0, tempo));
    while (beatsAt(upper) < target && upper < 86400.0)
        upper *= 2.0;

    auto lower = 0.0;
    for (int iteration = 0; iteration < 80; ++iteration)
    {
        const auto middle = (lower + upper) * 0.5;
        if (beatsAt(middle) < target)
            lower = middle;
        else
            upper = middle;
    }
    return (lower + upper) * 0.5;
}

MusicalPosition Project::musicalPositionAt(double seconds) const noexcept
{
    const auto target = std::max(0.0, seconds);
    auto current = MeterChange { 0.0,
                                 timeSignatureNumerator,
                                 timeSignatureDenominator };
    auto completedBars = 0;
    for (const auto& change : meterChanges)
    {
        if (change.timeSeconds <= current.timeSeconds + 0.0000001)
        {
            current = change;
            continue;
        }
        if (change.timeSeconds > target)
            break;

        const auto quarterBeats = beatsAt(change.timeSeconds)
            - beatsAt(current.timeSeconds);
        const auto metricBeats = quarterBeats
            * static_cast<double>(current.denominator)
            / 4.0;
        completedBars += static_cast<int>(
            std::ceil(metricBeats / static_cast<double>(current.numerator)
                      - 0.0000001));
        current = change;
    }

    const auto quarterBeats = beatsAt(target) - beatsAt(current.timeSeconds);
    const auto metricBeats = std::max(
        0.0,
        quarterBeats * static_cast<double>(current.denominator) / 4.0);
    const auto barOffset = static_cast<int>(
        std::floor(metricBeats / static_cast<double>(current.numerator)));
    const auto beatInBar = metricBeats
        - static_cast<double>(barOffset * current.numerator);
    auto beat = static_cast<int>(std::floor(beatInBar));
    auto ticks = static_cast<int>(
        std::round((beatInBar - static_cast<double>(beat)) * 960.0));
    if (ticks >= 960)
    {
        ticks = 0;
        ++beat;
    }
    if (beat >= current.numerator)
    {
        beat = 0;
        return { completedBars + barOffset + 2, 1, ticks, current };
    }
    return { completedBars + barOffset + 1, beat + 1, ticks, current };
}

RecordingPlan Project::recordingPlan(double cursorSeconds) const noexcept
{
    RecordingPlan plan;
    plan.loopEnabled = !punchEnabled
        && loopEnabled
        && loopEndSeconds > loopStartSeconds;
    plan.loopStartSeconds = loopStartSeconds;
    plan.loopEndSeconds = loopEndSeconds;
    plan.captureStartSeconds = punchEnabled
        ? punchInSeconds
        : plan.loopEnabled ? loopStartSeconds : std::max(0.0, cursorSeconds);
    plan.captureEndSeconds = punchEnabled ? punchOutSeconds : -1.0;
    plan.transportEndSeconds = punchEnabled
        ? punchOutSeconds + std::max(0.0, postRollSeconds)
        : -1.0;

    const auto meter = meterAt(plan.captureStartSeconds);
    const auto countInQuarterNotes = static_cast<double>(std::max(0, countInBars))
        * static_cast<double>(meter.numerator)
        * 4.0
        / static_cast<double>(meter.denominator);
    const auto countInStart = secondsAtBeat(
        std::max(0.0, beatsAt(plan.captureStartSeconds) - countInQuarterNotes));
    plan.transportStartSeconds = std::max(
        0.0,
        countInStart - std::max(0.0, preRollSeconds));
    return plan;
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
    juce::Array<juce::var> tempoValues;
    for (const auto& change : tempoChanges)
        tempoValues.add(change.toVar());
    object->setProperty("tempoChanges", juce::var(tempoValues));
    juce::Array<juce::var> meterValues;
    for (const auto& change : meterChanges)
        meterValues.add(change.toVar());
    object->setProperty("meterChanges", juce::var(meterValues));
    object->setProperty("metronomeEnabled", metronomeEnabled);
    object->setProperty("metronomeSubdivision", metronomeSubdivision);
    object->setProperty("metronomeOutputChannel", metronomeOutputChannel);
    object->setProperty("metronomeLevel", metronomeLevel);
    object->setProperty("metronomeAccentLevel", metronomeAccentLevel);
    object->setProperty("punchEnabled", punchEnabled);
    object->setProperty("punchInSeconds", punchInSeconds);
    object->setProperty("punchOutSeconds", punchOutSeconds);
    object->setProperty("countInBars", countInBars);
    object->setProperty("preRollSeconds", preRollSeconds);
    object->setProperty("postRollSeconds", postRollSeconds);
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
    const auto tempoValues = object->getProperty("tempoChanges");
    if (tempoValues.isArray())
    {
        for (const auto& tempoValue : *tempoValues.getArray())
        {
            auto change = TempoChange::fromVar(tempoValue, error);
            if (!change.has_value())
                return std::nullopt;
            project.tempoChanges.push_back(*change);
        }
        std::stable_sort(project.tempoChanges.begin(),
                         project.tempoChanges.end(),
                         [](const auto& left, const auto& right)
                         {
                             return left.timeSeconds < right.timeSeconds;
                         });
    }
    const auto meterValues = object->getProperty("meterChanges");
    if (meterValues.isArray())
    {
        for (const auto& meterValue : *meterValues.getArray())
        {
            auto change = MeterChange::fromVar(meterValue, error);
            if (!change.has_value())
                return std::nullopt;
            project.meterChanges.push_back(*change);
        }
        std::stable_sort(project.meterChanges.begin(),
                         project.meterChanges.end(),
                         [](const auto& left, const auto& right)
                         {
                             return left.timeSeconds < right.timeSeconds;
                         });
    }
    project.metronomeEnabled = booleanProperty(*object, "metronomeEnabled", true);
    project.metronomeSubdivision = juce::jlimit(
        1,
        8,
        integerProperty(*object, "metronomeSubdivision", 1));
    project.metronomeOutputChannel = std::max(
        0,
        integerProperty(*object, "metronomeOutputChannel", 0));
    project.metronomeLevel = juce::jlimit(
        0.0f,
        1.0f,
        static_cast<float>(numberProperty(*object, "metronomeLevel", 0.65)));
    project.metronomeAccentLevel = juce::jlimit(
        0.0f,
        1.0f,
        static_cast<float>(numberProperty(*object, "metronomeAccentLevel", 1.0)));
    project.punchEnabled = booleanProperty(*object, "punchEnabled", false);
    project.punchInSeconds = std::max(0.0,
                                      numberProperty(*object, "punchInSeconds", 0.0));
    project.punchOutSeconds = std::max(
        project.punchInSeconds,
        numberProperty(*object, "punchOutSeconds", 8.0));
    project.countInBars = juce::jlimit(
        0,
        8,
        integerProperty(*object, "countInBars", 0));
    project.preRollSeconds = juce::jlimit(
        0.0,
        30.0,
        numberProperty(*object, "preRollSeconds", 0.0));
    project.postRollSeconds = juce::jlimit(
        0.0,
        30.0,
        numberProperty(*object, "postRollSeconds", 0.0));
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

    for (const auto& track : project.tracks)
    {
        if (track.activeTakeTrackId.isNotEmpty())
        {
            const auto* activeTake = project.findTrack(track.activeTakeTrackId);
            if (activeTake == nullptr || activeTake->parentTrackId != track.id)
            {
                error = "A track references an invalid active take.";
                return std::nullopt;
            }
        }
        for (const auto& region : track.compRegions)
        {
            const auto* take = project.findTrack(region.sourceTrackId);
            if (take == nullptr || take->parentTrackId != track.id)
            {
                error = "A comp region references an invalid take lane.";
                return std::nullopt;
            }
        }
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

std::vector<CompRegion> replaceCompRegion(const std::vector<CompRegion>& existing,
                                          CompRegion replacement)
{
    std::vector<CompRegion> result;
    const auto replacementEnd = replacement.endSeconds();
    for (const auto& region : existing)
    {
        if (region.endSeconds() <= replacement.startSeconds
            || region.startSeconds >= replacementEnd)
        {
            result.push_back(region);
            continue;
        }

        if (region.startSeconds < replacement.startSeconds)
        {
            auto left = region;
            left.durationSeconds = replacement.startSeconds - region.startSeconds;
            result.push_back(std::move(left));
        }
        if (region.endSeconds() > replacementEnd)
        {
            auto right = region;
            if (region.startSeconds < replacement.startSeconds)
                right.id = juce::Uuid().toString();
            right.startSeconds = replacementEnd;
            right.durationSeconds = region.endSeconds() - replacementEnd;
            result.push_back(std::move(right));
        }
    }

    if (replacement.durationSeconds > 0.0 && replacement.sourceTrackId.isNotEmpty())
        result.push_back(std::move(replacement));
    std::stable_sort(result.begin(),
                     result.end(),
                     [](const auto& left, const auto& right)
                     {
                         return left.startSeconds < right.startSeconds;
                     });
    return result;
}

std::vector<RecordingPass> recordingPasses(double capturedDurationSeconds,
                                            const RecordingPlan& plan)
{
    const auto duration = std::max(0.0, capturedDurationSeconds);
    if (duration <= 0.0)
        return {};
    if (!plan.loopEnabled || plan.loopEndSeconds <= plan.loopStartSeconds)
        return { { plan.captureStartSeconds, 0.0, duration } };

    const auto loopDuration = plan.loopEndSeconds - plan.loopStartSeconds;
    std::vector<RecordingPass> passes;
    for (auto offset = 0.0; offset < duration - 0.0000001; offset += loopDuration)
    {
        passes.push_back({
            plan.loopStartSeconds,
            offset,
            std::min(loopDuration, duration - offset)
        });
    }
    return passes;
}
}
