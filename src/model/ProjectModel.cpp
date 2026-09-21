#include "ProjectModel.h"

#include "mix/RoutingGraph.h"
#include "project_io/ProjectMigration.h"

#include <algorithm>
#include <array>
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

bool readSectionId(const juce::DynamicObject& object,
                   juce::String& sectionId,
                   juce::String& error)
{
    if (!object.hasProperty("sectionId"))
        return true;
    const auto value = object.getProperty("sectionId");
    if (!value.isString())
    {
        error = "Transport map section IDs must be strings.";
        return false;
    }
    sectionId = value.toString();
    return true;
}

template <typename Change>
bool validateSectionOwner(const Project& project,
                          const std::vector<Change>& changes,
                          std::size_t index,
                          juce::String& error)
{
    const auto& change = changes[index];
    if (change.sectionId.isEmpty())
        return true;

    const auto* section = project.findSection(change.sectionId);
    if (section == nullptr
        || !juce::exactlyEqual(section->timeSeconds, change.timeSeconds))
    {
        error = "A section-owned transport point must match an existing section's position.";
        return false;
    }
    if (std::any_of(
            changes.cbegin(),
            changes.cbegin() + static_cast<std::ptrdiff_t>(index),
            [&change](const auto& previous)
            {
                return previous.sectionId == change.sectionId;
            }))
    {
        error = "A section can own only one point in each transport map.";
        return false;
    }
    return true;
}

bool validSha256(const juce::String& value)
{
    return value.isEmpty()
        || (value.length() == 64
            && value.containsOnly("0123456789abcdefABCDEF"));
}
}

juce::var WarpMarker::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("timelineOffsetSeconds", timelineOffsetSeconds);
    object->setProperty("sourceSeconds", sourceSeconds);
    return juce::var(object.release());
}

std::optional<WarpMarker> WarpMarker::fromVar(const juce::var& value,
                                              juce::String& error)
{
    const auto* object = requireObject(value, error, "Warp marker");
    if (object == nullptr)
        return std::nullopt;

    WarpMarker marker;
    marker.timelineOffsetSeconds = numberProperty(*object,
                                                  "timelineOffsetSeconds",
                                                  0.0);
    marker.sourceSeconds = numberProperty(*object, "sourceSeconds", 0.0);
    if (marker.timelineOffsetSeconds < 0.0 || marker.sourceSeconds < 0.0)
    {
        error = "Warp markers cannot contain negative positions.";
        return std::nullopt;
    }
    return marker;
}

juce::var TempoChange::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("timeSeconds", timeSeconds);
    object->setProperty("bpm", bpm);
    object->setProperty("rampToNext", rampToNext);
    if (sectionId.isNotEmpty())
        object->setProperty("sectionId", sectionId);
    return juce::var(object.release());
}

bool TempoChange::operator==(const TempoChange& other) const noexcept
{
    return std::abs(timeSeconds - other.timeSeconds) < 0.0000001
        && std::abs(bpm - other.bpm) < 0.0000001
        && rampToNext == other.rampToNext
        && sectionId == other.sectionId;
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
    if (!readSectionId(*object, change.sectionId, error))
        return std::nullopt;
    if (!std::isfinite(change.timeSeconds)
        || !std::isfinite(change.bpm)
        || change.timeSeconds < 0.0
        || change.bpm < 20.0
        || change.bpm > 400.0)
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
    if (sectionId.isNotEmpty())
        object->setProperty("sectionId", sectionId);
    return juce::var(object.release());
}

bool MeterChange::operator==(const MeterChange& other) const noexcept
{
    return std::abs(timeSeconds - other.timeSeconds) < 0.0000001
        && numerator == other.numerator
        && denominator == other.denominator
        && sectionId == other.sectionId;
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
    if (!readSectionId(*object, change.sectionId, error))
        return std::nullopt;
    if (!std::isfinite(change.timeSeconds)
        || change.timeSeconds < 0.0
        || change.numerator < 1
        || change.numerator > 32
        || !validMeterDenominator(change.denominator))
    {
        error = "Meter changes require a valid position and a supported time signature.";
        return std::nullopt;
    }
    return change;
}

bool SectionClickSettings::operator==(const SectionClickSettings& other) const
{
    return enabled == other.enabled && subdivision == other.subdivision
        && juce::exactlyEqual(level, other.level)
        && juce::exactlyEqual(accentLevel, other.accentLevel)
        && accentBeats == other.accentBeats;
}

bool SectionClickSettings::validate(juce::String& error) const
{
    if (subdivision < 1 || subdivision > 8
        || !std::isfinite(level) || level < 0.0f || level > 1.0f
        || !std::isfinite(accentLevel) || accentLevel < 0.0f || accentLevel > 1.0f)
    {
        error = "Section click subdivisions must be from 1 to 8 and levels must be finite values from 0 to 1.";
        return false;
    }
    std::array<bool, 33> seen {};
    for (const auto beat : accentBeats)
    {
        if (beat < 1 || beat > 32 || seen[static_cast<std::size_t>(beat)])
        {
            error = "Section click accent beats must be unique integers from 1 to 32.";
            return false;
        }
        seen[static_cast<std::size_t>(beat)] = true;
    }
    return true;
}

juce::var SectionClickSettings::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("enabled", enabled);
    object->setProperty("subdivision", subdivision);
    object->setProperty("level", level);
    object->setProperty("accentLevel", accentLevel);
    juce::Array<juce::var> accents;
    for (const auto beat : accentBeats)
        accents.add(beat);
    object->setProperty("accentBeats", juce::var(accents));
    return juce::var(object.release());
}

std::optional<SectionClickSettings> SectionClickSettings::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Section click settings");
    if (object == nullptr)
        return std::nullopt;

    SectionClickSettings settings;
    if (object->hasProperty("enabled"))
    {
        const auto enabledValue = object->getProperty("enabled");
        if (!enabledValue.isBool())
        {
            error = "The section click enabled setting must be a boolean.";
            return std::nullopt;
        }
        settings.enabled = static_cast<bool>(enabledValue);
    }
    if (object->hasProperty("subdivision"))
    {
        const auto subdivisionValue = object->getProperty("subdivision");
        if ((!subdivisionValue.isInt() && !subdivisionValue.isInt64())
            || static_cast<juce::int64>(subdivisionValue) < 1
            || static_cast<juce::int64>(subdivisionValue) > 8)
        {
            error = "The section click subdivision must be an integer from 1 to 8.";
            return std::nullopt;
        }
        settings.subdivision = static_cast<int>(subdivisionValue);
    }
    const auto readLevel = [&](const juce::Identifier& property, float& destination)
    {
        if (!object->hasProperty(property))
            return true;
        const auto levelValue = object->getProperty(property);
        if ((!levelValue.isDouble() && !levelValue.isInt() && !levelValue.isInt64())
            || !std::isfinite(static_cast<double>(levelValue))
            || static_cast<double>(levelValue) < 0.0
            || static_cast<double>(levelValue) > 1.0)
        {
            error = "Section click levels must be finite numbers from 0 to 1.";
            return false;
        }
        destination = static_cast<float>(static_cast<double>(levelValue));
        return true;
    };
    if (!readLevel("level", settings.level)
        || !readLevel("accentLevel", settings.accentLevel))
        return std::nullopt;
    if (object->hasProperty("accentBeats"))
    {
        const auto accents = object->getProperty("accentBeats");
        if (!accents.isArray())
        {
            error = "Section click accent beats must be an array of integers.";
            return std::nullopt;
        }
        settings.accentBeats.clear();
        for (const auto& accent : *accents.getArray())
        {
            if ((!accent.isInt() && !accent.isInt64())
                || static_cast<juce::int64>(accent) < 1
                || static_cast<juce::int64>(accent) > 32)
            {
                error = "Section click accent beats must be integers from 1 to 32.";
                return std::nullopt;
            }
            settings.accentBeats.push_back(static_cast<int>(accent));
        }
    }
    if (!settings.validate(error))
        return std::nullopt;
    return settings;
}

juce::var ProjectMarker::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("timeSeconds", timeSeconds);
    return juce::var(object.release());
}

std::optional<ProjectMarker> ProjectMarker::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Project marker");
    if (object == nullptr)
        return std::nullopt;

    ProjectMarker marker;
    marker.id = object->getProperty("id").toString();
    marker.name = object->getProperty("name").toString().trim();
    marker.timeSeconds = numberProperty(*object, "timeSeconds", 0.0);
    if (marker.id.trim().isEmpty()
        || marker.name.isEmpty()
        || !std::isfinite(marker.timeSeconds)
        || marker.timeSeconds < 0.0)
    {
        error = "Project markers require an ID, a name, and a finite non-negative position.";
        return std::nullopt;
    }
    return marker;
}

juce::var SongSection::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("timeSeconds", timeSeconds);
    if (clickSettings.has_value())
        object->setProperty("clickSettings", clickSettings->toVar());
    if (endTimeSeconds.has_value())
        object->setProperty("endTimeSeconds", *endTimeSeconds);
    return juce::var(object.release());
}

std::optional<SongSection> SongSection::fromVar(const juce::var& value,
                                                juce::String& error)
{
    const auto* object = requireObject(value, error, "Song section");
    if (object == nullptr)
        return std::nullopt;

    SongSection section;
    section.id = object->getProperty("id").toString();
    section.name = object->getProperty("name").toString().trim();
    section.timeSeconds = numberProperty(*object, "timeSeconds", 0.0);
    if (object->hasProperty("clickSettings"))
    {
        section.clickSettings = SectionClickSettings::fromVar(
            object->getProperty("clickSettings"), error);
        if (!section.clickSettings.has_value())
            return std::nullopt;
    }
    if (object->hasProperty("endTimeSeconds"))
        section.endTimeSeconds = numberProperty(
            *object,
            "endTimeSeconds",
            section.timeSeconds);
    if (section.id.trim().isEmpty()
        || section.name.isEmpty()
        || !std::isfinite(section.timeSeconds)
        || section.timeSeconds < 0.0
        || (section.endTimeSeconds.has_value()
            && (!std::isfinite(*section.endTimeSeconds)
                || *section.endTimeSeconds
                    <= section.timeSeconds)))
    {
        error = "Song sections require an ID, a name, and a finite non-negative position.";
        return std::nullopt;
    }
    return section;
}

juce::var EditGroup::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    juce::Array<juce::var> trackValues;
    for (const auto& trackId : trackIds)
        trackValues.add(trackId);
    object->setProperty("trackIds", juce::var(trackValues));
    object->setProperty("timingReferenceTrackId", timingReferenceTrackId);
    object->setProperty("quantizeStrength", quantizeStrength);
    juce::Array<juce::var> anchorValues;
    for (const auto anchor : protectedAnchorsSeconds)
        anchorValues.add(anchor);
    object->setProperty("protectedAnchorsSeconds", juce::var(anchorValues));
    object->setProperty("enabled", enabled);
    return juce::var(object.release());
}

std::optional<EditGroup> EditGroup::fromVar(const juce::var& value,
                                            juce::String& error)
{
    const auto* object = requireObject(value, error, "Edit group");
    if (object == nullptr)
        return std::nullopt;

    EditGroup group;
    group.id = object->getProperty("id").toString();
    group.name = object->getProperty("name").toString();
    const auto trackValues = object->getProperty("trackIds");
    if (!trackValues.isArray())
    {
        error = "Edit group track IDs must be an array.";
        return std::nullopt;
    }
    for (const auto& trackValue : *trackValues.getArray())
        group.trackIds.push_back(trackValue.toString());
    group.timingReferenceTrackId
        = object->getProperty("timingReferenceTrackId").toString();
    group.quantizeStrength = numberProperty(*object, "quantizeStrength", 1.0);
    const auto anchorValues = object->getProperty("protectedAnchorsSeconds");
    if (anchorValues.isArray())
        for (const auto& anchorValue : *anchorValues.getArray())
            group.protectedAnchorsSeconds.push_back(static_cast<double>(anchorValue));
    group.enabled = booleanProperty(*object, "enabled", true);

    if (group.id.isEmpty()
        || group.name.trim().isEmpty()
        || group.trackIds.size() < 2
        || group.quantizeStrength < 0.0
        || group.quantizeStrength > 1.0)
    {
        error = "Edit groups require an ID, name, tracks, and valid strength.";
        return std::nullopt;
    }
    return group;
}

juce::var ReampRoute::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("type", tonePathTypeToString(type));
    object->setProperty("sourceTrackId", sourceTrackId);
    object->setProperty("returnTrackId", returnTrackId);
    object->setProperty("outputChannel", outputChannel);
    object->setProperty("inputChannel", inputChannel);
    object->setProperty("latencySamples", latencySamples);
    object->setProperty("alignmentOffsetSamples", alignmentOffsetSamples);
    object->setProperty("polarityInverted", polarityInverted);
    object->setProperty("enabled", enabled);
    object->setProperty("ownsReturnTrack", ownsReturnTrack);
    object->setProperty("activeSnapshotId", activeSnapshotId);
    return juce::var(object.release());
}

std::optional<ReampRoute> ReampRoute::fromVar(const juce::var& value,
                                              juce::String& error)
{
    const auto* object = requireObject(value, error, "Reamp route");
    if (object == nullptr)
        return std::nullopt;

    ReampRoute route;
    route.id = object->getProperty("id").toString();
    route.name = object->getProperty("name").toString();
    const auto type = tonePathTypeFromString(object->getProperty("type").toString());
    if (!type.has_value())
    {
        error = "Reamp route contains an unsupported tone-path type.";
        return std::nullopt;
    }
    route.type = *type;
    route.sourceTrackId = object->getProperty("sourceTrackId").toString();
    route.returnTrackId = object->getProperty("returnTrackId").toString();
    route.outputChannel = std::max(0,
                                   integerProperty(*object, "outputChannel", 2));
    route.inputChannel = std::max(0,
                                  integerProperty(*object, "inputChannel", 0));
    route.latencySamples = std::max(0,
                                    integerProperty(*object, "latencySamples", 0));
    route.alignmentOffsetSamples
        = integerProperty(*object, "alignmentOffsetSamples", 0);
    route.polarityInverted = booleanProperty(*object, "polarityInverted", false);
    route.enabled = booleanProperty(*object, "enabled", true);
    route.ownsReturnTrack = booleanProperty(*object, "ownsReturnTrack", false);
    route.activeSnapshotId =
        object->getProperty("activeSnapshotId").toString();
    if (route.id.isEmpty()
        || route.name.trim().isEmpty()
        || route.sourceTrackId.isEmpty()
        || route.returnTrackId.isEmpty()
        || route.sourceTrackId == route.returnTrackId)
    {
        error = "Reamp routes require distinct source and return tracks.";
        return std::nullopt;
    }
    return route;
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
    object->setProperty("architecture", architecture);
    object->setProperty("fileOrIdentifier", fileOrIdentifier);
    object->setProperty("stateFile", stateFile);
    object->setProperty("stateHash", stateHash);
    object->setProperty(
        "stateFormat",
        pluginStateFormatToString(stateFormat));
    object->setProperty("bridgeMode", pluginBridgeModeToString(bridgeMode));
    object->setProperty("latencySamples", latencySamples);
    object->setProperty("tailSeconds", tailSeconds);
    object->setProperty("bypassed", bypassed);
    object->setProperty("missing", missing);
    object->setProperty("bundledDevice", bundledDevice);
    object->setProperty("araCapable", araCapable);
    object->setProperty("recoveryDisabled", recoveryDisabled);
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
    insert.architecture = object->getProperty("architecture").toString();
    insert.fileOrIdentifier = object->getProperty("fileOrIdentifier").toString();
    insert.stateFile = object->getProperty("stateFile").toString();
    insert.stateHash = object->getProperty("stateHash").toString();
    const auto stateFormat = pluginStateFormatFromString(
        object->getProperty("stateFormat").toString());
    if (!stateFormat.has_value())
    {
        error = "Plugin insert contains an unsupported state format.";
        return std::nullopt;
    }
    insert.stateFormat = *stateFormat;
    insert.latencySamples = juce::jmax(0, integerProperty(*object, "latencySamples", 0));
    insert.tailSeconds = std::max(0.0, numberProperty(*object, "tailSeconds", 0.0));
    insert.bypassed = booleanProperty(*object, "bypassed", false);
    insert.missing = booleanProperty(*object, "missing", false);
    insert.bundledDevice = booleanProperty(
        *object,
        "bundledDevice",
        false);
    insert.araCapable = booleanProperty(*object, "araCapable", false);
    insert.recoveryDisabled = booleanProperty(
        *object,
        "recoveryDisabled",
        false);

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

double AudioClip::sourceSecondsAt(double timelineOffsetSeconds) const noexcept
{
    const auto timelinePosition = juce::jlimit(0.0,
                                               durationSeconds,
                                               timelineOffsetSeconds);
    const auto sourceStart = sourceOffsetSeconds;
    const auto sourceEnd = std::min(sourceRangeEnd(),
                                    sourceOffsetSeconds
                                        + durationSeconds
                                            * std::max(0.01, playbackRate));

    auto previousTimeline = 0.0;
    auto previousSource = sourceStart;
    auto mapped = sourceStart;
    auto mappedWithinSegment = false;
    for (const auto& marker : warpMarkers)
    {
        if (marker.timelineOffsetSeconds <= previousTimeline
            || marker.timelineOffsetSeconds >= durationSeconds)
            continue;
        const auto markerSource = juce::jlimit(sourceStart,
                                               sourceEnd,
                                               marker.sourceSeconds);
        if (timelinePosition <= marker.timelineOffsetSeconds)
        {
            const auto progress = (timelinePosition - previousTimeline)
                / (marker.timelineOffsetSeconds - previousTimeline);
            mapped = previousSource + (markerSource - previousSource) * progress;
            mappedWithinSegment = true;
            break;
        }
        previousTimeline = marker.timelineOffsetSeconds;
        previousSource = markerSource;
    }
    if (!mappedWithinSegment && timelinePosition > previousTimeline)
    {
        const auto remaining = durationSeconds - previousTimeline;
        const auto progress = remaining > 0.0
            ? (timelinePosition - previousTimeline) / remaining
            : 0.0;
        mapped = previousSource + (sourceEnd - previousSource) * progress;
    }

    mapped = juce::jlimit(sourceStart, sourceEnd, mapped);
    return reversed ? sourceStart + sourceEnd - mapped : mapped;
}

double AudioClip::timelineOffsetForSourceSeconds(double sourceSeconds) const noexcept
{
    const auto target = juce::jlimit(sourceOffsetSeconds,
                                     sourceRangeEnd(),
                                     sourceSeconds);
    auto lower = 0.0;
    auto upper = durationSeconds;
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        const auto middle = (lower + upper) * 0.5;
        const auto mapped = sourceSecondsAt(middle);
        if ((!reversed && mapped < target) || (reversed && mapped > target))
            lower = middle;
        else
            upper = middle;
    }
    return (lower + upper) * 0.5;
}

float AudioClip::envelopeGainAt(double timelineOffsetSeconds) const noexcept
{
    const auto shaped = [](double progress, float curve)
    {
        progress = juce::jlimit(0.0, 1.0, progress);
        if (curve > 0.001f)
            return std::pow(progress, 1.0 + curve * 3.0);
        if (curve < -0.001f)
            return 1.0
                - std::pow(1.0 - progress, 1.0 - curve * 3.0);
        return progress;
    };
    const auto position = juce::jlimit(0.0,
                                       durationSeconds,
                                       timelineOffsetSeconds);
    auto gain = 1.0;
    if (fadeInSeconds > 0.0)
        gain = std::min(
            gain,
            shaped(position / fadeInSeconds, fadeInCurve));
    if (fadeOutSeconds > 0.0)
        gain = std::min(
            gain,
            shaped(
                (durationSeconds - position) / fadeOutSeconds,
                fadeOutCurve));
    return static_cast<float>(juce::jlimit(0.0, 1.0, gain));
}

juce::var AudioClip::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("sourceFile", sourceFile.getFullPathName());
    object->setProperty("sourceHash", sourceHash);
    object->setProperty("startSeconds", startSeconds);
    object->setProperty("sourceOffsetSeconds", sourceOffsetSeconds);
    object->setProperty("sourceLengthSeconds", sourceLengthSeconds);
    object->setProperty("sourceRangeStartSeconds", sourceRangeStartSeconds);
    object->setProperty("sourceRangeEndSeconds", sourceRangeEnd());
    object->setProperty("durationSeconds", durationSeconds);
    object->setProperty("stretchMode", stretchModeToString(stretchMode));
    object->setProperty("playbackRate", playbackRate);
    object->setProperty("fadeInSeconds", fadeInSeconds);
    object->setProperty("fadeOutSeconds", fadeOutSeconds);
    object->setProperty("fadeInCurve", fadeInCurve);
    object->setProperty("fadeOutCurve", fadeOutCurve);
    object->setProperty("polarityInverted", polarityInverted);
    object->setProperty("reversed", reversed);
    juce::Array<juce::var> warpValues;
    for (const auto& marker : warpMarkers)
        warpValues.add(marker.toVar());
    object->setProperty("warpMarkers", juce::var(warpValues));
    juce::Array<juce::var> transientValues;
    for (const auto transient : transientSourceSeconds)
        transientValues.add(transient);
    object->setProperty("transientSourceSeconds", juce::var(transientValues));
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
    clip.sourceHash = object->getProperty("sourceHash").toString();
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
    const auto stretchMode = stretchModeFromString(
        object->getProperty("stretchMode").toString());
    if (!stretchMode.has_value())
    {
        error = "Clip contains an unsupported stretch mode.";
        return std::nullopt;
    }
    clip.stretchMode = *stretchMode;
    clip.playbackRate = juce::jlimit(
        0.25,
        4.0,
        numberProperty(*object, "playbackRate", 1.0));
    clip.fadeInSeconds = std::max(0.0,
                                  numberProperty(*object, "fadeInSeconds", 0.0));
    clip.fadeOutSeconds = std::max(0.0,
                                   numberProperty(*object, "fadeOutSeconds", 0.0));
    clip.fadeInCurve = juce::jlimit(
        -1.0f,
        1.0f,
        static_cast<float>(
            numberProperty(*object, "fadeInCurve", 0.0)));
    clip.fadeOutCurve = juce::jlimit(
        -1.0f,
        1.0f,
        static_cast<float>(
            numberProperty(*object, "fadeOutCurve", 0.0)));
    clip.polarityInverted = booleanProperty(*object, "polarityInverted", false);
    clip.reversed = booleanProperty(*object, "reversed", false);
    const auto warpValues = object->getProperty("warpMarkers");
    if (warpValues.isArray())
    {
        for (const auto& warpValue : *warpValues.getArray())
        {
            auto marker = WarpMarker::fromVar(warpValue, error);
            if (!marker.has_value())
                return std::nullopt;
            clip.warpMarkers.push_back(*marker);
        }
        std::stable_sort(clip.warpMarkers.begin(),
                         clip.warpMarkers.end(),
                         [](const auto& left, const auto& right)
                         {
                             return left.timelineOffsetSeconds
                                 < right.timelineOffsetSeconds;
                         });
    }
    const auto transientValues = object->getProperty("transientSourceSeconds");
    if (transientValues.isArray())
        for (const auto& transient : *transientValues.getArray())
            clip.transientSourceSeconds.push_back(static_cast<double>(transient));
    clip.gainDecibels = static_cast<float>(numberProperty(*object, "gainDecibels", 0.0));
    clip.muted = booleanProperty(*object, "muted", false);
    clip.colour = colourProperty(*object, "colour", juce::Colour(0xffdd5b3f));

    if (clip.id.isEmpty() || clip.durationSeconds <= 0.0 || clip.startSeconds < 0.0
        || !validSha256(clip.sourceHash)
        || clip.sourceOffsetSeconds < 0.0
        || clip.sourceRangeStartSeconds < 0.0
        || clip.sourceRangeStartSeconds > clip.sourceOffsetSeconds + 0.0001
        || clip.sourceRangeStartSeconds >= clip.sourceRangeEnd() - 0.0001
        || clip.sourceRangeEnd() > clip.sourceLengthSeconds + 0.0001
        || clip.fadeInSeconds > clip.durationSeconds
        || clip.fadeOutSeconds > clip.durationSeconds
        || std::any_of(clip.transientSourceSeconds.cbegin(),
                       clip.transientSourceSeconds.cend(),
                       [&clip](double transient)
                       {
                           return transient < clip.sourceRangeStartSeconds
                               || transient > clip.sourceRangeEnd();
                       }))
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
    object->setProperty("automationMode",
                        automationModeToString(automationMode));
    object->setProperty("automationArmed", automationArmed);
    object->setProperty("type", trackTypeToString(type));
    object->setProperty("outputTrackId", outputTrackId);
    object->setProperty("folderTrackId", folderTrackId);
    juce::Array<juce::var> controlledTrackValues;
    for (const auto& trackId : controlledTrackIds)
        controlledTrackValues.add(trackId);
    object->setProperty("controlledTrackIds", juce::var(controlledTrackValues));
    object->setProperty("channelLayout", channelLayoutToString(channelLayout));
    object->setProperty("volumeDecibels", volumeDecibels);
    object->setProperty("pan", pan);
    object->setProperty("polarityInverted", polarityInverted);
    object->setProperty("muted", muted);
    object->setProperty("solo", solo);
    object->setProperty("soloSafe", soloSafe);
    object->setProperty("armed", armed);
    object->setProperty("inputChannel", inputChannel);
    object->setProperty("stereoInput", stereoInput);
    object->setProperty("inputMonitoring", inputMonitoring);
    object->setProperty("hardwareOutputChannel", hardwareOutputChannel);
    object->setProperty("controlRoomDimDecibels", controlRoomDimDecibels);
    object->setProperty("controlRoomDimmed", controlRoomDimmed);
    object->setProperty("controlRoomMono", controlRoomMono);
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

    juce::Array<juce::var> midiClipValues;
    midiClipValues.ensureStorageAllocated(static_cast<int>(midiClips.size()));
    for (const auto& clip : midiClips)
        midiClipValues.add(clip.toVar());
    object->setProperty("midiClips", juce::var(midiClipValues));

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
    const auto automationMode = automationModeFromString(
        object->getProperty("automationMode").toString());
    if (object->hasProperty("automationMode")
        && !automationMode.has_value())
    {
        error = "Track contains an unsupported automation mode.";
        return std::nullopt;
    }
    track.automationMode = automationMode.value_or(AutomationMode::read);
    track.automationArmed = booleanProperty(
        *object,
        "automationArmed",
        false);

    const auto type = trackTypeFromString(object->getProperty("type").toString());
    if (!type.has_value())
    {
        error = "Track contains an unsupported type.";
        return std::nullopt;
    }

    track.type = *type;
    track.outputTrackId = object->getProperty("outputTrackId").toString();
    track.folderTrackId = object->getProperty("folderTrackId").toString();
    const auto controlledTrackValues = object->getProperty("controlledTrackIds");
    if (controlledTrackValues.isArray())
        for (const auto& controlledTrackValue : *controlledTrackValues.getArray())
            track.controlledTrackIds.push_back(controlledTrackValue.toString());
    const auto channelLayout = channelLayoutFromString(
        object->getProperty("channelLayout").toString());
    if (object->hasProperty("channelLayout") && !channelLayout.has_value())
    {
        error = "Track contains an unsupported channel layout.";
        return std::nullopt;
    }
    track.channelLayout = channelLayout.value_or(ChannelLayout::stereo);
    track.volumeDecibels = static_cast<float>(numberProperty(*object, "volumeDecibels", 0.0));
    track.pan = juce::jlimit(-1.0f, 1.0f, static_cast<float>(numberProperty(*object, "pan", 0.0)));
    track.polarityInverted = booleanProperty(*object, "polarityInverted", false);
    track.muted = booleanProperty(*object, "muted", false);
    track.solo = booleanProperty(*object, "solo", false);
    track.soloSafe = booleanProperty(*object, "soloSafe", false);
    track.armed = booleanProperty(*object, "armed", false);
    track.inputChannel = juce::jmax(0, integerProperty(*object, "inputChannel", 0));
    track.stereoInput = booleanProperty(*object, "stereoInput", false);
    track.inputMonitoring = booleanProperty(*object, "inputMonitoring", false);
    track.hardwareOutputChannel = std::max(
        0,
        integerProperty(*object, "hardwareOutputChannel", 0));
    track.controlRoomDimDecibels = static_cast<float>(
        numberProperty(*object, "controlRoomDimDecibels", -20.0));
    track.controlRoomDimmed = booleanProperty(
        *object,
        "controlRoomDimmed",
        false);
    track.controlRoomMono = booleanProperty(
        *object,
        "controlRoomMono",
        false);
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

    const auto midiClipValues = object->getProperty("midiClips");
    if (!midiClipValues.isArray())
    {
        error = "Track MIDI clips must be a JSON array.";
        return std::nullopt;
    }
    for (const auto& midiClipValue : *midiClipValues.getArray())
    {
        auto clip = MidiClip::fromVar(midiClipValue, error);
        if (!clip.has_value())
            return std::nullopt;
        track.midiClips.push_back(std::move(*clip));
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

juce::var ProjectMetadata::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("artist", artist);
    object->setProperty("album", album);
    object->setProperty("originalArtist", originalArtist);
    object->setProperty("composer", composer);
    object->setProperty("songwriter", songwriter);
    object->setProperty("producer", producer);
    object->setProperty("arranger", arranger);
    object->setProperty("year", year);
    object->setProperty("genre", genre);
    object->setProperty("copyright", copyright);
    object->setProperty("website", website);
    object->setProperty("comment", comment);
    return juce::var(object.release());
}

std::optional<ProjectMetadata> ProjectMetadata::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Project metadata");
    if (object == nullptr)
        return std::nullopt;

    ProjectMetadata metadata;
    metadata.artist = object->getProperty("artist").toString();
    metadata.album = object->getProperty("album").toString();
    metadata.originalArtist =
        object->getProperty("originalArtist").toString();
    metadata.composer = object->getProperty("composer").toString();
    metadata.songwriter = object->getProperty("songwriter").toString();
    metadata.producer = object->getProperty("producer").toString();
    metadata.arranger = object->getProperty("arranger").toString();
    metadata.year = object->getProperty("year").toString();
    metadata.genre = object->getProperty("genre").toString();
    metadata.copyright = object->getProperty("copyright").toString();
    metadata.website = object->getProperty("website").toString();
    metadata.comment = object->getProperty("comment").toString();
    return metadata;
}

juce::var SceneSlot::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("trackId", trackId);
    object->setProperty("stopTrack", stopTrack);
    if (audioClip.has_value())
        object->setProperty("audioClip", audioClip->toVar());
    if (midiClip.has_value())
        object->setProperty("midiClip", midiClip->toVar());
    return juce::var(object.release());
}

std::optional<SceneSlot> SceneSlot::fromVar(const juce::var& value,
                                            juce::String& error)
{
    const auto* object = requireObject(value, error, "Scene slot");
    if (object == nullptr)
        return std::nullopt;

    SceneSlot slot;
    slot.id = object->getProperty("id").toString();
    slot.trackId = object->getProperty("trackId").toString();
    slot.stopTrack = booleanProperty(*object, "stopTrack", false);
    const auto audioValue = object->getProperty("audioClip");
    if (!audioValue.isVoid())
    {
        auto clip = AudioClip::fromVar(audioValue, error);
        if (!clip.has_value())
            return std::nullopt;
        slot.audioClip = std::move(*clip);
    }
    const auto midiValue = object->getProperty("midiClip");
    if (!midiValue.isVoid())
    {
        auto clip = MidiClip::fromVar(midiValue, error);
        if (!clip.has_value())
            return std::nullopt;
        slot.midiClip = std::move(*clip);
    }
    if (slot.id.isEmpty()
        || slot.trackId.isEmpty()
        || (slot.audioClip.has_value() && slot.midiClip.has_value()))
    {
        error =
            "Scene slots require IDs and tracks and may contain only one clip type.";
        return std::nullopt;
    }
    return slot;
}

juce::var ProjectScene::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("colour", colour.toString());
    juce::Array<juce::var> slotValues;
    slotValues.ensureStorageAllocated(static_cast<int>(slots.size()));
    for (const auto& slot : slots)
        slotValues.add(slot.toVar());
    object->setProperty("slots", juce::var(slotValues));
    return juce::var(object.release());
}

std::optional<ProjectScene> ProjectScene::fromVar(const juce::var& value,
                                                  juce::String& error)
{
    const auto* object = requireObject(value, error, "Scene");
    if (object == nullptr)
        return std::nullopt;

    ProjectScene scene;
    scene.id = object->getProperty("id").toString();
    scene.name = object->getProperty("name").toString().trim();
    scene.colour = colourProperty(
        *object,
        "colour",
        juce::Colour(0xff5d7fa3));
    const auto slotValues = object->getProperty("slots");
    if (!slotValues.isArray())
    {
        error = "Scene slots must be a JSON array.";
        return std::nullopt;
    }
    std::vector<juce::String> slotIds;
    std::vector<juce::String> trackIds;
    for (const auto& slotValue : *slotValues.getArray())
    {
        auto slot = SceneSlot::fromVar(slotValue, error);
        if (!slot.has_value())
            return std::nullopt;
        if (std::find(slotIds.cbegin(), slotIds.cend(), slot->id)
                != slotIds.cend()
            || std::find(trackIds.cbegin(), trackIds.cend(), slot->trackId)
                != trackIds.cend())
        {
            error = "Scene slots require unique IDs and tracks.";
            return std::nullopt;
        }
        slotIds.push_back(slot->id);
        trackIds.push_back(slot->trackId);
        scene.slots.push_back(std::move(*slot));
    }
    if (scene.id.isEmpty() || scene.name.isEmpty())
    {
        error = "Scenes require an ID and name.";
        return std::nullopt;
    }
    return scene;
}

juce::var CompatibilityIssue::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("severity", compatibilitySeverityToString(severity));
    object->setProperty("code", code);
    object->setProperty("objectPath", objectPath);
    object->setProperty("message", message);
    return juce::var(object.release());
}

std::optional<CompatibilityIssue> CompatibilityIssue::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Compatibility issue");
    if (object == nullptr)
        return std::nullopt;

    CompatibilityIssue issue;
    const auto severity = compatibilitySeverityFromString(
        object->getProperty("severity").toString());
    if (!severity.has_value())
    {
        error = "Compatibility issue contains an unsupported severity.";
        return std::nullopt;
    }
    issue.severity = *severity;
    issue.code = object->getProperty("code").toString().trim();
    issue.objectPath = object->getProperty("objectPath").toString().trim();
    issue.message = object->getProperty("message").toString().trim();
    if (issue.code.isEmpty()
        || issue.objectPath.isEmpty()
        || issue.message.isEmpty())
    {
        error =
            "Compatibility issues require a code, object path, and message.";
        return std::nullopt;
    }
    return issue;
}

bool CompatibilityReport::hasErrors() const noexcept
{
    return std::any_of(
        issues.cbegin(),
        issues.cend(),
        [](const auto& issue)
        {
            return issue.severity == CompatibilitySeverity::error;
        });
}

juce::String CompatibilityReport::toText() const
{
    juce::String text;
    text << format << " " << operation << " compatibility report\n";
    text << "Source: " << (source.isNotEmpty() ? source : "(none)") << "\n";
    text << "Destination: "
         << (destination.isNotEmpty() ? destination : "(none)") << "\n";
    text << "Created: " << (createdAt.isNotEmpty() ? createdAt : "(unknown)")
         << "\n";
    text << "Issues: " << juce::String(static_cast<int>(issues.size()))
         << "\n";
    for (const auto& issue : issues)
    {
        text << "\n[" << compatibilitySeverityToString(issue.severity)
             << "] " << issue.code << "\n";
        text << issue.objectPath << "\n";
        text << issue.message << "\n";
    }
    return text;
}

juce::var CompatibilityReport::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("format", format);
    object->setProperty("operation", operation);
    object->setProperty("source", source);
    object->setProperty("destination", destination);
    object->setProperty("createdAt", createdAt);
    juce::Array<juce::var> issueValues;
    issueValues.ensureStorageAllocated(static_cast<int>(issues.size()));
    for (const auto& issue : issues)
        issueValues.add(issue.toVar());
    object->setProperty("issues", juce::var(issueValues));
    return juce::var(object.release());
}

std::optional<CompatibilityReport> CompatibilityReport::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Compatibility report");
    if (object == nullptr)
        return std::nullopt;

    CompatibilityReport report;
    report.id = object->getProperty("id").toString();
    report.format = object->getProperty("format").toString().trim();
    report.operation = object->getProperty("operation").toString().trim();
    report.source = object->getProperty("source").toString();
    report.destination = object->getProperty("destination").toString();
    report.createdAt = object->getProperty("createdAt").toString();
    const auto issueValues = object->getProperty("issues");
    if (!issueValues.isArray())
    {
        error = "Compatibility report issues must be a JSON array.";
        return std::nullopt;
    }
    for (const auto& issueValue : *issueValues.getArray())
    {
        auto issue = CompatibilityIssue::fromVar(issueValue, error);
        if (!issue.has_value())
            return std::nullopt;
        report.issues.push_back(std::move(*issue));
    }
    if (report.id.isEmpty()
        || report.format.isEmpty()
        || report.operation.isEmpty())
    {
        error = "Compatibility reports require an ID, format, and operation.";
        return std::nullopt;
    }
    return report;
}

juce::var ToneSnapshot::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("reampRouteId", reampRouteId);
    object->setProperty("sourceTrackId", sourceTrackId);
    object->setProperty("returnTrackId", returnTrackId);
    object->setProperty("returnVolumeDecibels", returnVolumeDecibels);
    object->setProperty("returnPan", returnPan);
    object->setProperty("returnPolarityInverted", returnPolarityInverted);
    object->setProperty("sourceFingerprint", sourceFingerprint);
    object->setProperty("chainFingerprint", chainFingerprint);
    object->setProperty("renderFile", renderFile);
    object->setProperty("renderHash", renderHash);
    object->setProperty("frozenTrackId", frozenTrackId);
    object->setProperty("comparisonGainDecibels", comparisonGainDecibels);
    object->setProperty("frozen", frozen);
    juce::Array<juce::var> insertValues;
    for (const auto& insert : inserts)
        insertValues.add(insert.toVar());
    object->setProperty("inserts", juce::var(insertValues));
    juce::Array<juce::var> routeValues;
    for (const auto& route : routes)
        routeValues.add(route.toVar());
    object->setProperty("routes", juce::var(routeValues));
    juce::Array<juce::var> automationValues;
    for (const auto& lane : automation)
        automationValues.add(lane.toVar());
    object->setProperty("automation", juce::var(automationValues));
    return juce::var(object.release());
}

std::optional<ToneSnapshot> ToneSnapshot::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Tone snapshot");
    if (object == nullptr)
        return std::nullopt;
    ToneSnapshot snapshot;
    snapshot.id = object->getProperty("id").toString();
    snapshot.name = object->getProperty("name").toString();
    snapshot.reampRouteId = object->getProperty("reampRouteId").toString();
    snapshot.sourceTrackId = object->getProperty("sourceTrackId").toString();
    snapshot.returnTrackId = object->getProperty("returnTrackId").toString();
    snapshot.returnVolumeDecibels = static_cast<float>(
        numberProperty(*object, "returnVolumeDecibels", 0.0));
    snapshot.returnPan = static_cast<float>(
        numberProperty(*object, "returnPan", 0.0));
    snapshot.returnPolarityInverted = booleanProperty(
        *object,
        "returnPolarityInverted",
        false);
    snapshot.sourceFingerprint =
        object->getProperty("sourceFingerprint").toString();
    snapshot.chainFingerprint =
        object->getProperty("chainFingerprint").toString();
    snapshot.renderFile = object->getProperty("renderFile").toString();
    snapshot.renderHash = object->getProperty("renderHash").toString();
    snapshot.frozenTrackId =
        object->getProperty("frozenTrackId").toString();
    snapshot.comparisonGainDecibels = static_cast<float>(
        numberProperty(*object, "comparisonGainDecibels", 0.0));
    snapshot.frozen = booleanProperty(*object, "frozen", false);

    const auto insertValues = object->getProperty("inserts");
    const auto routeValues = object->getProperty("routes");
    const auto automationValues = object->getProperty("automation");
    if (!insertValues.isArray()
        || !routeValues.isArray()
        || !automationValues.isArray())
    {
        error = "Tone snapshot collections must be arrays.";
        return std::nullopt;
    }
    for (const auto& insertValue : *insertValues.getArray())
    {
        auto insert = PluginInsert::fromVar(insertValue, error);
        if (!insert.has_value())
            return std::nullopt;
        snapshot.inserts.push_back(std::move(*insert));
    }
    for (const auto& routeValue : *routeValues.getArray())
    {
        auto route = RoutingConnection::fromVar(routeValue, error);
        if (!route.has_value())
            return std::nullopt;
        snapshot.routes.push_back(std::move(*route));
    }
    for (const auto& automationValue : *automationValues.getArray())
    {
        auto lane = AutomationLane::fromVar(automationValue, error);
        if (!lane.has_value())
            return std::nullopt;
        snapshot.automation.push_back(std::move(*lane));
    }
    if (snapshot.id.isEmpty()
        || snapshot.name.trim().isEmpty()
        || snapshot.reampRouteId.isEmpty()
        || snapshot.sourceTrackId.isEmpty()
        || snapshot.returnTrackId.isEmpty()
        || snapshot.sourceFingerprint.isEmpty()
        || snapshot.chainFingerprint.isEmpty())
    {
        error = "Tone snapshot identity and fingerprints cannot be empty.";
        return std::nullopt;
    }
    return snapshot;
}

juce::var MixerTrackSnapshot::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("trackId", trackId);
    object->setProperty("volumeDecibels", volumeDecibels);
    object->setProperty("pan", pan);
    object->setProperty("muted", muted);
    object->setProperty("solo", solo);
    object->setProperty("soloSafe", soloSafe);
    object->setProperty("polarityInverted", polarityInverted);
    object->setProperty("channelLayout", channelLayoutToString(channelLayout));
    object->setProperty("folderTrackId", folderTrackId);
    juce::Array<juce::var> controlledValues;
    for (const auto& id : controlledTrackIds)
        controlledValues.add(id);
    object->setProperty("controlledTrackIds", juce::var(controlledValues));
    juce::Array<juce::var> insertValues;
    for (const auto& insert : inserts)
        insertValues.add(insert.toVar());
    object->setProperty("inserts", juce::var(insertValues));
    return juce::var(object.release());
}

std::optional<MixerTrackSnapshot> MixerTrackSnapshot::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Mixer track snapshot");
    if (object == nullptr)
        return std::nullopt;
    const auto layout = channelLayoutFromString(
        object->getProperty("channelLayout").toString());
    if (!layout.has_value())
    {
        error = "Mixer snapshot contains an unsupported channel layout.";
        return std::nullopt;
    }
    MixerTrackSnapshot snapshot;
    snapshot.trackId = object->getProperty("trackId").toString();
    snapshot.volumeDecibels = static_cast<float>(
        numberProperty(*object, "volumeDecibels", 0.0));
    snapshot.pan = static_cast<float>(numberProperty(*object, "pan", 0.0));
    snapshot.muted = booleanProperty(*object, "muted", false);
    snapshot.solo = booleanProperty(*object, "solo", false);
    snapshot.soloSafe = booleanProperty(*object, "soloSafe", false);
    snapshot.polarityInverted = booleanProperty(
        *object,
        "polarityInverted",
        false);
    snapshot.channelLayout = *layout;
    snapshot.folderTrackId =
        object->getProperty("folderTrackId").toString();
    const auto controlledValues = object->getProperty("controlledTrackIds");
    const auto insertValues = object->getProperty("inserts");
    if (!controlledValues.isArray() || !insertValues.isArray())
    {
        error = "Mixer snapshot collections must be arrays.";
        return std::nullopt;
    }
    for (const auto& controlled : *controlledValues.getArray())
        snapshot.controlledTrackIds.push_back(controlled.toString());
    for (const auto& insertValue : *insertValues.getArray())
    {
        auto insert = PluginInsert::fromVar(insertValue, error);
        if (!insert.has_value())
            return std::nullopt;
        snapshot.inserts.push_back(std::move(*insert));
    }
    if (snapshot.trackId.isEmpty())
    {
        error = "Mixer track snapshot requires a track ID.";
        return std::nullopt;
    }
    return snapshot;
}

juce::var MixerSnapshot::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    juce::Array<juce::var> trackValues;
    for (const auto& track : tracks)
        trackValues.add(track.toVar());
    object->setProperty("tracks", juce::var(trackValues));
    juce::Array<juce::var> routeValues;
    for (const auto& route : routes)
        routeValues.add(route.toVar());
    object->setProperty("routes", juce::var(routeValues));
    juce::Array<juce::var> automationValues;
    for (const auto& lane : automation)
        automationValues.add(lane.toVar());
    object->setProperty("automation", juce::var(automationValues));
    return juce::var(object.release());
}

std::optional<MixerSnapshot> MixerSnapshot::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Mixer snapshot");
    if (object == nullptr)
        return std::nullopt;
    MixerSnapshot snapshot;
    snapshot.id = object->getProperty("id").toString();
    snapshot.name = object->getProperty("name").toString();
    const auto trackValues = object->getProperty("tracks");
    const auto routeValues = object->getProperty("routes");
    const auto automationValues = object->getProperty("automation");
    if (!trackValues.isArray()
        || !routeValues.isArray()
        || !automationValues.isArray())
    {
        error = "Mixer snapshot collections must be arrays.";
        return std::nullopt;
    }
    for (const auto& trackValue : *trackValues.getArray())
    {
        auto track = MixerTrackSnapshot::fromVar(trackValue, error);
        if (!track.has_value())
            return std::nullopt;
        snapshot.tracks.push_back(std::move(*track));
    }
    for (const auto& routeValue : *routeValues.getArray())
    {
        auto route = RoutingConnection::fromVar(routeValue, error);
        if (!route.has_value())
            return std::nullopt;
        snapshot.routes.push_back(std::move(*route));
    }
    for (const auto& automationValue : *automationValues.getArray())
    {
        auto lane = AutomationLane::fromVar(automationValue, error);
        if (!lane.has_value())
            return std::nullopt;
        snapshot.automation.push_back(std::move(*lane));
    }
    if (snapshot.id.isEmpty()
        || snapshot.name.trim().isEmpty()
        || snapshot.tracks.empty())
    {
        error = "Mixer snapshot requires an ID, name, and tracks.";
        return std::nullopt;
    }
    return snapshot;
}

juce::var RenderReport::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("scope", scope);
    object->setProperty("outputFile", outputFile);
    object->setProperty("sourceHash", sourceHash);
    object->setProperty("chainHash", chainHash);
    object->setProperty("outputHash", outputHash);
    object->setProperty("mode", mode);
    object->setProperty("status", status);
    object->setProperty("warning", warning);
    object->setProperty("error", error);
    object->setProperty("durationSeconds", durationSeconds);
    object->setProperty("createdAt", createdAt);
    object->setProperty("format", format);
    object->setProperty("sampleRate", sampleRate);
    object->setProperty("bitDepth", bitDepth);
    if (integratedLoudnessLufs.has_value())
        object->setProperty(
            "integratedLoudnessLufs",
            *integratedLoudnessLufs);
    if (loudnessRangeLu.has_value())
        object->setProperty("loudnessRangeLu", *loudnessRangeLu);
    if (std::isfinite(truePeakDbtp))
        object->setProperty("truePeakDbtp", truePeakDbtp);
    if (std::isfinite(samplePeakDbfs))
        object->setProperty("samplePeakDbfs", samplePeakDbfs);
    object->setProperty("correlation", correlation);
    object->setProperty("settingsHash", settingsHash);
    object->setProperty("signingPublicKey", signingPublicKey);
    object->setProperty("signature", signature);
    return juce::var(object.release());
}

std::optional<RenderReport> RenderReport::fromVar(
    const juce::var& value,
    juce::String& parseError)
{
    const auto* object = requireObject(value, parseError, "Render report");
    if (object == nullptr)
        return std::nullopt;
    RenderReport report;
    report.id = object->getProperty("id").toString();
    report.scope = object->getProperty("scope").toString();
    report.outputFile = object->getProperty("outputFile").toString();
    report.sourceHash = object->getProperty("sourceHash").toString();
    report.chainHash = object->getProperty("chainHash").toString();
    report.outputHash = object->getProperty("outputHash").toString();
    report.mode = object->getProperty("mode").toString();
    report.status = object->getProperty("status").toString();
    report.warning = object->getProperty("warning").toString();
    report.error = object->getProperty("error").toString();
    report.durationSeconds = numberProperty(
        *object,
        "durationSeconds",
        0.0);
    report.createdAt = object->getProperty("createdAt").toString();
    report.format = object->getProperty("format").toString();
    report.sampleRate = numberProperty(*object, "sampleRate", 0.0);
    report.bitDepth = integerProperty(*object, "bitDepth", 0);
    const auto integrated =
        object->getProperty("integratedLoudnessLufs");
    if (integrated.isDouble()
        || integrated.isInt()
        || integrated.isInt64())
        report.integratedLoudnessLufs =
            static_cast<double>(integrated);
    const auto loudnessRange =
        object->getProperty("loudnessRangeLu");
    if (loudnessRange.isDouble()
        || loudnessRange.isInt()
        || loudnessRange.isInt64())
        report.loudnessRangeLu =
            static_cast<double>(loudnessRange);
    report.truePeakDbtp = numberProperty(
        *object,
        "truePeakDbtp",
        -std::numeric_limits<double>::infinity());
    report.samplePeakDbfs = numberProperty(
        *object,
        "samplePeakDbfs",
        -std::numeric_limits<double>::infinity());
    report.correlation = numberProperty(
        *object,
        "correlation",
        1.0);
    report.settingsHash =
        object->getProperty("settingsHash").toString();
    report.signingPublicKey =
        object->getProperty("signingPublicKey").toString();
    report.signature =
        object->getProperty("signature").toString();
    if (report.id.isEmpty()
        || report.scope.isEmpty()
        || report.status.isEmpty())
    {
        parseError = "Render report requires an ID, scope, and status.";
        return std::nullopt;
    }
    return report;
}

Project Project::createDefault()
{
    Project project;

    auto drumMap = createDefaultMetalDrumMap();
    project.midiPatterns = createDefaultMetalPatterns(drumMap);
    project.midiRoutingTemplates = {
        createDefaultMetalRoutingTemplate(drumMap)
    };
    project.drumMaps.push_back(std::move(drumMap));

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
    for (std::size_t index = 0; index + 1 < project.tracks.size(); ++index)
    {
        RoutingConnection output;
        output.name = "Main output";
        output.kind = RouteKind::mainOutput;
        output.sourceTrackId = project.tracks[index].id;
        output.destination.type = RouteEndpointType::track;
        output.destination.trackId = project.tracks.back().id;
        project.routingConnections.push_back(std::move(output));
    }
    return project;
}

ProjectMarker* Project::findMarker(const juce::String& markerId)
{
    const auto marker = std::find_if(
        markers.begin(), markers.end(),
        [&markerId](const auto& candidate)
        {
            return candidate.id == markerId;
        });
    return marker != markers.end() ? &*marker : nullptr;
}

const ProjectMarker* Project::findMarker(
    const juce::String& markerId) const
{
    const auto marker = std::find_if(
        markers.cbegin(), markers.cend(),
        [&markerId](const auto& candidate)
        {
            return candidate.id == markerId;
        });
    return marker != markers.cend() ? &*marker : nullptr;
}

SongSection* Project::findSection(const juce::String& sectionId)
{
    const auto section = std::find_if(
        sections.begin(), sections.end(),
        [&sectionId](const auto& candidate)
        {
            return candidate.id == sectionId;
        });
    return section != sections.end() ? &*section : nullptr;
}

const SongSection* Project::findSection(const juce::String& sectionId) const
{
    const auto section = std::find_if(
        sections.cbegin(), sections.cend(),
        [&sectionId](const auto& candidate)
        {
            return candidate.id == sectionId;
        });
    return section != sections.cend() ? &*section : nullptr;
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

RoutingConnection* Project::findRoutingConnection(
    const juce::String& connectionId)
{
    const auto iterator = std::find_if(
        routingConnections.begin(),
        routingConnections.end(),
        [&connectionId](const auto& connection)
        {
            return connection.id == connectionId;
        });
    return iterator == routingConnections.end() ? nullptr : &*iterator;
}

const RoutingConnection* Project::findRoutingConnection(
    const juce::String& connectionId) const
{
    const auto iterator = std::find_if(
        routingConnections.cbegin(),
        routingConnections.cend(),
        [&connectionId](const auto& connection)
        {
            return connection.id == connectionId;
        });
    return iterator == routingConnections.cend() ? nullptr : &*iterator;
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

MidiClip* Project::findMidiClip(const juce::String& clipId)
{
    for (auto& track : tracks)
    {
        const auto iterator = std::find_if(
            track.midiClips.begin(),
            track.midiClips.end(),
            [&clipId](const auto& clip) { return clip.id == clipId; });
        if (iterator != track.midiClips.end())
            return &*iterator;
    }
    return nullptr;
}

const MidiClip* Project::findMidiClip(const juce::String& clipId) const
{
    for (const auto& track : tracks)
    {
        const auto iterator = std::find_if(
            track.midiClips.cbegin(),
            track.midiClips.cend(),
            [&clipId](const auto& clip) { return clip.id == clipId; });
        if (iterator != track.midiClips.cend())
            return &*iterator;
    }
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

Track* Project::findTrackContainingMidiClip(const juce::String& clipId)
{
    const auto iterator = std::find_if(
        tracks.begin(),
        tracks.end(),
        [&clipId](const auto& track)
        {
            return std::any_of(
                track.midiClips.cbegin(),
                track.midiClips.cend(),
                [&clipId](const auto& clip) { return clip.id == clipId; });
        });
    return iterator == tracks.end() ? nullptr : &*iterator;
}

const Track* Project::findTrackContainingMidiClip(
    const juce::String& clipId) const
{
    const auto iterator = std::find_if(
        tracks.cbegin(),
        tracks.cend(),
        [&clipId](const auto& track)
        {
            return std::any_of(
                track.midiClips.cbegin(),
                track.midiClips.cend(),
                [&clipId](const auto& clip) { return clip.id == clipId; });
        });
    return iterator == tracks.cend() ? nullptr : &*iterator;
}

DrumMap* Project::findDrumMap(const juce::String& drumMapId)
{
    const auto iterator = std::find_if(
        drumMaps.begin(),
        drumMaps.end(),
        [&drumMapId](const auto& map) { return map.id == drumMapId; });
    return iterator == drumMaps.end() ? nullptr : &*iterator;
}

const DrumMap* Project::findDrumMap(const juce::String& drumMapId) const
{
    const auto iterator = std::find_if(
        drumMaps.cbegin(),
        drumMaps.cend(),
        [&drumMapId](const auto& map) { return map.id == drumMapId; });
    return iterator == drumMaps.cend() ? nullptr : &*iterator;
}

const MidiPatternAlias* Project::findMidiPattern(
    const juce::String& patternId) const
{
    const auto iterator = std::find_if(
        midiPatterns.cbegin(),
        midiPatterns.cend(),
        [&patternId](const auto& pattern) { return pattern.id == patternId; });
    return iterator == midiPatterns.cend() ? nullptr : &*iterator;
}

const MidiRoutingTemplate* Project::findMidiRoutingTemplate(
    const juce::String& templateId) const
{
    const auto iterator = std::find_if(
        midiRoutingTemplates.cbegin(),
        midiRoutingTemplates.cend(),
        [&templateId](const auto& routing)
        {
            return routing.id == templateId;
        });
    return iterator == midiRoutingTemplates.cend() ? nullptr : &*iterator;
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

juce::String Project::rootTrackId(const juce::String& trackId) const
{
    const auto* track = findTrack(trackId);
    if (track == nullptr)
        return {};
    return track->parentTrackId.isNotEmpty() ? track->parentTrackId : track->id;
}

const EditGroup* Project::editGroupForTrack(const juce::String& trackId) const
{
    const auto rootId = rootTrackId(trackId);
    if (rootId.isEmpty())
        return nullptr;
    const auto iterator = std::find_if(editGroups.cbegin(),
                                       editGroups.cend(),
                                       [&rootId](const auto& group)
    {
        return std::find(group.trackIds.cbegin(),
                         group.trackIds.cend(),
                         rootId) != group.trackIds.cend();
    });
    return iterator == editGroups.cend() ? nullptr : &*iterator;
}

const ReampRoute* Project::reampRouteForReturn(const juce::String& trackId) const
{
    const auto rootId = rootTrackId(trackId);
    const auto iterator = std::find_if(reampRoutes.cbegin(),
                                       reampRoutes.cend(),
                                       [&rootId](const auto& route)
    {
        return route.returnTrackId == rootId;
    });
    return iterator == reampRoutes.cend() ? nullptr : &*iterator;
}

juce::String Project::masterTrackId() const
{
    const auto master = std::find_if(tracks.cbegin(), tracks.cend(), [](const auto& track)
    {
        return track.type == TrackType::master && track.parentTrackId.isEmpty();
    });
    return master == tracks.cend() ? juce::String() : master->id;
}

juce::String Project::resolvedOutputTrackId(const Track& track) const
{
    if (track.type == TrackType::master)
        return {};
    const auto mainRoute = std::find_if(
        routingConnections.cbegin(),
        routingConnections.cend(),
        [&track](const auto& connection)
        {
            return connection.enabled
                && connection.signalType == SignalType::audio
                && connection.kind == RouteKind::mainOutput
                && connection.sourceTrackId == track.id
                && connection.destination.type == RouteEndpointType::track;
        });
    if (mainRoute != routingConnections.cend())
        return mainRoute->destination.trackId;
    return track.outputTrackId.isNotEmpty() ? track.outputTrackId : masterTrackId();
}

bool Project::validateTrackOutput(const juce::String& sourceTrackId,
                                  const juce::String& destinationTrackId,
                                  juce::String& error) const
{
    const auto* source = findTrack(sourceTrackId);
    if (source == nullptr)
    {
        error = "The source track no longer exists.";
        return false;
    }
    if (source->parentTrackId.isNotEmpty())
    {
        error = "Version lanes follow their parent track output.";
        return false;
    }
    if (source->type == TrackType::master)
    {
        error = "The master track cannot be routed.";
        return false;
    }

    const auto masterId = masterTrackId();
    if (masterId.isEmpty())
    {
        error = "The project does not contain a master track.";
        return false;
    }

    auto currentId = destinationTrackId.isNotEmpty() ? destinationTrackId : masterId;
    std::vector<juce::String> visited;
    while (currentId.isNotEmpty())
    {
        if (currentId == sourceTrackId)
        {
            error = "Track routing cannot contain a cycle.";
            return false;
        }
        if (std::find(visited.cbegin(), visited.cend(), currentId) != visited.cend())
        {
            error = "The existing track routing contains a cycle.";
            return false;
        }
        visited.push_back(currentId);

        const auto* destination = findTrack(currentId);
        if (destination == nullptr || destination->parentTrackId.isNotEmpty())
        {
            error = "The selected output track is unavailable.";
            return false;
        }
        if (destination->type == TrackType::master)
            return true;
        if (destination->type != TrackType::bus)
        {
            error = "Tracks can route only to a bus or the master.";
            return false;
        }
        currentId = resolvedOutputTrackId(*destination);
    }

    error = "The track output does not reach the master.";
    return false;
}

std::optional<std::vector<juce::String>> Project::routingOrder(juce::String& error) const
{
    return routingGraphOrder(error);
}

bool Project::validateRoutingGraph(juce::String& error) const
{
    return RoutingGraph::validate(*this, error);
}

std::optional<std::vector<juce::String>> Project::routingGraphOrder(
    juce::String& error) const
{
    return RoutingGraph::order(*this, error);
}

SectionClickSettings Project::clickSettingsAt(double seconds) const
{
    SectionClickSettings settings;
    settings.subdivision = metronomeSubdivision;
    settings.level = metronomeLevel;
    settings.accentLevel = metronomeAccentLevel;
    const auto position = std::max(0.0, seconds);
    auto latestPosition = -1.0;
    for (const auto& section : sections)
    {
        if (section.clickSettings.has_value()
            && section.timeSeconds <= position
            && section.timeSeconds > latestPosition)
        {
            settings = *section.clickSettings;
            latestPosition = section.timeSeconds;
        }
    }
    return settings;
}

std::optional<SectionTransportSettings> Project::sectionTransportSettings(
    const juce::String& sectionId,
    juce::String& error) const
{
    const auto* section = findSection(sectionId);
    if (section == nullptr)
    {
        error = "The song section no longer exists.";
        return std::nullopt;
    }

    SectionTransportSettings settings;
    settings.clickSettings = section->clickSettings;
    const auto tempoPoint = std::find_if(
        tempoChanges.cbegin(), tempoChanges.cend(),
        [&sectionId](const auto& change)
        {
            return change.sectionId == sectionId;
        });
    if (tempoPoint != tempoChanges.cend())
    {
        settings.tempoBpm = tempoPoint->bpm;
        if (tempoPoint != tempoChanges.cbegin())
            settings.rampFromPrevious = (tempoPoint - 1)->rampToNext;
    }
    const auto meterPoint = std::find_if(
        meterChanges.cbegin(), meterChanges.cend(),
        [&sectionId](const auto& change)
        {
            return change.sectionId == sectionId;
        });
    if (meterPoint != meterChanges.cend())
    {
        settings.timeSignature = SectionTimeSignature {
            meterPoint->numerator, meterPoint->denominator
        };
    }
    return settings;
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

bool Project::validateTransport(juce::String& error) const
{
    if (!std::isfinite(tempo)
        || tempo < 20.0
        || tempo > 400.0
        || timeSignatureNumerator < 1
        || timeSignatureNumerator > 32
        || !validMeterDenominator(timeSignatureDenominator))
    {
        error = "The base tempo or time signature is invalid.";
        return false;
    }

    for (std::size_t index = 0; index < markers.size(); ++index)
    {
        const auto& marker = markers[index];
        if (marker.id.trim().isEmpty()
            || marker.name.trim().isEmpty()
            || !std::isfinite(marker.timeSeconds)
            || marker.timeSeconds < 0.0)
        {
            error = "Project markers require an ID, a name, and a finite non-negative position.";
            return false;
        }
        if (std::any_of(
                markers.cbegin(),
                markers.cbegin() + static_cast<std::ptrdiff_t>(index),
                [&marker](const auto& previous)
                {
                    return previous.id == marker.id;
                }))
        {
            error = "Project markers require unique IDs.";
            return false;
        }
    }

    for (std::size_t index = 0; index < sections.size(); ++index)
    {
        const auto& section = sections[index];
        if (section.id.trim().isEmpty()
            || section.name.trim().isEmpty()
            || !std::isfinite(section.timeSeconds)
            || section.timeSeconds < 0.0)
        {
            error = "Song sections require an ID, a name, and a finite non-negative position.";
            return false;
        }
        if (section.clickSettings.has_value()
            && !section.clickSettings->validate(error))
            return false;
        if (section.endTimeSeconds.has_value()
            && (!std::isfinite(*section.endTimeSeconds)
                || *section.endTimeSeconds
                    <= section.timeSeconds))
        {
            error =
                "Song section ends must be finite and after their starts.";
            return false;
        }
        if (section.endTimeSeconds.has_value()
            && index + 1 < sections.size()
            && *section.endTimeSeconds
                > sections[index + 1].timeSeconds)
        {
            error =
                "Song sections cannot overlap the next section.";
            return false;
        }
        if (std::any_of(
                sections.cbegin(),
                sections.cbegin() + static_cast<std::ptrdiff_t>(index),
                [&section](const auto& previous)
                {
                    return previous.id == section.id
                        || std::abs(previous.timeSeconds - section.timeSeconds) < 0.0001;
                }))
        {
            error = "Song sections require unique IDs and timeline positions.";
            return false;
        }
    }

    for (std::size_t index = 0; index < tempoChanges.size(); ++index)
    {
        const auto& change = tempoChanges[index];
        if (!std::isfinite(change.timeSeconds)
            || !std::isfinite(change.bpm)
            || change.timeSeconds < 0.0
            || change.bpm < 20.0
            || change.bpm > 400.0)
        {
            error = "Tempo changes require finite positions and tempos from 20 to 400 BPM.";
            return false;
        }
        if (index > 0
            && change.timeSeconds <= tempoChanges[index - 1].timeSeconds)
        {
            error = "Tempo changes require unique positions in timeline order.";
            return false;
        }
        if (!validateSectionOwner(*this, tempoChanges, index, error))
            return false;
    }

    for (std::size_t index = 0; index < meterChanges.size(); ++index)
    {
        const auto& change = meterChanges[index];
        if (!std::isfinite(change.timeSeconds)
            || change.timeSeconds < 0.0
            || change.numerator < 1
            || change.numerator > 32
            || !validMeterDenominator(change.denominator))
        {
            error = "Meter changes require finite positions and supported time signatures.";
            return false;
        }
        if (index > 0
            && change.timeSeconds <= meterChanges[index - 1].timeSeconds)
        {
            error = "Meter changes require unique positions in timeline order.";
            return false;
        }
        if (!validateSectionOwner(*this, meterChanges, index, error))
            return false;
    }

    if (metronomeSubdivision < 1
        || metronomeSubdivision > 8
        || metronomeOutputChannel < 0
        || !std::isfinite(metronomeLevel)
        || metronomeLevel < 0.0f
        || metronomeLevel > 1.0f
        || !std::isfinite(metronomeAccentLevel)
        || metronomeAccentLevel < 0.0f
        || metronomeAccentLevel > 1.0f)
    {
        error = "The metronome routing, subdivision, or level is invalid.";
        return false;
    }

    if (!std::isfinite(punchInSeconds)
        || !std::isfinite(punchOutSeconds)
        || punchInSeconds < 0.0
        || punchOutSeconds <= punchInSeconds
        || countInBars < 0
        || countInBars > 8
        || !std::isfinite(preRollSeconds)
        || preRollSeconds < 0.0
        || preRollSeconds > 30.0
        || !std::isfinite(postRollSeconds)
        || postRollSeconds < 0.0
        || postRollSeconds > 30.0)
    {
        error = "The punch, count-in, or pre/post-roll settings are invalid.";
        return false;
    }

    if (!std::isfinite(loopStartSeconds)
        || !std::isfinite(loopEndSeconds)
        || loopStartSeconds < 0.0
        || loopEndSeconds <= loopStartSeconds)
    {
        error = "The loop range is invalid.";
        return false;
    }

    return true;
}

double Project::lengthSeconds() const noexcept
{
    double length = 8.0;
    for (const auto& section : sections)
    {
        length = std::max(length, section.timeSeconds);
        if (section.endTimeSeconds.has_value())
            length = std::max(
                length,
                *section.endTimeSeconds);
    }
    for (const auto& track : tracks)
    {
        for (const auto& clip : track.clips)
            length = std::max(length, clip.endSeconds());
        for (const auto& clip : track.midiClips)
            length = std::max(length, secondsAtBeat(clip.endBeats()));
    }

    return length;
}

double Project::timelineEndSeconds() const noexcept
{
    auto end = lengthSeconds();
    for (const auto& change : tempoChanges)
        end = std::max(end, change.timeSeconds);
    for (const auto& change : meterChanges)
        end = std::max(end, change.timeSeconds);
    if (punchEnabled)
        end = std::max(end, punchOutSeconds + postRollSeconds);
    if (loopEnabled)
        end = std::max(end, loopEndSeconds);
    return end;
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
    object->setProperty("metadata", metadata.toVar());
    object->setProperty("mastering", mastering.toVar());
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
    juce::Array<juce::var> markerValues;
    for (const auto& marker : markers)
        markerValues.add(marker.toVar());
    object->setProperty("markers", juce::var(markerValues));
    juce::Array<juce::var> sectionValues;
    for (const auto& section : sections)
        sectionValues.add(section.toVar());
    object->setProperty("sections", juce::var(sectionValues));
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
    juce::Array<juce::var> editGroupValues;
    for (const auto& group : editGroups)
        editGroupValues.add(group.toVar());
    object->setProperty("editGroups", juce::var(editGroupValues));
    juce::Array<juce::var> reampValues;
    for (const auto& route : reampRoutes)
        reampValues.add(route.toVar());
    object->setProperty("reampRoutes", juce::var(reampValues));
    juce::Array<juce::var> routingValues;
    for (const auto& connection : routingConnections)
        routingValues.add(connection.toVar());
    object->setProperty("routingConnections", juce::var(routingValues));
    juce::Array<juce::var> automationValues;
    for (const auto& lane : automationLanes)
        automationValues.add(lane.toVar());
    object->setProperty("automationLanes", juce::var(automationValues));
    juce::Array<juce::var> toneSnapshotValues;
    for (const auto& snapshot : toneSnapshots)
        toneSnapshotValues.add(snapshot.toVar());
    object->setProperty("toneSnapshots", juce::var(toneSnapshotValues));
    juce::Array<juce::var> mixerSnapshotValues;
    for (const auto& snapshot : mixerSnapshots)
        mixerSnapshotValues.add(snapshot.toVar());
    object->setProperty("mixerSnapshots", juce::var(mixerSnapshotValues));
    juce::Array<juce::var> reportValues;
    for (const auto& report : renderReports)
        reportValues.add(report.toVar());
    object->setProperty("renderReports", juce::var(reportValues));
    juce::Array<juce::var> drumMapValues;
    for (const auto& map : drumMaps)
        drumMapValues.add(map.toVar());
    object->setProperty("drumMaps", juce::var(drumMapValues));
    juce::Array<juce::var> patternValues;
    for (const auto& pattern : midiPatterns)
        patternValues.add(pattern.toVar());
    object->setProperty("midiPatterns", juce::var(patternValues));
    juce::Array<juce::var> midiRoutingTemplateValues;
    for (const auto& routing : midiRoutingTemplates)
        midiRoutingTemplateValues.add(routing.toVar());
    object->setProperty(
        "midiRoutingTemplates",
        juce::var(midiRoutingTemplateValues));
    juce::Array<juce::var> sceneValues;
    for (const auto& scene : scenes)
        sceneValues.add(scene.toVar());
    object->setProperty("scenes", juce::var(sceneValues));
    juce::Array<juce::var> compatibilityReportValues;
    for (const auto& report : compatibilityReports)
        compatibilityReportValues.add(report.toVar());
    object->setProperty(
        "compatibilityReports",
        juce::var(compatibilityReportValues));

    juce::Array<juce::var> trackValues;
    trackValues.ensureStorageAllocated(static_cast<int>(tracks.size()));
    for (const auto& track : tracks)
        trackValues.add(track.toVar());

    object->setProperty("tracks", juce::var(trackValues));
    return juce::var(object.release());
}

std::optional<Project> Project::fromVar(const juce::var& value, juce::String& error)
{
    const auto migrated = ProjectMigration::migrateToCurrent(value, error);
    if (!migrated.has_value())
        return std::nullopt;

    const auto* object = requireObject(*migrated, error, "Project");
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
    auto metadata = ProjectMetadata::fromVar(
        object->getProperty("metadata"),
        error);
    if (!metadata.has_value())
        return std::nullopt;
    project.metadata = std::move(*metadata);
    auto mastering = MasteringAlbum::fromVar(
        object->getProperty("mastering"),
        error);
    if (!mastering.has_value())
        return std::nullopt;
    project.mastering = std::move(*mastering);
    project.tempo = numberProperty(*object, "tempo", 120.0);
    project.timeSignatureNumerator = integerProperty(*object, "timeSignatureNumerator", 4);
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
    const auto markerValues = object->getProperty("markers");
    if (markerValues.isArray())
    {
        for (const auto& markerValue : *markerValues.getArray())
        {
            auto marker = ProjectMarker::fromVar(
                markerValue,
                error);
            if (!marker.has_value())
                return std::nullopt;
            const auto duplicate = std::find_if(
                project.markers.cbegin(),
                project.markers.cend(),
                [&marker](const auto& existing)
                {
                    return existing.id == marker->id;
                });
            if (duplicate != project.markers.cend())
            {
                error = "Project markers require unique IDs.";
                return std::nullopt;
            }
            project.markers.push_back(std::move(*marker));
        }
        std::stable_sort(
            project.markers.begin(),
            project.markers.end(),
            [](const auto& left, const auto& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
    }
    const auto sectionValues = object->getProperty("sections");
    if (sectionValues.isArray())
    {
        for (const auto& sectionValue : *sectionValues.getArray())
        {
            auto section = SongSection::fromVar(sectionValue, error);
            if (!section.has_value())
                return std::nullopt;
            const auto duplicate = std::find_if(
                project.sections.cbegin(),
                project.sections.cend(),
                [&section](const auto& existing)
                {
                    return existing.id == section->id
                        || std::abs(existing.timeSeconds - section->timeSeconds)
                           < 0.0001;
                });
            if (duplicate != project.sections.cend())
            {
                error = "Song sections require unique IDs and timeline positions.";
                return std::nullopt;
            }
            project.sections.push_back(std::move(*section));
        }
        std::stable_sort(project.sections.begin(),
                         project.sections.end(),
                         [](const auto& left, const auto& right)
                         {
                            return left.timeSeconds < right.timeSeconds;
                         });
    }
    project.metronomeEnabled = booleanProperty(*object, "metronomeEnabled", true);
    project.metronomeSubdivision = integerProperty(*object, "metronomeSubdivision", 1);
    project.metronomeOutputChannel = integerProperty(*object, "metronomeOutputChannel", 0);
    project.metronomeLevel = static_cast<float>(
        numberProperty(*object, "metronomeLevel", 0.65));
    project.metronomeAccentLevel = static_cast<float>(
        numberProperty(*object, "metronomeAccentLevel", 1.0));
    project.punchEnabled = booleanProperty(*object, "punchEnabled", false);
    project.punchInSeconds = numberProperty(*object, "punchInSeconds", 0.0);
    project.punchOutSeconds = numberProperty(*object, "punchOutSeconds", 8.0);
    project.countInBars = integerProperty(*object, "countInBars", 0);
    project.preRollSeconds = numberProperty(*object, "preRollSeconds", 0.0);
    project.postRollSeconds = numberProperty(*object, "postRollSeconds", 0.0);
    project.loopEnabled = booleanProperty(*object, "loopEnabled", false);
    project.loopStartSeconds = numberProperty(*object, "loopStartSeconds", 0.0);
    project.loopEndSeconds = numberProperty(*object, "loopEndSeconds", 8.0);
    const auto editGroupValues = object->getProperty("editGroups");
    if (editGroupValues.isArray())
    {
        for (const auto& groupValue : *editGroupValues.getArray())
        {
            auto group = EditGroup::fromVar(groupValue, error);
            if (!group.has_value())
                return std::nullopt;
            project.editGroups.push_back(std::move(*group));
        }
    }
    const auto reampValues = object->getProperty("reampRoutes");
    if (reampValues.isArray())
    {
        for (const auto& routeValue : *reampValues.getArray())
        {
            auto route = ReampRoute::fromVar(routeValue, error);
            if (!route.has_value())
                return std::nullopt;
            project.reampRoutes.push_back(std::move(*route));
        }
    }
    const auto routingValues = object->getProperty("routingConnections");
    if (routingValues.isArray())
    {
        for (const auto& routingValue : *routingValues.getArray())
        {
            auto connection = RoutingConnection::fromVar(routingValue, error);
            if (!connection.has_value())
                return std::nullopt;
            project.routingConnections.push_back(std::move(*connection));
        }
    }
    const auto automationValues = object->getProperty("automationLanes");
    if (automationValues.isArray())
    {
        for (const auto& automationValue : *automationValues.getArray())
        {
            auto lane = AutomationLane::fromVar(automationValue, error);
            if (!lane.has_value())
                return std::nullopt;
            project.automationLanes.push_back(std::move(*lane));
        }
    }
    const auto toneSnapshotValues = object->getProperty("toneSnapshots");
    if (toneSnapshotValues.isArray())
    {
        for (const auto& snapshotValue : *toneSnapshotValues.getArray())
        {
            auto snapshot = ToneSnapshot::fromVar(snapshotValue, error);
            if (!snapshot.has_value())
                return std::nullopt;
            project.toneSnapshots.push_back(std::move(*snapshot));
        }
    }
    const auto mixerSnapshotValues = object->getProperty("mixerSnapshots");
    if (mixerSnapshotValues.isArray())
    {
        for (const auto& snapshotValue : *mixerSnapshotValues.getArray())
        {
            auto snapshot = MixerSnapshot::fromVar(snapshotValue, error);
            if (!snapshot.has_value())
                return std::nullopt;
            project.mixerSnapshots.push_back(std::move(*snapshot));
        }
    }
    const auto reportValues = object->getProperty("renderReports");
    if (reportValues.isArray())
    {
        for (const auto& reportValue : *reportValues.getArray())
        {
            auto report = RenderReport::fromVar(reportValue, error);
            if (!report.has_value())
                return std::nullopt;
            project.renderReports.push_back(std::move(*report));
        }
    }
    const auto drumMapValues = object->getProperty("drumMaps");
    if (drumMapValues.isArray())
    {
        for (const auto& drumMapValue : *drumMapValues.getArray())
        {
            auto map = DrumMap::fromVar(drumMapValue, error);
            if (!map.has_value())
                return std::nullopt;
            project.drumMaps.push_back(std::move(*map));
        }
    }
    const auto patternValues = object->getProperty("midiPatterns");
    if (patternValues.isArray())
    {
        for (const auto& patternValue : *patternValues.getArray())
        {
            auto pattern = MidiPatternAlias::fromVar(patternValue, error);
            if (!pattern.has_value())
                return std::nullopt;
            project.midiPatterns.push_back(std::move(*pattern));
        }
    }
    const auto midiRoutingTemplateValues =
        object->getProperty("midiRoutingTemplates");
    if (midiRoutingTemplateValues.isArray())
    {
        for (const auto& templateValue :
             *midiRoutingTemplateValues.getArray())
        {
            auto routing = MidiRoutingTemplate::fromVar(
                templateValue,
                error);
            if (!routing.has_value())
                return std::nullopt;
            project.midiRoutingTemplates.push_back(std::move(*routing));
        }
    }
    const auto sceneValues = object->getProperty("scenes");
    if (!sceneValues.isArray())
    {
        error = "Project scenes must be a JSON array.";
        return std::nullopt;
    }
    for (const auto& sceneValue : *sceneValues.getArray())
    {
        auto scene = ProjectScene::fromVar(sceneValue, error);
        if (!scene.has_value())
            return std::nullopt;
        project.scenes.push_back(std::move(*scene));
    }
    const auto compatibilityReportValues =
        object->getProperty("compatibilityReports");
    if (!compatibilityReportValues.isArray())
    {
        error = "Project compatibility reports must be a JSON array.";
        return std::nullopt;
    }
    for (const auto& reportValue : *compatibilityReportValues.getArray())
    {
        auto report = CompatibilityReport::fromVar(reportValue, error);
        if (!report.has_value())
            return std::nullopt;
        project.compatibilityReports.push_back(std::move(*report));
    }

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
        if (!track.midiClips.empty()
            && (track.parentTrackId.isNotEmpty()
                || (track.type != TrackType::midi
                    && track.type != TrackType::instrument)))
        {
            error = "MIDI clips require a root MIDI or instrument track.";
            return std::nullopt;
        }
        if ((track.parentTrackId.isNotEmpty() || track.type == TrackType::master)
            && track.outputTrackId.isNotEmpty())
        {
            error = track.type == TrackType::master
                ? "The master track cannot have a project output route."
                : "Version lanes cannot override their parent track output.";
            return std::nullopt;
        }
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
    std::vector<juce::String> sceneIds;
    std::vector<juce::String> sceneObjectIds;
    const auto addSceneObjectId = [&sceneObjectIds](
                                      const juce::String& objectId)
    {
        if (objectId.isEmpty()
            || std::find(
                   sceneObjectIds.cbegin(),
                   sceneObjectIds.cend(),
                   objectId)
                != sceneObjectIds.cend())
            return false;
        sceneObjectIds.push_back(objectId);
        return true;
    };
    for (const auto& scene : project.scenes)
    {
        if (std::find(sceneIds.cbegin(), sceneIds.cend(), scene.id)
                != sceneIds.cend()
            || !addSceneObjectId(scene.id))
        {
            error = "Scene IDs must be unique.";
            return std::nullopt;
        }
        sceneIds.push_back(scene.id);
        for (const auto& slot : scene.slots)
        {
            const auto* track = project.findTrack(slot.trackId);
            if (track == nullptr || !addSceneObjectId(slot.id))
            {
                error =
                    "Scene slots require unique IDs and available tracks.";
                return std::nullopt;
            }
            if (slot.audioClip.has_value())
            {
                if (!addSceneObjectId(slot.audioClip->id))
                {
                    error = "Scene clip IDs must be unique.";
                    return std::nullopt;
                }
            }
            if (slot.midiClip.has_value())
            {
                const auto& clip = *slot.midiClip;
                if (!addSceneObjectId(clip.id)
                    || (clip.drumMapId.isNotEmpty()
                        && project.findDrumMap(clip.drumMapId) == nullptr))
                {
                    error =
                        "Scene MIDI clips require unique IDs and available drum maps.";
                    return std::nullopt;
                }
                const auto* map = project.findDrumMap(clip.drumMapId);
                for (const auto& note : clip.notes)
                {
                    if (!addSceneObjectId(note.id)
                        || (note.drumMapEntryId.isNotEmpty()
                            && (map == nullptr
                                || map->entryForId(note.drumMapEntryId)
                                    == nullptr)))
                    {
                        error =
                            "Scene MIDI notes require unique IDs and available drum-map entries.";
                        return std::nullopt;
                    }
                    for (const auto& expression : note.expressions)
                    {
                        if (!addSceneObjectId(expression.id))
                        {
                            error =
                                "Scene MIDI expression IDs must be unique.";
                            return std::nullopt;
                        }
                    }
                }
            }
        }
    }
    std::vector<juce::String> compatibilityReportIds;
    for (const auto& report : project.compatibilityReports)
    {
        if (std::find(
                compatibilityReportIds.cbegin(),
                compatibilityReportIds.cend(),
                report.id)
            != compatibilityReportIds.cend())
        {
            error = "Compatibility report IDs must be unique.";
            return std::nullopt;
        }
        compatibilityReportIds.push_back(report.id);
    }
    if (!project.routingOrder(error).has_value())
        return std::nullopt;
    std::vector<juce::String> groupedTracks;
    for (const auto& group : project.editGroups)
    {
        if (std::find(group.trackIds.cbegin(),
                      group.trackIds.cend(),
                      group.timingReferenceTrackId) == group.trackIds.cend())
        {
            error = "An edit group has an invalid timing reference.";
            return std::nullopt;
        }
        for (const auto& trackId : group.trackIds)
        {
            const auto* track = project.findTrack(trackId);
            if (track == nullptr
                || track->parentTrackId.isNotEmpty()
                || std::find(groupedTracks.cbegin(),
                             groupedTracks.cend(),
                             trackId) != groupedTracks.cend())
            {
                error = "An edit group references an unavailable or duplicate track.";
                return std::nullopt;
            }
            groupedTracks.push_back(trackId);
        }
    }
    std::vector<juce::String> returnTracks;
    for (const auto& route : project.reampRoutes)
    {
        const auto* source = project.findTrack(route.sourceTrackId);
        const auto* returnTrack = project.findTrack(route.returnTrackId);
        if (source == nullptr
            || returnTrack == nullptr
            || source->parentTrackId.isNotEmpty()
            || returnTrack->parentTrackId.isNotEmpty()
            || source->type != TrackType::audio
            || returnTrack->type != TrackType::audio
            || std::find(returnTracks.cbegin(),
                         returnTracks.cend(),
                         route.returnTrackId) != returnTracks.cend())
        {
            error = "A reamp route references unavailable or duplicate tracks.";
            return std::nullopt;
        }
        returnTracks.push_back(route.returnTrackId);
    }
    std::vector<juce::String> automationLaneIds;
    for (const auto& lane : project.automationLanes)
    {
        if (project.findTrack(lane.target.trackId) == nullptr
            || std::find(automationLaneIds.cbegin(),
                         automationLaneIds.cend(),
                         lane.id) != automationLaneIds.cend())
        {
            error = "Automation lanes reference unavailable tracks or duplicate IDs.";
            return std::nullopt;
        }
        if (lane.target.routeId.isNotEmpty()
            && project.findRoutingConnection(lane.target.routeId) == nullptr)
        {
            error = "Automation lane references an unavailable route.";
            return std::nullopt;
        }
        if (lane.target.insertId.isNotEmpty())
        {
            const auto* track = project.findTrack(lane.target.trackId);
            if (track == nullptr
                || std::none_of(
                    track->inserts.cbegin(),
                    track->inserts.cend(),
                    [&lane](const auto& insert)
                    {
                        return insert.id == lane.target.insertId;
                    }))
            {
                error = "Automation lane references an unavailable insert.";
                return std::nullopt;
            }
        }
        automationLaneIds.push_back(lane.id);
    }
    std::vector<juce::String> toneSnapshotIds;
    for (const auto& snapshot : project.toneSnapshots)
    {
        const auto route = std::find_if(
            project.reampRoutes.cbegin(),
            project.reampRoutes.cend(),
            [&snapshot](const auto& candidate)
            {
                return candidate.id == snapshot.reampRouteId;
            });
        if (route == project.reampRoutes.cend()
            || route->sourceTrackId != snapshot.sourceTrackId
            || route->returnTrackId != snapshot.returnTrackId
            || std::find(toneSnapshotIds.cbegin(),
                         toneSnapshotIds.cend(),
                         snapshot.id) != toneSnapshotIds.cend())
        {
            error = "Tone snapshots reference unavailable routes or duplicate IDs.";
            return std::nullopt;
        }
        toneSnapshotIds.push_back(snapshot.id);
    }
    for (const auto& route : project.reampRoutes)
    {
        if (route.activeSnapshotId.isEmpty())
            continue;
        const auto snapshot = std::find_if(
            project.toneSnapshots.cbegin(),
            project.toneSnapshots.cend(),
            [&route](const auto& candidate)
            {
                return candidate.id == route.activeSnapshotId
                    && candidate.reampRouteId == route.id;
            });
        if (snapshot == project.toneSnapshots.cend())
        {
            error = "A reamp route references an unavailable active snapshot.";
            return std::nullopt;
        }
    }
    std::vector<juce::String> mixerSnapshotIds;
    for (const auto& snapshot : project.mixerSnapshots)
    {
        if (std::find(mixerSnapshotIds.cbegin(),
                      mixerSnapshotIds.cend(),
                      snapshot.id) != mixerSnapshotIds.cend()
            || std::any_of(
                snapshot.tracks.cbegin(),
                snapshot.tracks.cend(),
                [&project](const auto& track)
                {
                    return project.findTrack(track.trackId) == nullptr;
                }))
        {
            error = "Mixer snapshots reference unavailable tracks or duplicate IDs.";
            return std::nullopt;
        }
        mixerSnapshotIds.push_back(snapshot.id);
    }
    std::vector<juce::String> reportIds;
    for (const auto& report : project.renderReports)
    {
        if (std::find(reportIds.cbegin(),
                      reportIds.cend(),
                      report.id) != reportIds.cend())
        {
            error = "Render report IDs must be unique.";
            return std::nullopt;
        }
        reportIds.push_back(report.id);
    }

    std::vector<juce::String> drumMapIds;
    for (const auto& map : project.drumMaps)
    {
        if (std::find(
                drumMapIds.cbegin(),
                drumMapIds.cend(),
                map.id)
            != drumMapIds.cend())
        {
            error = "Drum-map IDs must be unique.";
            return std::nullopt;
        }
        drumMapIds.push_back(map.id);
    }
    std::vector<juce::String> patternIds;
    for (const auto& pattern : project.midiPatterns)
    {
        if (std::find(patternIds.cbegin(), patternIds.cend(), pattern.id)
            != patternIds.cend())
        {
            error = "MIDI pattern IDs must be unique.";
            return std::nullopt;
        }
        patternIds.push_back(pattern.id);
    }
    std::vector<juce::String> routingTemplateIds;
    for (const auto& routing : project.midiRoutingTemplates)
    {
        if (std::find(
                routingTemplateIds.cbegin(),
                routingTemplateIds.cend(),
                routing.id)
            != routingTemplateIds.cend())
        {
            error = "MIDI routing-template IDs must be unique.";
            return std::nullopt;
        }
        routingTemplateIds.push_back(routing.id);
    }
    std::vector<juce::String> midiClipIds;
    std::vector<juce::String> midiObjectIds;
    const auto addMidiObjectId = [&midiObjectIds](
                                     const juce::String& objectId)
    {
        if (objectId.isEmpty()
            || std::find(
                   midiObjectIds.cbegin(),
                   midiObjectIds.cend(),
                   objectId)
                != midiObjectIds.cend())
            return false;
        midiObjectIds.push_back(objectId);
        return true;
    };
    for (const auto& map : project.drumMaps)
    {
        if (!addMidiObjectId(map.id))
        {
            error = "Persisted MIDI object IDs must be unique.";
            return std::nullopt;
        }
        for (const auto& entry : map.entries)
        {
            if (!addMidiObjectId(entry.id))
            {
                error = "Persisted MIDI object IDs must be unique.";
                return std::nullopt;
            }
        }
    }
    for (const auto& pattern : project.midiPatterns)
    {
        if (!addMidiObjectId(pattern.id))
        {
            error = "Persisted MIDI object IDs must be unique.";
            return std::nullopt;
        }
        for (const auto& event : pattern.events)
        {
            if (!addMidiObjectId(event.id))
            {
                error = "Persisted MIDI object IDs must be unique.";
                return std::nullopt;
            }
        }
    }
    for (const auto& routing : project.midiRoutingTemplates)
    {
        if (!addMidiObjectId(routing.id))
        {
            error = "Persisted MIDI object IDs must be unique.";
            return std::nullopt;
        }
        for (const auto& output : routing.outputs)
        {
            if (!addMidiObjectId(output.id))
            {
                error = "Persisted MIDI object IDs must be unique.";
                return std::nullopt;
            }
        }
    }
    for (const auto& track : project.tracks)
    {
        for (const auto& clip : track.midiClips)
        {
            if (std::find(
                    midiClipIds.cbegin(),
                    midiClipIds.cend(),
                    clip.id)
                    != midiClipIds.cend()
                || (clip.drumMapId.isNotEmpty()
                    && project.findDrumMap(clip.drumMapId) == nullptr)
                || !addMidiObjectId(clip.id))
            {
                error = "MIDI clips require unique IDs and available drum maps.";
                return std::nullopt;
            }
            midiClipIds.push_back(clip.id);
            const auto* map = project.findDrumMap(clip.drumMapId);
            for (const auto& note : clip.notes)
            {
                if (!addMidiObjectId(note.id)
                    || (note.drumMapEntryId.isNotEmpty()
                        && (map == nullptr
                            || map->entryForId(note.drumMapEntryId)
                                == nullptr)))
                {
                    error = "MIDI notes require unique IDs and available drum-map entries.";
                    return std::nullopt;
                }
                for (const auto& expression : note.expressions)
                {
                    if (!addMidiObjectId(expression.id))
                    {
                        error = "MIDI expression IDs must be unique.";
                        return std::nullopt;
                    }
                }
            }
        }
    }

    if (!project.validateTransport(error))
        return std::nullopt;
    return project;
}

juce::String trackTypeToString(TrackType type)
{
    switch (type)
    {
        case TrackType::audio: return "audio";
        case TrackType::instrument: return "instrument";
        case TrackType::midi: return "midi";
        case TrackType::aux: return "aux";
        case TrackType::bus: return "bus";
        case TrackType::folder: return "folder";
        case TrackType::vca: return "vca";
        case TrackType::controlRoom: return "controlRoom";
        case TrackType::master: return "master";
    }

    return "audio";
}

std::optional<TrackType> trackTypeFromString(const juce::String& value)
{
    if (value == "audio") return TrackType::audio;
    if (value == "instrument") return TrackType::instrument;
    if (value == "midi") return TrackType::midi;
    if (value == "aux") return TrackType::aux;
    if (value == "bus") return TrackType::bus;
    if (value == "folder") return TrackType::folder;
    if (value == "vca") return TrackType::vca;
    if (value == "controlRoom") return TrackType::controlRoom;
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

juce::String pluginStateFormatToString(PluginStateFormat format)
{
    switch (format)
    {
        case PluginStateFormat::hostOpaque: return "hostOpaque";
        case PluginStateFormat::generic: return "generic";
        case PluginStateFormat::vst2Preset: return "vst2Preset";
        case PluginStateFormat::vst3Preset: return "vst3Preset";
        case PluginStateFormat::clapPreset: return "clapPreset";
        case PluginStateFormat::auPreset: return "auPreset";
    }
    return "hostOpaque";
}

std::optional<PluginStateFormat> pluginStateFormatFromString(
    const juce::String& value)
{
    if (value.isEmpty() || value == "hostOpaque")
        return PluginStateFormat::hostOpaque;
    if (value == "generic") return PluginStateFormat::generic;
    if (value == "vst2Preset") return PluginStateFormat::vst2Preset;
    if (value == "vst3Preset") return PluginStateFormat::vst3Preset;
    if (value == "clapPreset") return PluginStateFormat::clapPreset;
    if (value == "auPreset") return PluginStateFormat::auPreset;
    return std::nullopt;
}

juce::String stretchModeToString(StretchMode mode)
{
    switch (mode)
    {
        case StretchMode::drums: return "drums";
        case StretchMode::monophonic: return "monophonic";
        case StretchMode::polyphonic: return "polyphonic";
        case StretchMode::mix: return "mix";
    }
    return "polyphonic";
}

std::optional<StretchMode> stretchModeFromString(const juce::String& value)
{
    if (value == "drums") return StretchMode::drums;
    if (value == "monophonic") return StretchMode::monophonic;
    if (value == "polyphonic" || value.isEmpty()) return StretchMode::polyphonic;
    if (value == "mix") return StretchMode::mix;
    return std::nullopt;
}

juce::String tonePathTypeToString(TonePathType type)
{
    switch (type)
    {
        case TonePathType::hardware: return "hardware";
        case TonePathType::plugin: return "plugin";
    }
    return "hardware";
}

std::optional<TonePathType> tonePathTypeFromString(const juce::String& value)
{
    if (value == "hardware" || value.isEmpty()) return TonePathType::hardware;
    if (value == "plugin") return TonePathType::plugin;
    return std::nullopt;
}

juce::String compatibilitySeverityToString(CompatibilitySeverity severity)
{
    switch (severity)
    {
        case CompatibilitySeverity::info: return "info";
        case CompatibilitySeverity::warning: return "warning";
        case CompatibilitySeverity::error: return "error";
    }
    return "warning";
}

std::optional<CompatibilitySeverity> compatibilitySeverityFromString(
    const juce::String& value)
{
    if (value == "info") return CompatibilitySeverity::info;
    if (value == "warning") return CompatibilitySeverity::warning;
    if (value == "error") return CompatibilitySeverity::error;
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
