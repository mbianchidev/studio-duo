#include "MasteringModel.h"

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
const juce::DynamicObject* requireObject(
    const juce::var& value,
    juce::String& error,
    const juce::String& context)
{
    if (const auto* object = value.getDynamicObject())
        return object;
    error = context + " must be a JSON object.";
    return nullptr;
}

double numberProperty(
    const juce::DynamicObject& object,
    const juce::Identifier& name,
    double fallback)
{
    const auto value = object.getProperty(name);
    return value.isDouble() || value.isInt() || value.isInt64()
        ? static_cast<double>(value)
        : fallback;
}

bool validHash(const juce::String& value)
{
    return value.isEmpty()
        || (value.length() == 64
            && value.containsOnly("0123456789abcdefABCDEF"));
}

bool validIsrc(const juce::String& value)
{
    if (value.isEmpty())
        return true;
    if (value.length() != 12)
        return false;
    for (int index = 0; index < value.length(); ++index)
    {
        const auto character = value[index];
        if (index < 5)
        {
            if (!juce::CharacterFunctions::isLetterOrDigit(character)
                || (index < 2
                    && !juce::CharacterFunctions::isLetter(character)))
                return false;
        }
        else if (!juce::CharacterFunctions::isDigit(character))
        {
            return false;
        }
    }
    return true;
}
}

juce::var MasteringSourceMix::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("file", file.getFullPathName());
    object->setProperty("sourceHash", sourceHash);
    object->setProperty("durationSeconds", durationSeconds);
    return juce::var(object.release());
}

std::optional<MasteringSourceMix> MasteringSourceMix::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(
        value,
        error,
        "Mastering source mix");
    if (object == nullptr)
        return std::nullopt;

    MasteringSourceMix source;
    source.id = object->getProperty("id").toString();
    source.name = object->getProperty("name").toString();
    source.file = juce::File(object->getProperty("file").toString());
    source.sourceHash = object->getProperty("sourceHash").toString();
    source.durationSeconds = numberProperty(
        *object,
        "durationSeconds",
        0.0);
    if (source.id.isEmpty()
        || source.name.trim().isEmpty()
        || source.file.getFullPathName().isEmpty()
        || !validHash(source.sourceHash)
        || !std::isfinite(source.durationSeconds)
        || source.durationSeconds <= 0.0)
    {
        error = "Mastering sources require an ID, name, file, valid hash, and positive duration.";
        return std::nullopt;
    }
    return source;
}

const MasteringSourceMix* MasteringTrack::selectedSource() const noexcept
{
    const auto selected = std::find_if(
        sources.cbegin(),
        sources.cend(),
        [this](const auto& source)
        {
            return source.id == selectedSourceId;
        });
    return selected != sources.cend() ? &*selected : nullptr;
}

MasteringSourceMix* MasteringTrack::selectedSource() noexcept
{
    return const_cast<MasteringSourceMix*>(
        std::as_const(*this).selectedSource());
}

juce::var MasteringTrack::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("title", title);
    object->setProperty("artist", artist);
    object->setProperty("songwriter", songwriter);
    object->setProperty("isrc", isrc);
    object->setProperty("gapBeforeSeconds", gapBeforeSeconds);
    object->setProperty(
        "overlapPreviousSeconds",
        overlapPreviousSeconds);
    object->setProperty("fadeInSeconds", fadeInSeconds);
    object->setProperty("fadeOutSeconds", fadeOutSeconds);
    object->setProperty("gainDecibels", gainDecibels);
    juce::Array<juce::var> markerValues;
    for (const auto marker : indexMarkersSeconds)
        markerValues.add(marker);
    object->setProperty(
        "indexMarkersSeconds",
        juce::var(markerValues));
    juce::Array<juce::var> sourceValues;
    for (const auto& source : sources)
        sourceValues.add(source.toVar());
    object->setProperty("sources", juce::var(sourceValues));
    object->setProperty("selectedSourceId", selectedSourceId);
    return juce::var(object.release());
}

std::optional<MasteringTrack> MasteringTrack::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "Mastering track");
    if (object == nullptr)
        return std::nullopt;

    MasteringTrack track;
    track.id = object->getProperty("id").toString();
    track.title = object->getProperty("title").toString();
    track.artist = object->getProperty("artist").toString();
    track.songwriter = object->getProperty("songwriter").toString();
    track.isrc = object->getProperty("isrc").toString().toUpperCase();
    track.gapBeforeSeconds = numberProperty(
        *object,
        "gapBeforeSeconds",
        0.0);
    track.overlapPreviousSeconds = numberProperty(
        *object,
        "overlapPreviousSeconds",
        0.0);
    track.fadeInSeconds = numberProperty(
        *object,
        "fadeInSeconds",
        0.0);
    track.fadeOutSeconds = numberProperty(
        *object,
        "fadeOutSeconds",
        0.0);
    track.gainDecibels = numberProperty(
        *object,
        "gainDecibels",
        0.0);

    const auto markerValues =
        object->getProperty("indexMarkersSeconds");
    const auto sourceValues = object->getProperty("sources");
    if (!markerValues.isArray() || !sourceValues.isArray())
    {
        error = "Mastering track markers and sources must be arrays.";
        return std::nullopt;
    }
    for (const auto& markerValue : *markerValues.getArray())
    {
        if (!markerValue.isDouble()
            && !markerValue.isInt()
            && !markerValue.isInt64())
        {
            error = "Mastering track markers must be numbers.";
            return std::nullopt;
        }
        track.indexMarkersSeconds.push_back(
            static_cast<double>(markerValue));
    }
    for (const auto& sourceValue : *sourceValues.getArray())
    {
        auto source = MasteringSourceMix::fromVar(
            sourceValue,
            error);
        if (!source.has_value())
            return std::nullopt;
        track.sources.push_back(std::move(*source));
    }
    track.selectedSourceId =
        object->getProperty("selectedSourceId").toString();
    const auto* selected = track.selectedSource();
    if (track.id.isEmpty()
        || track.title.trim().isEmpty()
        || !validIsrc(track.isrc)
        || !std::isfinite(track.gapBeforeSeconds)
        || !std::isfinite(track.overlapPreviousSeconds)
        || !std::isfinite(track.fadeInSeconds)
        || !std::isfinite(track.fadeOutSeconds)
        || !std::isfinite(track.gainDecibels)
        || track.gapBeforeSeconds < 0.0
        || track.overlapPreviousSeconds < 0.0
        || track.fadeInSeconds < 0.0
        || track.fadeOutSeconds < 0.0
        || track.sources.empty()
        || selected == nullptr
        || track.fadeInSeconds > selected->durationSeconds
        || track.fadeOutSeconds > selected->durationSeconds
        || track.overlapPreviousSeconds > selected->durationSeconds)
    {
        error = "Mastering tracks require valid metadata, timing, sources, and a selected source.";
        return std::nullopt;
    }
    std::vector<juce::String> sourceIds;
    for (const auto& source : track.sources)
    {
        if (std::find(sourceIds.cbegin(), sourceIds.cend(), source.id)
            != sourceIds.cend())
        {
            error = "Mastering source IDs must be unique within a track.";
            return std::nullopt;
        }
        sourceIds.push_back(source.id);
    }
    auto previousMarker = -1.0;
    for (const auto marker : track.indexMarkersSeconds)
    {
        if (!std::isfinite(marker)
            || marker < 0.0
            || marker > selected->durationSeconds
            || marker <= previousMarker)
        {
            error = "Mastering track markers must be unique, ordered, and within the selected source.";
            return std::nullopt;
        }
        previousMarker = marker;
    }
    return track;
}

juce::var MasteringReference::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("file", file.getFullPathName());
    object->setProperty("sourceHash", sourceHash);
    return juce::var(object.release());
}

std::optional<MasteringReference> MasteringReference::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(
        value,
        error,
        "Mastering reference");
    if (object == nullptr)
        return std::nullopt;
    MasteringReference reference;
    reference.id = object->getProperty("id").toString();
    reference.name = object->getProperty("name").toString();
    reference.file = juce::File(object->getProperty("file").toString());
    reference.sourceHash =
        object->getProperty("sourceHash").toString();
    if (reference.id.isEmpty()
        || reference.name.trim().isEmpty()
        || reference.file.getFullPathName().isEmpty()
        || !validHash(reference.sourceHash))
    {
        error = "Mastering references require an ID, name, file, and valid hash.";
        return std::nullopt;
    }
    return reference;
}

double MasteringTrackPlacement::endSeconds() const noexcept
{
    return startSeconds + durationSeconds;
}

std::vector<MasteringTrackPlacement> MasteringAlbum::placements() const
{
    std::vector<MasteringTrackPlacement> result;
    result.reserve(tracks.size());
    auto previousEnd = 0.0;
    for (const auto& track : tracks)
    {
        const auto* source = track.selectedSource();
        if (source == nullptr)
            continue;
        MasteringTrackPlacement placement;
        placement.trackId = track.id;
        placement.sourceId = source->id;
        placement.startSeconds = std::max(
            0.0,
            previousEnd
                + track.gapBeforeSeconds
                - track.overlapPreviousSeconds);
        placement.durationSeconds = source->durationSeconds;
        previousEnd = placement.endSeconds();
        result.push_back(std::move(placement));
    }
    return result;
}

double MasteringAlbum::durationSeconds() const noexcept
{
    const auto layout = placements();
    return layout.empty() ? 0.0 : layout.back().endSeconds();
}

juce::var MasteringAlbum::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("title", title);
    object->setProperty("artist", artist);
    object->setProperty("songwriter", songwriter);
    object->setProperty("label", label);
    object->setProperty("catalogNumber", catalogNumber);
    object->setProperty("mcn", mcn);
    object->setProperty("releaseDate", releaseDate);
    object->setProperty("genre", genre);
    object->setProperty("copyright", copyright);
    object->setProperty("outputGainDecibels", outputGainDecibels);
    juce::Array<juce::var> trackValues;
    for (const auto& track : tracks)
        trackValues.add(track.toVar());
    object->setProperty("tracks", juce::var(trackValues));
    juce::Array<juce::var> referenceValues;
    for (const auto& reference : references)
        referenceValues.add(reference.toVar());
    object->setProperty("references", juce::var(referenceValues));
    return juce::var(object.release());
}

std::optional<MasteringAlbum> MasteringAlbum::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(
        value,
        error,
        "Mastering album");
    if (object == nullptr)
        return std::nullopt;
    MasteringAlbum album;
    album.title = object->getProperty("title").toString();
    album.artist = object->getProperty("artist").toString();
    album.songwriter = object->getProperty("songwriter").toString();
    album.label = object->getProperty("label").toString();
    album.catalogNumber =
        object->getProperty("catalogNumber").toString();
    album.mcn = object->getProperty("mcn").toString();
    album.releaseDate = object->getProperty("releaseDate").toString();
    album.genre = object->getProperty("genre").toString();
    album.copyright = object->getProperty("copyright").toString();
    album.outputGainDecibels = numberProperty(
        *object,
        "outputGainDecibels",
        0.0);
    const auto trackValues = object->getProperty("tracks");
    const auto referenceValues = object->getProperty("references");
    if (!trackValues.isArray() || !referenceValues.isArray())
    {
        error = "Mastering album tracks and references must be arrays.";
        return std::nullopt;
    }
    for (const auto& trackValue : *trackValues.getArray())
    {
        auto track = MasteringTrack::fromVar(trackValue, error);
        if (!track.has_value())
            return std::nullopt;
        album.tracks.push_back(std::move(*track));
    }
    for (const auto& referenceValue : *referenceValues.getArray())
    {
        auto reference = MasteringReference::fromVar(
            referenceValue,
            error);
        if (!reference.has_value())
            return std::nullopt;
        album.references.push_back(std::move(*reference));
    }
    if (!std::isfinite(album.outputGainDecibels)
        || album.outputGainDecibels < -60.0
        || album.outputGainDecibels > 12.0)
    {
        error = "Mastering album output gain must be between -60 and 12 dB.";
        return std::nullopt;
    }
    std::vector<juce::String> objectIds;
    for (std::size_t index = 0; index < album.tracks.size(); ++index)
    {
        const auto& track = album.tracks[index];
        if (std::find(objectIds.cbegin(), objectIds.cend(), track.id)
            != objectIds.cend())
        {
            error = "Mastering track and reference IDs must be unique.";
            return std::nullopt;
        }
        if (index > 0)
        {
            const auto* previous =
                album.tracks[index - 1].selectedSource();
            if (previous != nullptr
                && track.overlapPreviousSeconds
                    > previous->durationSeconds)
            {
                error = "Mastering overlaps cannot exceed the preceding source.";
                return std::nullopt;
            }
        }
        objectIds.push_back(track.id);
    }
    for (const auto& reference : album.references)
    {
        if (std::find(
                objectIds.cbegin(),
                objectIds.cend(),
                reference.id)
            != objectIds.cend())
        {
            error = "Mastering track and reference IDs must be unique.";
            return std::nullopt;
        }
        objectIds.push_back(reference.id);
    }
    return album;
}
}
