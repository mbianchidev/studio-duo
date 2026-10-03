#include "StudioOneProjectIO.h"

#include "dawproject_io/DawProjectIdMapper.h"
#include "mix/RoutingGraph.h"
#include "project_io/ProjectCollectionService.h"
#include "project_io/ProjectArchiveReader.h"
#include "project_io/ProjectFile.h"
#include "project_io/ZipUtilities.h"
#include "util/NumberParsing.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace studio
{
namespace
{
juce::String objectId(const juce::XmlElement& element)
{
    for (auto index = 0; index < element.getNumAttributes(); ++index)
    {
        const auto name = element.getAttributeName(index);
        if (name == "x:id" || name == "x_id" || name == "id")
            return element.getAttributeValue(index);
    }
    return {};
}

const juce::XmlElement* findElement(
    const juce::XmlElement& parent,
    const juce::String& tag,
    const juce::String& id = {})
{
    if (parent.hasTagName(tag)
        && (id.isEmpty() || objectId(parent) == id))
        return &parent;
    for (const auto* child : parent.getChildIterator())
        if (const auto* found = findElement(*child, tag, id))
            return found;
    return nullptr;
}

const juce::XmlElement* childWithId(
    const juce::XmlElement& parent,
    const juce::String& tag,
    const juce::String& id)
{
    for (const auto* child : parent.getChildIterator())
        if (child->hasTagName(tag) && objectId(*child) == id)
            return child;
    return nullptr;
}

juce::String normalizedId(juce::String value)
{
    value = value.trim();
    if (value.containsChar('{') && value.containsChar('}'))
        value = value.fromFirstOccurrenceOf("{", false, false)
            .upToFirstOccurrenceOf("}", false, false);
    else
        value = value.upToFirstOccurrenceOf("/", false, false);
    if (value.length() == 36 && value[8] == '-' && value[13] == '-'
        && value[18] == '-' && value[23] == '-'
        && value.containsOnly("0123456789abcdefABCDEF-"))
        value = value.removeCharacters("-");
    return value.toLowerCase();
}

bool xmlWithinLimits(const juce::String& text)
{
    const auto bytes = text.toStdString();
    std::size_t cursor = 0;
    std::size_t nodes = 0;
    int depth = 0;
    while ((cursor = bytes.find('<', cursor)) != std::string::npos)
    {
        const auto skip = [&](const char* prefix, const char* suffix)
        {
            if (bytes.compare(cursor, std::strlen(prefix), prefix) != 0)
                return false;
            const auto end = bytes.find(suffix, cursor + std::strlen(prefix));
            cursor = end == std::string::npos ? bytes.size()
                : end + std::strlen(suffix);
            return true;
        };
        if (skip("<!--", "-->") || skip("<![CDATA[", "]]>") || skip("<?", "?>"))
            continue;
        if (cursor + 1 >= bytes.size() || bytes[cursor + 1] == '!')
            return false;
        const auto closing = bytes[cursor + 1] == '/';
        auto end = cursor + 1;
        char quote = 0;
        for (; end < bytes.size(); ++end)
        {
            const auto character = bytes[end];
            if (quote != 0)
            {
                if (character == quote)
                    quote = 0;
            }
            else if (character == '"' || character == '\'')
                quote = character;
            else if (character == '>')
                break;
        }
        if (end == bytes.size())
            return false;
        if (closing)
        {
            if (--depth < 0)
                return false;
        }
        else
        {
            if (++nodes > 200000)
                return false;
            if (bytes[end - 1] != '/' && ++depth > 128)
                return false;
        }
        cursor = end + 1;
    }
    return depth == 0;
}

class SongImporter
{
public:
    SongImporter(const juce::File& sourceToUse,
                 const juce::File& destinationToUse,
                 bool allowPartialToUse)
        : source(sourceToUse),
          destination(ProjectFile::normalisePackagePath(destinationToUse)),
          archive(source, "Studio One song"),
          allowPartial(allowPartialToUse)
    {
        report.format = "Studio One song";
        report.operation = "import";
        report.source = source.getFullPathName();
        report.destination = destination.getFullPathName();
        report.createdAt = juce::Time::getCurrentTime().toISO8601(true);
        formats.registerBasicFormats();
    }

    ProjectImportResult run()
    {
        if (const auto validation = archive.validate(
                { "Song/song.xml", "metainfo.xml", "Song/mediapool.xml" }, true);
            validation.failed())
            fail("import.archive", "/", validation.getErrorMessage());
        if (destination.exists())
            fail("import.destination-exists", "/", "Choose a new Studio Duo project path; the destination already exists.");
        if (failed())
            return finish();

        const auto song = readXml("Song/song.xml");
        const auto metadata = readXml("metainfo.xml");
        const auto pool = readXml("Song/mediapool.xml");
        const auto mixer = archive.has("Devices/audiomixer.xml")
            ? readXml("Devices/audiomixer.xml") : nullptr;
        if (failed())
            return finish();
        if (!song->hasTagName("Song"))
        {
            fail("import.song-xml", "/Song", "The selected archive does not contain a Studio One Song document.");
            return finish();
        }

        project.name = metadataValue(*metadata, "Document:Title");
        if (project.name.isEmpty())
            project.name = source.getFileNameWithoutExtension();
        const auto tempo = parseFiniteNumber(
            metadataValue(*metadata, "Media:Tempo"));
        if (!tempo.has_value() || *tempo < 20.0 || *tempo > 400.0)
        {
            fail("import.tempo", "/metainfo.xml", "Studio One tempo must be a finite value from 20 to 400 BPM.");
            return finish();
        }
        project.tempo = *tempo;
        project.metronomeEnabled = false;
        importMetadata(*metadata);
        validateTimeContext(*song);
        if (failed())
            return finish();

        std::map<juce::String, juce::String> mediaPaths;
        collectMedia(*pool, mediaPaths);
        const auto* tracks = findElement(*song, "List", "Tracks");
        if (tracks == nullptr)
        {
            fail("import.tracks", "/Song", "The Studio One song has no readable Tracks list.");
            return finish();
        }
        auto trackIndex = 0;
        for (const auto* element : tracks->getChildIterator())
        {
            if (element->hasTagName("MarkerTrack"))
            {
                importMarkers(*element);
                continue;
            }
            if (!element->hasTagName("MediaTrack")
                && !element->hasTagName("FolderTrack"))
            {
                const auto knownUtility = element->hasTagName("ChordTrack")
                    || element->hasTagName("LyricsTrack")
                    || element->hasTagName("VideoTrack")
                    || element->hasTagName("ArrangerTrack");
                auto hasContent = false;
                for (const auto* child : element->getChildIterator())
                    if (!child->hasTagName("UID")
                        && !(child->hasTagName("Attributes") && objectId(*child) == "attributes")
                        && !(child->hasTagName("List") && child->getNumChildElements() == 0))
                        hasContent = true;
                if (!element->isTextElement() && (!knownUtility || hasContent))
                    unsupported("unsupported.track", "/Song/Tracks/" + element->getTagName(),
                                "Native track '" + element->getStringAttribute("name", element->getTagName())
                                    + "' is not translated. Use DAWproject or rendered audio.");
                continue;
            }
            const auto externalId = element->getStringAttribute(
                "trackID", "track-" + juce::String(++trackIndex));
            const auto path = "/Song/Tracks/Track[" + externalId + "]";
            Track track;
            track.id = importedId("track", externalId, path);
            track.name = element->getStringAttribute("name", "Track");
            track.colour = importColour(*element, track.colour, path);
            track.type = element->hasTagName("FolderTrack")
                ? TrackType::folder
                : element->getStringAttribute("mediaType") == "Music"
                    ? TrackType::instrument : TrackType::audio;
            track.folderTrackId = element->getStringAttribute("parentFolder");
            if (!trackIds.emplace(normalizedId(externalId), track.id).second)
                fail("import.track-id", path, "The Studio One song contains a duplicate track ID.");
            if (const auto* channel = childWithId(*element, "UID", "channelID"))
            {
                channelTrackIds.emplace(
                    normalizedId(channel->getStringAttribute("uid")), track.id);
                if (mixer == nullptr)
                    fail("import.mixer-missing", path,
                         "This native track references a mixer document that is missing. Export DAWproject instead.");
            }
            if (const auto* events = findElement(*element, "List", "Events"))
            {
                auto eventIndex = 0;
                for (const auto* event : events->getChildIterator())
                {
                    const auto eventPath = path + "/" + event->getTagName()
                        + "[" + juce::String(++eventIndex) + "]";
                    if (event->hasTagName("AudioEvent"))
                        importAudio(*event, mediaPaths, track, eventPath);
                    else if (event->hasTagName("MusicPart"))
                        unsupported(
                            "unsupported.native-midi",
                            eventPath,
                            "Native MIDI part '" + event->getStringAttribute("name")
                                + "' uses Studio One's private music data. Export DAWproject to transfer its notes.");
                    else if (!event->isTextElement())
                        unsupported("unsupported.event", eventPath,
                                    "Native event '" + event->getTagName()
                                        + "' is not translated. Use DAWproject or rendered audio.");
                }
            }
            if (findElement(*element, "AutomationRegion") != nullptr)
                unsupported("unsupported.automation", path + "/AutomationRegion",
                            "Native Studio One automation is not translated. Export DAWproject to preserve it.");
            project.tracks.push_back(std::move(track));
        }
        resolveFolders();
        if (mixer != nullptr)
            importMixer(*mixer);
        if (failed())
            return finish();
        ensureMaster();
        if (mixer != nullptr)
            resolveMixerRoutes(*mixer);
        juce::String routingError;
        if (!RoutingGraph::validate(project, routingError))
            fail("import.routing", "/Devices/audiomixer.xml", routingError);
        if (failed())
            return finish();
        if (hasUnsupportedContent && !allowPartial)
        {
            ProjectImportResult output;
            output.result = juce::Result::fail(
                "This native song contains data that requires DAWproject for transfer. "
                "Review the compatibility report before explicitly importing only supported content.");
            output.report = report;
            output.requiresCompatibilityConfirmation = true;
            return output;
        }
        project.compatibilityReports.push_back(report);

        juce::String error;
        const auto collection = ProjectCollectionService::savePortableCopy(
            project, source.getParentDirectory(), destination, error);
        if (!collection.has_value())
        {
            fail("import.native-save", "/Project", error);
            return finish();
        }
        auto reopened = ProjectFile::load(destination, error);
        if (!reopened.has_value())
        {
            fail("import.native-verify", "/Project", error);
            if (!destination.deleteRecursively())
                fail("import.cleanup", "/Project", "Could not remove the failed imported project.");
            return finish();
        }
        ProjectImportResult output;
        output.result = juce::Result::ok();
        output.project = std::move(reopened);
        output.package = destination;
        output.report = report;
        return output;
    }

private:
    juce::File source;
    juce::File destination;
    ProjectArchiveReader archive;
    juce::AudioFormatManager formats;
    CompatibilityReport report;
    Project project;
    DawProjectIdMapper ids;
    std::map<juce::String, juce::String> trackIds;
    std::map<juce::String, juce::String> channelTrackIds;
    bool allowPartial = false;
    bool hasUnsupportedContent = false;

    void fail(const juce::String& code,
              const juce::String& path,
              const juce::String& message)
    {
        report.issues.push_back({
            CompatibilitySeverity::error, code, path, message
        });
    }

    void warn(const juce::String& code,
              const juce::String& path,
              const juce::String& message)
    {
        report.issues.push_back({
            CompatibilitySeverity::warning, code, path, message
        });
    }

    void unsupported(const juce::String& code,
                     const juce::String& path,
                     const juce::String& message)
    {
        hasUnsupportedContent = true;
        warn(code, path, message);
    }

    juce::String importedId(const juce::String& kind,
                            const juce::String& externalId,
                            const juce::String& path)
    {
        juce::String error;
        const auto id = ids.importedId("studio-one-" + kind, externalId, error);
        if (error.isNotEmpty())
            fail("import.id", path, error);
        return id;
    }

    std::optional<double> number(
        const juce::XmlElement& element,
        const juce::String& name,
        double fallback,
        double minimum,
        double maximum,
        const juce::String& path)
    {
        const auto value = parseFiniteNumber(
            element.getStringAttribute(name, juce::String(fallback)));
        if (!value.has_value() || *value < minimum || *value > maximum)
        {
            fail("import.number", path, "Studio One '" + name + "' has an invalid numeric value.");
            return std::nullopt;
        }
        return value;
    }

    bool flag(const juce::XmlElement& element,
              const juce::String& name,
              const juce::String& path,
              bool allowNonzero = false)
    {
        const auto value = number(element, name, 0.0, 0.0,
                                  allowNonzero ? 127.0 : 1.0, path);
        if (value.has_value() && *value != std::floor(*value))
            fail("import.flag", path, "Studio One '" + name + "' must be an integer flag.");
        return value.has_value() && *value != 0.0;
    }

    void importMetadata(const juce::XmlElement& metadata)
    {
        project.metadata.artist = metadataValue(metadata, "Media:Artist");
        project.metadata.album = metadataValue(metadata, "Media:Album");
        project.metadata.comment = metadataValue(metadata, "Media:Comment");
        const auto numerator = parseFiniteNumber(
            metadataValue(metadata, "Media:TimeSignatureNumerator"));
        const auto denominator = parseFiniteNumber(
            metadataValue(metadata, "Media:TimeSignatureDenominator"));
        if (!numerator.has_value() || !denominator.has_value()
            || *numerator < 1.0 || *numerator > 64.0
            || *denominator < 1.0 || *denominator > 32.0
            || std::floor(*numerator) != *numerator
            || std::floor(*denominator) != *denominator)
        {
            fail("import.meter", "/metainfo.xml", "The Studio One song has an invalid time signature.");
            return;
        }
        const auto meterDenominator = static_cast<int>(*denominator);
        if ((meterDenominator & (meterDenominator - 1)) != 0)
        {
            fail("import.meter", "/metainfo.xml", "The Studio One meter denominator must be a power of two.");
            return;
        }
        project.timeSignatureNumerator = static_cast<int>(*numerator);
        project.timeSignatureDenominator = meterDenominator;
    }

    void validateTimeContext(const juce::XmlElement& song)
    {
        const auto* tempoMap = findElement(song, "TempoMap");
        if (tempoMap != nullptr)
        {
            const juce::XmlElement* segment = nullptr;
            auto count = 0;
            for (const auto* child : tempoMap->getChildIterator())
                if (child->hasTagName("TempoMapSegment"))
                {
                    segment = child;
                    ++count;
                }
            const auto start = segment != nullptr
                ? parseFiniteNumber(segment->getStringAttribute("start", "0")) : std::nullopt;
            const auto curve = segment != nullptr
                ? parseFiniteNumber(segment->getStringAttribute("curveType", "0")) : std::nullopt;
            if (count != 1 || segment == nullptr
                || !start.has_value() || *start != 0.0
                || !curve.has_value() || *curve != 0.0)
            {
                fail("unsupported.tempo-map", "/Song/TempoMap",
                     "Native tempo changes or curves cannot be translated safely. Export DAWproject to preserve their timing.");
                return;
            }
            const auto secondsPerBeat = parseFiniteNumber(segment->getStringAttribute("tempo"));
            if (!secondsPerBeat.has_value() || *secondsPerBeat < 0.15 || *secondsPerBeat > 3.0)
                fail("import.tempo", "/Song/TempoMap", "The native tempo segment has an invalid seconds-per-beat value.");
            else
                project.tempo = 60.0 / *secondsPerBeat;
        }
        const auto* meterMap = findElement(song, "TimeSignatureMap");
        if (meterMap != nullptr)
        {
            auto count = 0;
            for (const auto* segment : meterMap->getChildIterator())
                if (segment->hasTagName("TimeSignatureMapSegment"))
                {
                    ++count;
                    const auto start = parseFiniteNumber(segment->getStringAttribute("start", "0"));
                    const auto numerator = parseFiniteNumber(segment->getStringAttribute("numerator"));
                    const auto denominator = parseFiniteNumber(segment->getStringAttribute("denominator"));
                    const auto sameMeter = numerator.has_value() && denominator.has_value()
                        && *numerator >= 1.0 && *numerator <= 64.0
                        && *denominator >= 1.0 && *denominator <= 32.0
                        && std::floor(*numerator) == *numerator
                        && std::floor(*denominator) == *denominator
                        && static_cast<int>(*numerator) == project.timeSignatureNumerator
                        && static_cast<int>(*denominator) == project.timeSignatureDenominator;
                    if (!start.has_value() || *start != 0.0
                        || !sameMeter)
                        fail("unsupported.meter-map", "/Song/TimeSignatureMap",
                             "Native meter changes require DAWproject to preserve their timing.");
                }
            if (count != 1)
                fail("unsupported.meter-map", "/Song/TimeSignatureMap",
                     "Native meter changes require DAWproject to preserve their timing.");
        }
    }

    juce::Colour importColour(const juce::XmlElement& element,
                               juce::Colour fallback,
                               const juce::String& path)
    {
        if (!element.hasAttribute("color"))
            return fallback;
        const auto value = element.getStringAttribute("color").trim();
        if (value.length() != 8 || !value.containsOnly("0123456789abcdefABCDEF"))
        {
            warn("unsupported.colour", path, "The native Studio One colour could not be decoded.");
            return fallback;
        }
        return juce::Colour::fromString(value);
    }

    void importMarkers(const juce::XmlElement& track)
    {
        const auto* events = childWithId(track, "List", "Events");
        if (events == nullptr)
            events = &track;
        auto index = 0;
        for (const auto* event : events->getChildIterator())
        {
            if (!event->hasTagName("MarkerEvent"))
                continue;
            const auto path = "/Song/MarkerTrack/MarkerEvent[" + juce::String(++index) + "]";
            const auto start = parseFiniteNumber(event->getStringAttribute("start", "0"));
            const auto timeFormat = event->getStringAttribute(
                "timeFormat", track.getStringAttribute("timeFormat", "2"));
            if (!start.has_value() || *start < 0.0
                || (timeFormat != "0" && timeFormat != "2"))
            {
                fail("import.marker-time", path, "The Studio One marker has an invalid or unsupported time position.");
                continue;
            }
            ProjectMarker marker;
            marker.id = importedId(
                "marker", track.getStringAttribute("trackID", "markers") + ":" + juce::String(index), path);
            marker.name = event->getStringAttribute("name", "Marker");
            marker.timeSeconds = timeFormat == "2"
                ? project.secondsAtBeat(*start) : *start;
            if (!std::isfinite(marker.timeSeconds))
            {
                fail("import.marker-time", path, "The native marker position exceeds supported numeric bounds.");
                continue;
            }
            if (flag(*event, "markerStop", path))
                unsupported("unsupported.marker-stop", path, "Native stop-at-marker behavior is not transferred.");
            project.markers.push_back(std::move(marker));
        }
        std::stable_sort(
            project.markers.begin(), project.markers.end(),
            [](const auto& left, const auto& right) { return left.timeSeconds < right.timeSeconds; });
    }

    void resolveFolders()
    {
        for (auto& track : project.tracks)
        {
            if (track.folderTrackId.isEmpty())
                continue;
            const auto found = trackIds.find(normalizedId(track.folderTrackId));
            if (found == trackIds.end())
                fail("import.folder", "/Song/Tracks/" + track.name,
                     "The Studio One track references a missing folder.");
            else
                track.folderTrackId = found->second;
        }
    }

    static std::vector<const juce::XmlElement*> mixerChannels(
        const juce::XmlElement& element,
        bool inChannelGroup = false)
    {
        std::vector<const juce::XmlElement*> result;
        if (element.hasTagName("AudioTrackChannel")
            || element.hasTagName("AudioGroupChannel")
            || element.hasTagName("AudioEffectChannel")
            || element.hasTagName("AudioOutputChannel")
            || element.hasTagName("AudioSynthChannel")
            || element.hasTagName("AudioAuxChannel")
            || element.hasTagName("AudioVCAChannel")
            || element.hasTagName("AudioListenBusChannel")
            || (inChannelGroup
                && element.getTagName().endsWith("Channel")
                && !element.hasTagName("AudioInputChannel")))
            result.push_back(&element);
        for (const auto* child : element.getChildIterator())
        {
            auto nested = mixerChannels(*child, element.hasTagName("ChannelGroup"));
            result.insert(result.end(), nested.begin(), nested.end());
        }
        return result;
    }

    static juce::String channelId(const juce::XmlElement& channel)
    {
        const auto* id = childWithId(channel, "UID", "uniqueID");
        return id != nullptr ? normalizedId(id->getStringAttribute("uid")) : juce::String();
    }

    void importNativeInserts(
        const juce::XmlElement& channel,
        Track& track,
        const juce::String& path)
    {
        for (const auto* section : channel.getChildIterator())
        {
            const auto stage = objectId(*section);
            if (!section->hasTagName("Attributes")
                || (stage != "InputFX" && stage != "Inserts" && stage != "PostFaderInserts"))
                continue;
            auto index = 0;
            for (const auto* slot : section->getChildIterator())
            {
                const auto* descriptor = childWithId(*slot, "Attributes", "deviceData");
                if (descriptor == nullptr && !slot->getStringAttribute("name").startsWithIgnoreCase("FX"))
                    continue;
                const auto slotPath = path + "/" + stage + "/"
                    + slot->getStringAttribute("name", "FX" + juce::String(++index));
                PluginInsert insert;
                insert.id = importedId("plugin", slotPath, slotPath);
                insert.pluginIdentifier = "studio-one-native:" + insert.id;
                insert.name = descriptor != nullptr
                    ? descriptor->getStringAttribute("name", "Studio One device")
                    : juce::String("Studio One device");
                insert.format = "Studio One native";
                insert.stateFormat = PluginStateFormat::generic;
                insert.bypassed = flag(*slot, "bypass", slotPath);
                insert.missing = true;
                track.inserts.push_back(insert);
                unsupported(
                    "unsupported.plugin-state", slotPath,
                    "Native processor '" + insert.name
                        + "' was retained as a missing descriptor, not restored. "
                        "Use DAWproject or rendered audio to transfer its sound.");
            }
        }
    }

    void importMixer(const juce::XmlElement& mixer)
    {
        auto hasMaster = false;
        for (const auto* channel : mixerChannels(mixer))
        {
            const auto externalId = channelId(*channel);
            const auto path = "/Devices/audiomixer.xml/Channel[" + externalId + "]";
            if (externalId.isEmpty())
            {
                fail("import.channel-id", path, "The Studio One mixer channel has no unique ID.");
                continue;
            }
            const auto existing = channelTrackIds.find(externalId);
            const auto* linked = existing != channelTrackIds.end()
                ? project.findTrack(existing->second) : nullptr;
            if (linked == nullptr || linked->type == TrackType::folder)
            {
                Track track;
                track.id = importedId("channel", externalId, path);
                track.name = channel->getStringAttribute(
                    "label", channel->getStringAttribute("name", "Channel"));
                track.type = channel->hasTagName("AudioGroupChannel")
                    ? TrackType::bus
                    : channel->hasTagName("AudioEffectChannel") || channel->hasTagName("AudioAuxChannel")
                        ? TrackType::aux
                        : channel->hasTagName("AudioOutputChannel") && !hasMaster
                            ? TrackType::master
                            : channel->hasTagName("AudioOutputChannel")
                                ? TrackType::bus
                                : channel->hasTagName("AudioSynthChannel")
                                    ? TrackType::instrument : TrackType::audio;
                if (track.type == TrackType::master)
                    hasMaster = true;
                if (linked != nullptr && linked->type == TrackType::folder)
                    track.folderTrackId = linked->id;
                channelTrackIds[externalId] = track.id;
                project.tracks.push_back(std::move(track));
            }
            auto* track = project.findTrack(channelTrackIds.at(externalId));
            if (track == nullptr)
            {
                fail("import.channel", path, "The Studio One mixer channel could not be mapped to a track.");
                continue;
            }
            const auto gain = number(*channel, "gain", 1.0, 0.0, 4.0, path);
            const auto pan = number(*channel, "pan", 0.5, 0.0, 1.0, path);
            if (gain.has_value())
            {
                const auto decibels = juce::Decibels::gainToDecibels(*gain, -60.0);
                track->volumeDecibels = static_cast<float>(juce::jlimit(-60.0, 12.0, decibels));
                if (decibels > 12.0 || (*gain > 0.0 && decibels <= -60.0))
                    unsupported("unsupported.mixer-gain", path, "The mixer gain was bounded to Studio Duo's fader range.");
            }
            if (pan.has_value())
                track->pan = static_cast<float>(*pan * 2.0 - 1.0);
            track->colour = importColour(*channel, track->colour, path);
            track->muted = flag(*channel, "mute", path)
                || flag(*channel, "disabled", path)
                || (gain.has_value() && *gain == 0.0);
            track->solo = flag(*channel, "solo", path, true);
            track->soloSafe = flag(*channel, "soloSafe", path);
            if (const auto* speaker = childWithId(*channel, "SpeakerSetup", "speakerType"))
            {
                const auto type = speaker->getStringAttribute("type");
                if (type == "Mono")
                    track->channelLayout = ChannelLayout::mono;
                else if (type == "Stereo")
                    track->channelLayout = ChannelLayout::stereo;
                else
                    unsupported("unsupported.speaker-layout", path, "The Studio One speaker layout is not supported.");
            }
            if (channel->hasTagName("AudioSynthChannel"))
                unsupported("unsupported.native-instrument", path,
                            "Native instrument '" + track->name
                                + "' cannot be restored from Studio One's private device data. Use DAWproject or rendered audio.");
            else if (!channel->hasTagName("AudioTrackChannel")
                     && !channel->hasTagName("AudioGroupChannel")
                     && !channel->hasTagName("AudioEffectChannel")
                     && !channel->hasTagName("AudioAuxChannel")
                     && !channel->hasTagName("AudioOutputChannel"))
                unsupported("unsupported.mixer-channel", path,
                            "This native mixer channel type has no direct Studio Duo equivalent: " + channel->getTagName());
            importNativeInserts(*channel, *track, path);
        }
    }

    void ensureMaster()
    {
        if (std::any_of(project.tracks.begin(), project.tracks.end(),
                        [](const auto& track) { return track.type == TrackType::master; }))
            return;
        Track master;
        master.id = importedId("channel", "synthesized-master", "/Song");
        master.name = "Master";
        master.type = TrackType::master;
        project.tracks.push_back(std::move(master));
    }

    juce::String destinationId(const juce::XmlElement& connection,
                               const juce::String& path)
    {
        const auto id = normalizedId(connection.getStringAttribute("objectID"));
        const auto found = channelTrackIds.find(id);
        if (found == channelTrackIds.end())
        {
            fail("import.routing-destination", path,
                 "The Studio One mixer references an unavailable destination channel: " + id);
            return {};
        }
        return found->second;
    }

    void resolveMixerRoutes(const juce::XmlElement& mixer)
    {
        for (const auto* channel : mixerChannels(mixer))
        {
            const auto externalId = channelId(*channel);
            const auto found = channelTrackIds.find(externalId);
            if (found == channelTrackIds.end())
                continue;
            auto* track = project.findTrack(found->second);
            const auto path = "/Devices/audiomixer.xml/Channel[" + externalId + "]";
            if (track != nullptr && track->type != TrackType::master)
                if (const auto* connection = childWithId(*channel, "Connection", "destination"))
                    track->outputTrackId = destinationId(*connection, path);
            const auto* sends = childWithId(*channel, "Attributes", "Sends");
            if (track == nullptr || sends == nullptr)
                continue;
            auto sendIndex = 0;
            for (const auto* send : sends->getChildIterator())
            {
                const auto* connection = childWithId(*send, "Connection", "destination");
                if (connection == nullptr)
                {
                    if (send->getStringAttribute("name").startsWithIgnoreCase("Send"))
                        unsupported("unsupported.send", path,
                                    "A native send has no translatable destination.");
                    continue;
                }
                const auto sendPath = path + "/Send[" + juce::String(++sendIndex) + "]";
                const auto gain = number(*send, "level", 1.0, 0.0, 4.0, sendPath);
                const auto pan = number(*send, "pan", 0.5, 0.0, 1.0, sendPath);
                RoutingConnection route;
                route.id = importedId("route", externalId + ":" + juce::String(sendIndex), sendPath);
                route.name = send->getStringAttribute("name", "Send");
                route.kind = RouteKind::send;
                route.sourceTrackId = track->id;
                route.destination.trackId = destinationId(*connection, sendPath);
                route.tap = flag(*send, "prefader", sendPath) ? RouteTap::preFader : RouteTap::postFader;
                route.enabled = !flag(*send, "bypass", sendPath);
                route.muted = gain.has_value() && *gain == 0.0;
                if (gain.has_value())
                    route.gainDecibels = static_cast<float>(juce::Decibels::gainToDecibels(*gain, -60.0));
                if (pan.has_value())
                    route.pan = static_cast<float>(*pan * 2.0 - 1.0);
                project.routingConnections.push_back(std::move(route));
            }
        }
    }

    bool failed() const
    {
        return report.hasErrors();
    }

    ProjectImportResult finish() const
    {
        ProjectImportResult output;
        output.report = report;
        const auto error = std::find_if(
            report.issues.begin(), report.issues.end(),
            [](const auto& issue)
            {
                return issue.severity == CompatibilitySeverity::error;
            });
        output.result = juce::Result::fail(
            error != report.issues.end()
                ? error->message
                : juce::String("Studio One import failed."));
        return output;
    }

    std::unique_ptr<juce::XmlElement> readXml(const juce::String& path)
    {
        juce::String error;
        const auto xmlText = archive.readText(path, error, 16 * 1024 * 1024);
        if (!xmlText.has_value())
        {
            fail("import.xml", "/" + path, error);
            return {};
        }
        const auto& text = *xmlText;
        if (text.containsIgnoreCase("<!DOCTYPE") || text.containsIgnoreCase("<!ENTITY"))
        {
            fail("import.xml", "/" + path, "Studio One XML cannot contain DTDs or entities.");
            return {};
        }
        if (!xmlWithinLimits(text))
        {
            fail("import.xml-limits", "/" + path,
                 "Studio One XML is malformed or exceeds the supported depth or element count.");
            return {};
        }
        juce::XmlDocument document(text);
        auto xml = document.getDocumentElement();
        if (xml == nullptr)
            fail("import.xml", "/" + path, "Could not parse Studio One XML: " + document.getLastParseError());
        return xml;
    }

    static juce::String metadataValue(
        const juce::XmlElement& metadata,
        const juce::String& id)
    {
        const auto* attribute = findElement(metadata, "Attribute", id);
        return attribute != nullptr ? attribute->getStringAttribute("value") : juce::String();
    }

    void collectMedia(const juce::XmlElement& element,
                      std::map<juce::String, juce::String>& mediaPaths)
    {
        if (element.hasTagName("AudioClip"))
        {
            const auto id = normalizedId(element.getStringAttribute("mediaID"));
            const auto* path = findElement(element, "Url", "path");
            if (id.isEmpty() || path == nullptr
                || !mediaPaths.emplace(id, path->getStringAttribute("url")).second)
                fail("import.media-id", "/Song/mediapool.xml/AudioClip[" + id + "]",
                     "Native Studio One media IDs must be present, unique and associated with a path.");
        }
        for (const auto* child : element.getChildIterator())
            collectMedia(*child, mediaPaths);
    }

    juce::File resolveMediaFile(
        const juce::String& storedPath,
        const juce::String& objectPath)
    {
        auto path = storedPath.replaceCharacter('\\', '/');
        if (path.startsWithIgnoreCase("file://"))
        {
            path = path.substring(7);
            if (path.startsWithIgnoreCase("localhost/"))
                path = path.substring(9);
            if (!path.startsWithChar('/'))
            {
                fail("import.media-path", objectPath, "Only local file URLs are supported for native Studio One media.");
                return {};
            }
        }
        else if (path.contains("://"))
        {
            fail("import.media-path", objectPath, "Network media URLs are not supported; export a self-contained DAWproject instead.");
            return {};
        }
        path = juce::URL::removeEscapeChars(path.replace("+", "%2B"))
            .replaceCharacter('\\', '/');

        juce::String relativeMedia;
        if (path.startsWithIgnoreCase("Media/"))
            relativeMedia = path;
        else if (const auto position = path.lastIndexOfIgnoreCase("/Media/");
                 position >= 0)
            relativeMedia = "Media/" + path.substring(position + 7);
        if (relativeMedia.isNotEmpty())
        {
            if (!project_archive::safeArchivePath(relativeMedia))
            {
                fail("import.media-path", objectPath, "The native Studio One media path contains unsafe traversal.");
                return {};
            }
            const auto current = source.getParentDirectory().getChildFile(relativeMedia);
            if (current.existsAsFile())
            {
                if (path != relativeMedia && path != current.getFullPathName())
                    warn("import.media-relocated", objectPath,
                         "Resolved the stored media reference from the song's current Media folder.");
                return current;
            }
        }
#if JUCE_WINDOWS
        if (path.length() >= 3 && path[0] == '/' && path[2] == ':')
            path = path.substring(1);
#else
        if ((path.length() >= 3 && path[0] == '/' && path[2] == ':')
            || (path.length() >= 2 && path[1] == ':'))
        {
            fail("import.media-missing", objectPath,
                 "The Windows media path is unavailable. Copy the original Media folder beside the song, or export DAWproject.");
            return {};
        }
#endif
        if (juce::File::isAbsolutePath(path))
            return juce::File(path);
        if (!project_archive::safeArchivePath(path))
        {
            fail("import.media-path", objectPath, "The native Studio One media path is empty or unsafe.");
            return {};
        }
        return source.getParentDirectory().getChildFile(path);
    }

    void importAudio(const juce::XmlElement& event,
                     const std::map<juce::String, juce::String>& mediaPaths,
                     Track& track,
                     const juce::String& path)
    {
        const auto media = mediaPaths.find(normalizedId(event.getStringAttribute("clipID")));
        if (media == mediaPaths.end())
        {
            fail("import.media-reference", path, "The Studio One audio event references unavailable media.");
            return;
        }
        for (const auto* name : { "transpose", "tune", "normalize", "modification" })
        {
            if (!event.hasAttribute(name))
                continue;
            const auto value = parseFiniteNumber(event.getStringAttribute(name));
            if (!value.has_value())
                fail("import.number", path, "The native audio-processing value is invalid: " + juce::String(name));
            else if (*value != 0.0)
                unsupported("unsupported.audio-processing", path,
                            "Native audio processing '" + juce::String(name)
                                + "' is not translated. Use DAWproject or render this event in Studio One.");
        }
        const juce::StringArray knownAttributes {
            "clipID", "start", "length", "timeFormat", "offset", "name", "speed",
            "color", "mute", "muted", "transpose", "tune", "normalize", "modification",
            "iid", "selected"
        };
        for (auto index = 0; index < event.getNumAttributes(); ++index)
            if (!knownAttributes.contains(event.getAttributeName(index)))
                unsupported("unsupported.audio-processing", path + "/@" + event.getAttributeName(index),
                            "This native audio event attribute is not translated: " + event.getAttributeName(index));
        for (const auto* child : event.getChildIterator())
            if (!child->isTextElement())
                unsupported("unsupported.audio-processing", path + "/" + child->getTagName(),
                            "The native audio event's additional processing is not translated.");
        const auto start = parseFiniteNumber(event.getStringAttribute("start", "0"));
        const auto length = parseFiniteNumber(event.getStringAttribute("length"));
        const auto offset = parseFiniteNumber(event.getStringAttribute("offset", "0"));
        const auto speed = number(event, "speed", 1.0, 0.25, 4.0, path);
        const auto timeFormat = event.getStringAttribute("timeFormat", "0");
        if (!start.has_value() || !length.has_value() || !offset.has_value() || !speed.has_value()
            || *start < 0.0 || *length <= 0.0 || *offset < 0.0
            || (timeFormat != "0" && timeFormat != "2"))
        {
            fail("import.event-time", path, "The Studio One audio event has an unsupported or invalid time range.");
            return;
        }
        const auto mediaFile = resolveMediaFile(media->second, path);
        if (mediaFile == juce::File())
            return;
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(mediaFile));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            fail("import.media-missing", path, "Studio One audio is missing or unreadable: " + mediaFile.getFullPathName());
            return;
        }
        AudioClip clip;
        clip.name = event.getStringAttribute("name", mediaFile.getFileNameWithoutExtension());
        clip.sourceFile = mediaFile;
        clip.sourceHash = juce::SHA256(mediaFile).toHexString();
        const auto unit = timeFormat == "2" ? 60.0 / project.tempo : 1.0;
        clip.startSeconds = *start * unit;
        clip.durationSeconds = *length * unit;
        clip.sourceOffsetSeconds = *offset;
        clip.playbackRate = *speed;
        clip.colour = importColour(event, track.colour, path);
        clip.muted = flag(event, "mute", path) || flag(event, "muted", path);
        clip.sourceLengthSeconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        clip.sourceRangeEndSeconds = clip.sourceLengthSeconds;
        if (!std::isfinite(clip.startSeconds) || !std::isfinite(clip.durationSeconds)
            || clip.sourceOffsetSeconds + clip.durationSeconds * clip.playbackRate
                > clip.sourceLengthSeconds + std::max(0.0001, 1.0 / reader->sampleRate))
        {
            fail("import.audio-bounds", path, "The native audio event exceeds the source audio's valid bounds.");
            return;
        }
        track.channelLayout = reader->numChannels == 1 ? ChannelLayout::mono : ChannelLayout::stereo;
        track.clips.push_back(std::move(clip));
    }
};
}

ProjectImportResult StudioOneProjectIO::importSong(
    const juce::File& source,
    const juce::File& destination,
    bool allowPartialImport)
{
    return SongImporter(source, destination, allowPartialImport).run();
}
}
