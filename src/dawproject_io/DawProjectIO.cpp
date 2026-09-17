#include "DawProjectIO.h"

#include "DawProjectIdMapper.h"
#include "DawProjectSchemaValidator.h"
#include "devices/DeviceRegistry.h"
#include "plugin_host/PluginStateStore.h"
#include "project_io/ProjectFile.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>

namespace studio
{
namespace
{
constexpr auto formatName = "DAWproject 1.0";
constexpr auto projectEntryName = "project.xml";
constexpr auto metadataEntryName = "metadata.xml";
constexpr auto maxXmlBytes = static_cast<juce::int64>(64 * 1024 * 1024);
constexpr auto maxArchiveBytes =
    static_cast<juce::int64>(0x7fffffff);

class Crc32
{
public:
    void update(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index)
            value = table()[(value ^ bytes[index]) & 0xff] ^ (value >> 8);
    }

    [[nodiscard]] std::uint32_t result() const noexcept
    {
        return value ^ 0xffffffffu;
    }

private:
    std::uint32_t value = 0xffffffffu;

    static const std::array<std::uint32_t, 256>& table()
    {
        static const auto values = []
        {
            std::array<std::uint32_t, 256> result {};
            for (std::uint32_t index = 0; index < result.size(); ++index)
            {
                auto crc = index;
                for (auto bit = 0; bit < 8; ++bit)
                    crc = (crc & 1u) != 0
                        ? 0xedb88320u ^ (crc >> 1)
                        : crc >> 1;
                result[index] = crc;
            }
            return result;
        }();
        return values;
    }
};

std::uint16_t readLittleEndian16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(
        bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t readLittleEndian32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(
        bytes[0]
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24));
}

juce::String formatNumber(double value)
{
    if (!std::isfinite(value))
        return "0";
    if (std::abs(value) < 0.0000005)
        value = 0.0;
    auto text = juce::String(value, 6);
    while (text.containsChar('.') && text.endsWithChar('0'))
        text = text.dropLastCharacters(1);
    if (text.endsWithChar('.'))
        text = text.dropLastCharacters(1);
    return text;
}

juce::String boolText(bool value)
{
    return value ? "true" : "false";
}

bool parseFiniteNumber(const juce::String& value, double& parsed)
{
    const auto text = value.trim().toStdString();
    if (text.empty())
        return false;
    char* end = nullptr;
    parsed = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size()
        && std::isfinite(parsed);
}

juce::String colourText(juce::Colour colour)
{
    return "#" + colour.toDisplayString(false).toUpperCase();
}

juce::Colour colourFromText(const juce::String& text,
                            juce::Colour fallback)
{
    auto value = text.trim();
    if (value.startsWithChar('#'))
        value = value.substring(1);
    if (value.length() == 6)
        value = "ff" + value;
    if (value.length() != 8)
        return fallback;
    return juce::Colour::fromString(value);
}

juce::String xmlText(const juce::XmlElement& element)
{
    juce::XmlElement::TextFormat format;
    format.customHeader =
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)";
    format.newLineChars = "\n";
    format.lineWrapLength = 0;
    return element.toString(format);
}

std::unique_ptr<juce::XmlElement> makeElement(const char* name)
{
    return std::make_unique<juce::XmlElement>(name);
}

juce::XmlElement* addElement(juce::XmlElement& parent, const char* name)
{
    return parent.createNewChildElement(name);
}

juce::XmlElement* addTextElement(juce::XmlElement& parent,
                                 const char* name,
                                 const juce::String& text)
{
    auto* child = addElement(parent, name);
    child->addTextElement(text);
    return child;
}

CompatibilityReport makeReport(const juce::String& operation,
                               const juce::File& source,
                               const juce::File& destination)
{
    CompatibilityReport report;
    report.format = formatName;
    report.operation = operation;
    report.source = source == juce::File()
        ? juce::String()
        : source.getFullPathName();
    report.destination = destination == juce::File()
        ? juce::String()
        : destination.getFullPathName();
    report.createdAt = juce::Time::getCurrentTime().toISO8601(true);
    return report;
}

void addIssue(CompatibilityReport& report,
              CompatibilitySeverity severity,
              const juce::String& code,
              const juce::String& objectPath,
              const juce::String& message)
{
    report.issues.push_back({
        severity,
        code,
        objectPath,
        message
    });
}

juce::String trackPath(const Track& track)
{
    return "/Project/Structure/Track[" + track.id + "]";
}

juce::String clipPath(const Track& track, const AudioClip& clip)
{
    return trackPath(track) + "/AudioClip[" + clip.id + "]";
}

juce::String midiClipPath(const Track& track, const MidiClip& clip)
{
    return trackPath(track) + "/MidiClip[" + clip.id + "]";
}

juce::String devicePath(const Track& track, const PluginInsert& insert)
{
    return trackPath(track) + "/Device[" + insert.id + "]";
}

juce::String routePath(const RoutingConnection& route)
{
    return "/Project/RoutingConnection[" + route.id + "]";
}

juce::String automationPath(const AutomationLane& lane)
{
    return "/Project/AutomationLane[" + lane.id + "]";
}

juce::String scenePath(const ProjectScene& scene)
{
    return "/Project/Scenes/Scene[" + scene.id + "]";
}

juce::String stateExtension(const PluginInsert& insert)
{
    switch (insert.stateFormat)
    {
        case PluginStateFormat::vst2Preset: return ".fxp";
        case PluginStateFormat::vst3Preset: return ".vstpreset";
        case PluginStateFormat::clapPreset: return ".clap-preset";
        case PluginStateFormat::auPreset: return ".aupreset";
        case PluginStateFormat::hostOpaque:
        case PluginStateFormat::generic:
            return ".bin";
    }
    return ".bin";
}

juce::String deviceElementName(const PluginInsert& insert)
{
    if (insert.bundledDevice)
        return "BuiltinDevice";
    const auto hasState =
        insert.stateFile.isNotEmpty() || insert.stateHash.isNotEmpty();
    if (hasState
        && insert.stateFormat == PluginStateFormat::hostOpaque
        && (insert.format.containsIgnoreCase("VST")
            || insert.format.containsIgnoreCase("AU")))
    {
        return "Device";
    }
    if (insert.format.containsIgnoreCase("CLAP"))
        return "ClapPlugin";
    if (insert.format.containsIgnoreCase("VST3"))
        return "Vst3Plugin";
    if (insert.format.containsIgnoreCase("VST"))
        return "Vst2Plugin";
    if (insert.format.containsIgnoreCase("AU"))
        return "AuPlugin";
    return "Device";
}

juce::String stretchAlgorithm(StretchMode mode)
{
    switch (mode)
    {
        case StretchMode::drums: return "studio-duo:drums";
        case StretchMode::monophonic: return "studio-duo:monophonic";
        case StretchMode::polyphonic: return "studio-duo:polyphonic";
        case StretchMode::mix: return "studio-duo:mix";
    }
    return {};
}

std::optional<StretchMode> stretchModeForAlgorithm(
    const juce::String& algorithm)
{
    if (algorithm == "studio-duo:drums") return StretchMode::drums;
    if (algorithm == "studio-duo:monophonic") return StretchMode::monophonic;
    if (algorithm == "studio-duo:polyphonic" || algorithm.isEmpty())
        return StretchMode::polyphonic;
    if (algorithm == "studio-duo:mix") return StretchMode::mix;
    return std::nullopt;
}

struct Payload
{
    juce::File snapshot;
    juce::int64 size = 0;
    std::uint32_t crc = 0;
};

#if STUDIO_DUO_TESTING
DawProjectIO::ExportTestHook& exportTestHook()
{
    static DawProjectIO::ExportTestHook hook;
    return hook;
}

void runExportTestHook(DawProjectIO::ExportTestPhase phase,
                       const juce::File& file)
{
    if (exportTestHook())
        exportTestHook()(phase, file);
}
#endif

struct AudioFileInfo
{
    juce::String archivePath;
    double durationSeconds = 0.0;
    int channels = 0;
    int sampleRate = 0;
};

class Exporter
{
public:
    Exporter(const Project& sourceProject,
             const juce::File& package,
             const juce::File& destination)
        : project(sourceProject),
          sourcePackage(package),
          destinationArchive(destination),
          report(makeReport("export", package, destination))
    {
        formats.registerBasicFormats();
    }

    ~Exporter()
    {
        if (payloadStagingDirectory.exists())
            payloadStagingDirectory.deleteRecursively();
    }

    DawProjectExportResult run()
    {
        DawProjectExportResult output;
        output.archive = destinationArchive;
        reportUnsupportedProjectData();
        auto metadata = buildMetadata();
        auto projectDocument = buildProject();
        if (fatal)
        {
            output.result = juce::Result::fail(
                firstError.isNotEmpty()
                    ? firstError
                    : juce::String("DAWproject export failed."));
            return finishRun(std::move(output));
        }

        const auto metadataString = xmlText(*metadata);
        const auto projectString = xmlText(*projectDocument);
        const auto metadataValidation =
            DawProjectSchemaValidator::validateMetadataXml(metadataString);
        const auto projectValidation =
            DawProjectSchemaValidator::validateProjectXml(projectString);
        appendSchemaIssues(
            metadataValidation,
            "schema.metadata",
            "/MetaData");
        appendSchemaIssues(
            projectValidation,
            "schema.project",
            "/Project");
        if (!metadataValidation.valid || !projectValidation.valid)
        {
            output.result = juce::Result::fail(
                "Generated DAWproject XML failed official schema validation.");
            return finishRun(std::move(output));
        }

        const auto writeResult = writeArchive(
            projectString,
            metadataString);
        if (writeResult.failed())
        {
            addIssue(
                report,
                CompatibilitySeverity::error,
                "export.archive",
                "/Archive",
                writeResult.getErrorMessage());
        }
        output.result = writeResult;
        return finishRun(std::move(output));
    }

private:
    const Project& project;
    juce::File sourcePackage;
    juce::File destinationArchive;
    CompatibilityReport report;
    DawProjectIdMapper ids;
    juce::AudioFormatManager formats;
    std::map<juce::String, Payload> payloads;
    std::map<juce::String, AudioFileInfo> audioInfo;
    juce::File payloadStagingDirectory;
    juce::int64 stagedPayloadBytes = 0;
    std::uint32_t nextPayloadSnapshot = 0;
    bool fatal = false;
    juce::String firstError;

    void fail(const juce::String& code,
              const juce::String& path,
              const juce::String& message)
    {
        addIssue(
            report,
            CompatibilitySeverity::error,
            code,
            path,
            message);
        fatal = true;
        if (firstError.isEmpty())
            firstError = message;
    }

    void warn(const juce::String& code,
              const juce::String& path,
              const juce::String& message)
    {
        addIssue(
            report,
            CompatibilitySeverity::warning,
            code,
            path,
            message);
    }

    std::optional<juce::File> createPayloadSnapshot(
        const juce::String& path)
    {
        if (payloadStagingDirectory == juce::File())
        {
            payloadStagingDirectory =
                destinationArchive.getSiblingFile(
                    destinationArchive.getFileName()
                    + ".payload-staging-"
                    + juce::Uuid().toString());
            if (!payloadStagingDirectory.createDirectory())
            {
                fail(
                    "export.payload-staging",
                    path,
                    "Could not create the immutable payload staging directory.");
                return std::nullopt;
            }
        }
        const auto snapshot = payloadStagingDirectory.getChildFile(
            "payload-"
            + juce::String(++nextPayloadSnapshot).paddedLeft('0', 8)
            + ".bin");
        if (snapshot.exists())
            snapshot.deleteRecursively();
        return snapshot;
    }

    bool stageMemoryPayload(const juce::String& archivePath,
                            const juce::MemoryBlock& data,
                            const juce::String& objectPath)
    {
        const auto size =
            static_cast<juce::int64>(data.getSize());
        if (size < 0
            || size > maxArchiveBytes
            || stagedPayloadBytes > maxArchiveBytes - size)
        {
            fail(
                "export.payload-snapshot-too-large",
                objectPath,
                "The immutable payload snapshots exceed the current 2 GiB classic-ZIP limit.");
            return false;
        }
        const auto snapshot = createPayloadSnapshot(objectPath);
        if (!snapshot.has_value())
            return false;
        auto output = snapshot->createOutputStream();
        if (output == nullptr
            || !output->write(data.getData(), data.getSize()))
        {
            if (output != nullptr)
                output.reset();
            snapshot->deleteFile();
            fail(
                "export.payload-snapshot",
                objectPath,
                "Could not write an immutable payload snapshot.");
            return false;
        }
        output->flush();
        const auto status = output->getStatus();
        output.reset();
        if (status.failed()
            || !snapshot->existsAsFile()
            || snapshot->getSize() != size)
        {
            snapshot->deleteFile();
            fail(
                "export.payload-snapshot",
                objectPath,
                "The immutable payload snapshot could not be completed.");
            return false;
        }
        Crc32 crc;
        crc.update(data.getData(), data.getSize());
        payloads[archivePath] = {
            *snapshot,
            size,
            crc.result()
        };
        stagedPayloadBytes += size;
        return true;
    }

    juce::Result cleanupPayloadStaging()
    {
        if (payloadStagingDirectory == juce::File()
            || !payloadStagingDirectory.exists())
        {
            payloadStagingDirectory = juce::File();
            return juce::Result::ok();
        }
        const auto path =
            payloadStagingDirectory.getFullPathName();
        if (!payloadStagingDirectory.deleteRecursively())
        {
            return juce::Result::fail(
                "Could not remove immutable payload staging directory "
                + path + ".");
        }
        payloadStagingDirectory = juce::File();
        return juce::Result::ok();
    }

    DawProjectExportResult finishRun(
        DawProjectExportResult output)
    {
        if (const auto cleanup = cleanupPayloadStaging();
            cleanup.failed())
        {
            addIssue(
                report,
                CompatibilitySeverity::error,
                "export.payload-staging-cleanup",
                "/Archive",
                cleanup.getErrorMessage());
            output.result = cleanup;
        }
        output.report = std::move(report);
        if (output.result.failed())
            output.archive = juce::File();
        return output;
    }

    void appendSchemaIssues(const DawProjectValidationResult& validation,
                            const juce::String& code,
                            const juce::String& fallbackPath)
    {
        for (const auto& issue : validation.issues)
        {
            fail(
                code,
                issue.objectPath.isNotEmpty()
                    ? issue.objectPath
                    : fallbackPath,
                issue.message);
        }
    }

    juce::String trackId(const juce::String& internalId)
    {
        return ids.externalId("track", internalId);
    }

    juce::String channelId(const juce::String& internalId)
    {
        return ids.externalId("channel", internalId);
    }

    juce::String trackParameterId(const juce::String& type,
                                  const juce::String& track)
    {
        return ids.externalId("track-" + type, track);
    }

    juce::String routeParameterId(const juce::String& type,
                                  const juce::String& route)
    {
        return ids.externalId("route-" + type, route);
    }

    juce::String deviceParameterId(const AutomationLane& lane)
    {
        const auto parameterKey = lane.target.parameterId.isNotEmpty()
            ? lane.target.parameterId
            : lane.target.parameterIndex >= 0
                ? juce::String(lane.target.parameterIndex)
                : lane.id;
        return ids.externalId(
            "device-parameter",
            lane.target.insertId + ":" + parameterKey);
    }

    std::unique_ptr<juce::XmlElement> buildMetadata() const
    {
        auto root = makeElement("MetaData");
        if (project.name.isNotEmpty())
            addTextElement(*root, "Title", project.name);
        const std::pair<const char*, const juce::String*> values[] = {
            { "Artist", &project.metadata.artist },
            { "Album", &project.metadata.album },
            { "OriginalArtist", &project.metadata.originalArtist },
            { "Composer", &project.metadata.composer },
            { "Songwriter", &project.metadata.songwriter },
            { "Producer", &project.metadata.producer },
            { "Arranger", &project.metadata.arranger },
            { "Year", &project.metadata.year },
            { "Genre", &project.metadata.genre },
            { "Copyright", &project.metadata.copyright },
            { "Website", &project.metadata.website },
            { "Comment", &project.metadata.comment }
        };
        for (const auto& [name, value] : values)
            if (value->isNotEmpty())
                addTextElement(*root, name, *value);
        return root;
    }

    std::unique_ptr<juce::XmlElement> buildProject()
    {
        auto root = makeElement("Project");
        root->setAttribute("version", "1.0");
        auto* application = addElement(*root, "Application");
        application->setAttribute("name", "Studio Duo");
        application->setAttribute("version", STUDIO_DUO_VERSION);

        writeTransport(*root);
        writeStructure(*root);
        writeArrangement(*root);
        writeScenes(*root);
        return root;
    }

    void writeTransport(juce::XmlElement& root)
    {
        auto* transport = addElement(root, "Transport");
        auto* tempo = addElement(*transport, "Tempo");
        tempo->setAttribute("id", ids.externalId("transport", "tempo"));
        tempo->setAttribute("name", "Tempo");
        tempo->setAttribute("unit", "bpm");
        tempo->setAttribute("min", "20");
        tempo->setAttribute("max", "400");
        tempo->setAttribute("value", formatNumber(project.tempo));
        auto* signature = addElement(*transport, "TimeSignature");
        signature->setAttribute(
            "id",
            ids.externalId("transport", "time-signature"));
        signature->setAttribute("name", "Time signature");
        signature->setAttribute(
            "numerator",
            project.timeSignatureNumerator);
        signature->setAttribute(
            "denominator",
            project.timeSignatureDenominator);
    }

    juce::String hierarchyParent(const Track& track) const
    {
        if (track.folderTrackId.isNotEmpty())
            return track.folderTrackId;
        return track.parentTrackId;
    }

    std::vector<const Track*> childTracks(
        const juce::String& parentId) const
    {
        std::vector<const Track*> children;
        for (const auto& track : project.tracks)
            if (hierarchyParent(track) == parentId)
                children.push_back(&track);
        return children;
    }

    void writeStructure(juce::XmlElement& root)
    {
        auto* structure = addElement(root, "Structure");
        std::set<juce::String> written;
        for (const auto& track : project.tracks)
        {
            const auto parent = hierarchyParent(track);
            if (parent.isNotEmpty() && project.findTrack(parent) != nullptr)
                continue;
            writeTrack(*structure, track, written);
        }
        for (const auto& track : project.tracks)
        {
            if (written.find(track.id) == written.cend())
            {
                warn(
                    "unsupported.track-hierarchy",
                    trackPath(track),
                    "The track hierarchy is cyclic or references an unavailable parent; the track was exported at the root.");
                writeTrack(*structure, track, written);
            }
        }
    }

    juce::String contentTypes(const Track& track) const
    {
        juce::StringArray values;
        if (!track.clips.empty()
            || track.type == TrackType::audio
            || track.type == TrackType::aux
            || track.type == TrackType::bus
            || track.type == TrackType::master)
            values.add("audio");
        if (!track.midiClips.empty()
            || track.type == TrackType::midi
            || track.type == TrackType::instrument)
            values.add("notes");
        if (std::any_of(
                project.automationLanes.cbegin(),
                project.automationLanes.cend(),
                [&track](const auto& lane)
                {
                    return lane.target.trackId == track.id;
                }))
            values.add("automation");
        if (!childTracks(track.id).empty())
            values.add("tracks");
        return values.joinIntoString(" ");
    }

    juce::String mixerRole(TrackType type) const
    {
        switch (type)
        {
            case TrackType::audio:
            case TrackType::instrument:
            case TrackType::midi:
            case TrackType::folder:
                return "regular";
            case TrackType::master: return "master";
            case TrackType::aux: return "effect";
            case TrackType::bus: return "submix";
            case TrackType::vca: return "vca";
            case TrackType::controlRoom: return "effect";
        }
        return "regular";
    }

    const RoutingConnection* mainOutputFor(const Track& track) const
    {
        const auto route = std::find_if(
            project.routingConnections.cbegin(),
            project.routingConnections.cend(),
            [&track](const auto& candidate)
            {
                return candidate.sourceTrackId == track.id
                    && candidate.kind == RouteKind::mainOutput
                    && candidate.enabled
                    && !candidate.muted
                    && candidate.signalType == SignalType::audio
                    && candidate.destination.type
                           == RouteEndpointType::track;
            });
        return route == project.routingConnections.cend()
            ? nullptr
            : &*route;
    }

    std::vector<const RoutingConnection*> sendsFor(
        const Track& track) const
    {
        std::vector<const RoutingConnection*> routes;
        for (const auto& route : project.routingConnections)
        {
            if (route.sourceTrackId == track.id
                && route.kind == RouteKind::send
                && route.enabled
                && !route.muted
                && route.signalType == SignalType::audio
                && route.destination.type == RouteEndpointType::track)
            {
                routes.push_back(&route);
            }
        }
        return routes;
    }

    void writeTrack(juce::XmlElement& parent,
                    const Track& track,
                    std::set<juce::String>& written)
    {
        if (!written.insert(track.id).second)
            return;
        auto* element = addElement(parent, "Track");
        element->setAttribute("id", trackId(track.id));
        element->setAttribute("name", track.name);
        element->setAttribute("color", colourText(track.colour));
        const auto types = contentTypes(track);
        if (types.isNotEmpty())
            element->setAttribute("contentType", types);
        element->setAttribute("loaded", "true");

        if (track.type != TrackType::folder)
            writeChannel(*element, track);
        for (const auto* child : childTracks(track.id))
            writeTrack(*element, *child, written);
    }

    void writeChannel(juce::XmlElement& trackElement,
                      const Track& track)
    {
        auto* channel = addElement(trackElement, "Channel");
        channel->setAttribute("id", channelId(track.id));
        channel->setAttribute("name", track.name);
        channel->setAttribute("color", colourText(track.colour));
        channel->setAttribute(
            "audioChannels",
            track.channelLayout == ChannelLayout::mono ? 1 : 2);
        channel->setAttribute("role", mixerRole(track.type));
        channel->setAttribute("solo", boolText(track.solo));
        if (const auto* route = mainOutputFor(track))
            channel->setAttribute(
                "destination",
                channelId(route->destination.trackId));

        writeDevices(*channel, track);
        auto* mute = addElement(*channel, "Mute");
        mute->setAttribute(
            "id",
            trackParameterId("mute", track.id));
        mute->setAttribute("name", "Mute");
        mute->setAttribute("value", boolText(track.muted));
        auto* pan = addElement(*channel, "Pan");
        pan->setAttribute(
            "id",
            trackParameterId("pan", track.id));
        pan->setAttribute("name", "Pan");
        pan->setAttribute("unit", "normalized");
        pan->setAttribute("min", "-1");
        pan->setAttribute("max", "1");
        pan->setAttribute("value", formatNumber(track.pan));
        writeSends(*channel, track);
        auto* volume = addElement(*channel, "Volume");
        volume->setAttribute(
            "id",
            trackParameterId("volume", track.id));
        volume->setAttribute("name", "Volume");
        volume->setAttribute("unit", "decibel");
        volume->setAttribute("value", formatNumber(track.volumeDecibels));
    }

    void writeDevices(juce::XmlElement& channel, const Track& track)
    {
        if (track.inserts.empty())
            return;
        auto* devices = addElement(channel, "Devices");
        for (std::size_t index = 0; index < track.inserts.size(); ++index)
        {
            const auto& insert = track.inserts[index];
            auto device =
                makeElement(deviceElementName(insert).toRawUTF8());
            device->setAttribute(
                "id",
                ids.externalId("device", insert.id));
            device->setAttribute(
                "deviceID",
                insert.pluginIdentifier.isNotEmpty()
                    ? insert.pluginIdentifier
                    : insert.fileOrIdentifier);
            device->setAttribute("deviceName", insert.name);
            device->setAttribute(
                "deviceVendor",
                insert.manufacturer);
            device->setAttribute(
                "deviceRole",
                track.type == TrackType::instrument && index == 0
                    ? "instrument"
                    : "audioFX");
            device->setAttribute(
                "loaded",
                boolText(!insert.missing));
            if (deviceElementName(insert) != "Device"
                && deviceElementName(insert) != "BuiltinDevice"
                && insert.version.isNotEmpty())
            {
                device->setAttribute(
                    "pluginVersion",
                    insert.version);
            }
            if (deviceElementName(insert) == "Device"
                && insert.format.isNotEmpty())
            {
                device->setAttribute(
                    "comment",
                    "studio-duo-format:" + insert.format);
            }

            writeDeviceParameters(*device, track, insert);
            auto* enabled = addElement(*device, "Enabled");
            enabled->setAttribute("name", "Enabled");
            enabled->setAttribute(
                "value",
                boolText(!insert.bypassed));
            writeDeviceState(*device, track, insert);
            devices->addChildElement(device.release());
        }
    }

    void writeDeviceParameters(juce::XmlElement& device,
                               const Track& track,
                               const PluginInsert& insert)
    {
        std::vector<const AutomationLane*> lanes;
        for (const auto& lane : project.automationLanes)
        {
            if (lane.target.trackId == track.id
                && lane.target.insertId == insert.id
                && (lane.target.type
                        == AutomationTargetType::pluginParameter
                    || lane.target.type
                        == AutomationTargetType::deviceParameter))
            {
                lanes.push_back(&lane);
            }
        }
        if (lanes.empty())
            return;
        std::stable_sort(
            lanes.begin(),
            lanes.end(),
            [](const auto* left, const auto* right)
            {
                if (left->target.parameterIndex
                    != right->target.parameterIndex)
                {
                    return left->target.parameterIndex
                        < right->target.parameterIndex;
                }
                return left->target.parameterId
                    < right->target.parameterId;
            });
        auto* parameters = addElement(device, "Parameters");
        std::set<juce::String> writtenParameters;
        for (const auto* lane : lanes)
        {
            const auto parameterId = deviceParameterId(*lane);
            if (!writtenParameters.insert(parameterId).second)
                continue;
            auto* parameter = addElement(*parameters, "RealParameter");
            parameter->setAttribute("id", parameterId);
            parameter->setAttribute(
                "name",
                lane->target.parameterId.isNotEmpty()
                    ? lane->target.parameterId
                    : lane->name);
            if (lane->target.parameterIndex >= 0)
                parameter->setAttribute(
                    "parameterID",
                    lane->target.parameterIndex);
            parameter->setAttribute("unit", "normalized");
            parameter->setAttribute("min", "0");
            parameter->setAttribute("max", "1");
            if (!lane->points.empty())
            {
                const auto value =
                    lane->points.front().value
                    + lane->trimOffset;
                if (!std::isfinite(value)
                    || value < 0.0
                    || value > 1.0)
                {
                    fail(
                        "export.device-automation-value",
                        automationPath(*lane),
                        "Device automation values must be normalized between 0 and 1.");
                    continue;
                }
                parameter->setAttribute(
                    "value",
                    formatNumber(value));
            }
        }
    }

    void writeDeviceState(juce::XmlElement& device,
                          const Track& track,
                          const PluginInsert& insert)
    {
        if (insert.stateFile.isEmpty() && insert.stateHash.isEmpty())
        {
            warn(
                "unsupported.device-state-missing",
                devicePath(track, insert),
                "The device has no captured state to embed.");
            return;
        }
        juce::MemoryBlock state;
        juce::String error;
        if (!PluginStateStore::load(
                sourcePackage,
                { insert.stateFile, insert.stateHash },
                state,
                error))
        {
            fail(
                "export.device-state",
                devicePath(track, insert),
                "Could not load embedded device state: " + error);
            return;
        }
        if (state.getSize()
            > static_cast<std::size_t>(maxArchiveBytes))
        {
            fail(
                "export.device-state-too-large",
                devicePath(track, insert),
                "The device state exceeds the current 2 GiB classic-ZIP limit.");
            return;
        }
        const auto path =
            "plugin-state/"
            + ids.externalId("device-state", insert.id)
            + stateExtension(insert);
        if (!stageMemoryPayload(
                path,
                state,
                devicePath(track, insert)))
        {
            return;
        }
        auto* stateReference = addElement(device, "State");
        stateReference->setAttribute("path", path);
        stateReference->setAttribute("external", "false");
    }

    void writeSends(juce::XmlElement& channel, const Track& track)
    {
        const auto routes = sendsFor(track);
        if (routes.empty())
            return;
        auto* sends = addElement(channel, "Sends");
        for (const auto* route : routes)
        {
            auto* send = addElement(*sends, "Send");
            send->setAttribute(
                "id",
                ids.externalId("send", route->id));
            send->setAttribute("name", route->name);
            send->setAttribute(
                "destination",
                channelId(route->destination.trackId));
            send->setAttribute(
                "type",
                route->tap == RouteTap::preFader ? "pre" : "post");
            auto* pan = addElement(*send, "Pan");
            pan->setAttribute(
                "id",
                routeParameterId("pan", route->id));
            pan->setAttribute("name", "Pan");
            pan->setAttribute("unit", "normalized");
            pan->setAttribute("min", "-1");
            pan->setAttribute("max", "1");
            pan->setAttribute("value", formatNumber(route->pan));
            auto* volume = addElement(*send, "Volume");
            volume->setAttribute(
                "id",
                routeParameterId("volume", route->id));
            volume->setAttribute("name", "Volume");
            volume->setAttribute("unit", "decibel");
            volume->setAttribute(
                "value",
                formatNumber(route->gainDecibels));
        }
    }

    void writeArrangement(juce::XmlElement& root)
    {
        auto* arrangement = addElement(root, "Arrangement");
        arrangement->setAttribute(
            "id",
            ids.externalId("arrangement", project.id));
        arrangement->setAttribute("name", "Arrangement");

        auto lanes = makeElement("Lanes");
        lanes->setAttribute("timeUnit", "beats");
        auto hasLanes = false;
        for (const auto& track : project.tracks)
        {
            if (track.clips.empty() && track.midiClips.empty())
                continue;
            auto* trackLanes = addElement(*lanes, "Lanes");
            trackLanes->setAttribute("track", trackId(track.id));
            if (!track.clips.empty())
            {
                auto* clips = addElement(*trackLanes, "Clips");
                clips->setAttribute("timeUnit", "seconds");
                for (const auto& clip : track.clips)
                    clips->addChildElement(
                        writeAudioClip(track, clip).release());
            }
            if (!track.midiClips.empty())
            {
                auto* clips = addElement(*trackLanes, "Clips");
                clips->setAttribute("timeUnit", "beats");
                for (const auto& clip : track.midiClips)
                    clips->addChildElement(
                        writeMidiClip(track, clip).release());
            }
            hasLanes = true;
        }
        for (const auto& lane : project.automationLanes)
        {
            auto points = writeAutomation(lane);
            if (points != nullptr)
            {
                lanes->addChildElement(points.release());
                hasLanes = true;
            }
        }
        if (hasLanes)
            arrangement->addChildElement(lanes.release());

        if (!project.sections.empty())
        {
            auto* markers = addElement(*arrangement, "Markers");
            markers->setAttribute("timeUnit", "seconds");
            for (const auto& section : project.sections)
            {
                auto* marker = addElement(*markers, "Marker");
                marker->setAttribute("name", section.name);
                marker->setAttribute(
                    "time",
                    formatNumber(section.timeSeconds));
            }
        }
        writeTempoAutomation(*arrangement);
        writeMeterAutomation(*arrangement);
    }

    std::optional<AudioFileInfo> ensureAudio(
        const Track& track,
        const AudioClip& clip)
    {
        const auto key = clip.sourceFile.getFullPathName();
        if (const auto existing = audioInfo.find(key);
            existing != audioInfo.cend())
        {
            return existing->second;
        }
        if (!clip.sourceFile.existsAsFile())
        {
            fail(
                "export.media-missing",
                clipPath(track, clip),
                "The referenced audio file is missing: "
                    + clip.sourceFile.getFullPathName());
            return std::nullopt;
        }
        if (clip.sourceFile.getSize() > maxArchiveBytes)
        {
            fail(
                "export.media-too-large",
                clipPath(track, clip),
                "The audio file exceeds the current 2 GiB classic-ZIP limit.");
            return std::nullopt;
        }
        const auto initialSize = clip.sourceFile.getSize();
        const auto initialModificationTime =
            clip.sourceFile.getLastModificationTime();
#if STUDIO_DUO_TESTING
        runExportTestHook(
            DawProjectIO::ExportTestPhase::beforePayloadSnapshot,
            clip.sourceFile);
#endif
        if (!clip.sourceFile.existsAsFile()
            || clip.sourceFile.getSize() != initialSize
            || clip.sourceFile.getLastModificationTime()
                   != initialModificationTime)
        {
            fail(
                "export.payload-snapshot",
                clipPath(track, clip),
                "The audio payload changed or disappeared before it could be snapshotted.");
            return std::nullopt;
        }
        if (initialSize < 0
            || initialSize > maxArchiveBytes
            || stagedPayloadBytes > maxArchiveBytes - initialSize)
        {
            fail(
                "export.payload-snapshot-too-large",
                clipPath(track, clip),
                "The external payload snapshots exceed the current 2 GiB classic-ZIP limit.");
            return std::nullopt;
        }
        const auto snapshot =
            createPayloadSnapshot(clipPath(track, clip));
        if (!snapshot.has_value())
            return std::nullopt;
        auto input = clip.sourceFile.createInputStream();
        auto output = snapshot->createOutputStream();
        if (input == nullptr || output == nullptr)
        {
            input.reset();
            output.reset();
            snapshot->deleteFile();
            fail(
                "export.payload-snapshot",
                clipPath(track, clip),
                "Could not open the audio payload or immutable snapshot.");
            return std::nullopt;
        }
        Crc32 snapshotCrc;
        std::array<std::uint8_t, 64 * 1024> buffer {};
        auto remaining = initialSize;
        while (remaining > 0)
        {
            const auto requested = static_cast<int>(
                std::min<juce::int64>(
                    remaining,
                    static_cast<juce::int64>(buffer.size())));
            const auto read = input->read(
                buffer.data(),
                requested);
            if (read <= 0
                || !output->write(
                    buffer.data(),
                    static_cast<std::size_t>(read)))
            {
                input.reset();
                output.reset();
                snapshot->deleteFile();
                fail(
                    "export.payload-snapshot",
                    clipPath(track, clip),
                    "The audio payload became unreadable while it was being snapshotted.");
                return std::nullopt;
            }
            snapshotCrc.update(
                buffer.data(),
                static_cast<std::size_t>(read));
            remaining -= read;
        }
        std::uint8_t extraByte = 0;
        if (input->read(&extraByte, 1) > 0
            || !clip.sourceFile.existsAsFile()
            || clip.sourceFile.getSize() != initialSize
            || clip.sourceFile.getLastModificationTime()
                   != initialModificationTime)
        {
            input.reset();
            output.reset();
            snapshot->deleteFile();
            fail(
                "export.payload-snapshot",
                clipPath(track, clip),
                "The audio payload changed while it was being snapshotted.");
            return std::nullopt;
        }
        input.reset();
        output->flush();
        const auto snapshotStatus = output->getStatus();
        output.reset();
        if (snapshotStatus.failed()
            || !snapshot->existsAsFile()
            || snapshot->getSize() != initialSize)
        {
            snapshot->deleteFile();
            fail(
                "export.payload-snapshot",
                clipPath(track, clip),
                "The immutable audio payload snapshot could not be completed.");
            return std::nullopt;
        }
#if STUDIO_DUO_TESTING
        runExportTestHook(
            DawProjectIO::ExportTestPhase::afterPayloadSnapshot,
            *snapshot);
#endif
        if (!snapshot->existsAsFile()
            || snapshot->getSize() != initialSize)
        {
            snapshot->deleteFile();
            fail(
                "export.payload-snapshot",
                clipPath(track, clip),
                "The immutable audio payload snapshot was removed or changed after capture.");
            return std::nullopt;
        }
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(
                snapshot->createInputStream()));
        if (reader == nullptr
            || reader->sampleRate <= 0.0
            || reader->numChannels == 0)
        {
            snapshot->deleteFile();
            fail(
                "export.media-invalid",
                clipPath(track, clip),
                "The referenced audio file could not be decoded.");
            return std::nullopt;
        }
        auto extension =
            clip.sourceFile.getFileExtension().toLowerCase();
        if (extension.isEmpty()
            || extension.length() > 12
            || extension.containsAnyOf("/\\:"))
        {
            extension = ".bin";
        }
        const auto hash =
            juce::SHA256(*snapshot).toHexString().toLowerCase();
        AudioFileInfo info;
        info.archivePath = "media/" + hash + extension;
        info.durationSeconds =
            static_cast<double>(reader->lengthInSamples)
            / reader->sampleRate;
        info.channels = static_cast<int>(reader->numChannels);
        info.sampleRate =
            static_cast<int>(std::round(reader->sampleRate));
        reader.reset();
        payloads[info.archivePath] = {
            *snapshot,
            initialSize,
            snapshotCrc.result()
        };
        stagedPayloadBytes += initialSize;
        audioInfo.emplace(key, info);
        return info;
    }

    std::unique_ptr<juce::XmlElement> writeAudio(
        const Track&,
        const AudioClip& clip,
        const AudioFileInfo& info)
    {
        auto audio = makeElement("Audio");
        audio->setAttribute("timeUnit", "seconds");
        audio->setAttribute(
            "duration",
            formatNumber(info.durationSeconds));
        audio->setAttribute("channels", info.channels);
        audio->setAttribute("sampleRate", info.sampleRate);
        audio->setAttribute(
            "algorithm",
            stretchAlgorithm(clip.stretchMode));
        auto* file = addElement(*audio, "File");
        file->setAttribute("path", info.archivePath);
        file->setAttribute("external", "false");
        return audio;
    }

    std::unique_ptr<juce::XmlElement> writeAudioContent(
        const Track& track,
        const AudioClip& clip,
        const AudioFileInfo& info)
    {
        const auto needsWarps = !clip.warpMarkers.empty()
            || std::abs(clip.playbackRate - 1.0) > 0.000001;
        std::unique_ptr<juce::XmlElement> content;
        if (needsWarps)
        {
            auto warps = makeElement("Warps");
            warps->setAttribute("timeUnit", "seconds");
            warps->setAttribute("contentTimeUnit", "seconds");
            warps->addChildElement(
                writeAudio(track, clip, info).release());
            std::vector<WarpMarker> markers {
                { 0.0, clip.sourceOffsetSeconds },
                {
                clip.durationSeconds,
                std::min(
                    clip.sourceRangeEnd(),
                    clip.sourceOffsetSeconds
                        + clip.durationSeconds
                            * clip.playbackRate)
                }
            };
            markers.insert(
                markers.end(),
                clip.warpMarkers.begin(),
                clip.warpMarkers.end());
            std::stable_sort(
                markers.begin(),
                markers.end(),
                [](const auto& left, const auto& right)
                {
                    return left.timelineOffsetSeconds
                        < right.timelineOffsetSeconds;
                });
            std::vector<WarpMarker> uniqueMarkers;
            for (const auto& marker : markers)
            {
                if (!uniqueMarkers.empty()
                    && std::abs(
                           uniqueMarkers.back().timelineOffsetSeconds
                           - marker.timelineOffsetSeconds)
                           < 0.0000001)
                {
                    uniqueMarkers.back() = marker;
                }
                else
                {
                    uniqueMarkers.push_back(marker);
                }
            }
            markers = std::move(uniqueMarkers);
            for (const auto& marker : markers)
            {
                auto* warp = addElement(*warps, "Warp");
                warp->setAttribute(
                    "time",
                    formatNumber(marker.timelineOffsetSeconds));
                warp->setAttribute(
                    "contentTime",
                    formatNumber(marker.sourceSeconds));
            }
            content = std::move(warps);
        }
        else
        {
            content = writeAudio(track, clip, info);
        }

        if (std::abs(clip.gainDecibels) > 0.000001f)
        {
            auto lanes = makeElement("Lanes");
            lanes->setAttribute("timeUnit", "seconds");
            lanes->addChildElement(content.release());
            auto* points = addElement(*lanes, "Points");
            points->setAttribute("timeUnit", "seconds");
            points->setAttribute("unit", "decibel");
            auto* target = addElement(*points, "Target");
            target->setAttribute("expression", "gain");
            auto* point = addElement(*points, "RealPoint");
            point->setAttribute("time", "0");
            point->setAttribute(
                "value",
                formatNumber(clip.gainDecibels));
            point->setAttribute("interpolation", "hold");
            content = std::move(lanes);
        }
        return content;
    }

    std::unique_ptr<juce::XmlElement> writeAudioClip(
        const Track& track,
        const AudioClip& clip)
    {
        auto element = makeElement("Clip");
        element->setAttribute("name", clip.name);
        element->setAttribute("color", colourText(clip.colour));
        element->setAttribute(
            "time",
            formatNumber(clip.startSeconds));
        element->setAttribute(
            "duration",
            formatNumber(clip.durationSeconds));
        element->setAttribute("contentTimeUnit", "seconds");
        element->setAttribute(
            "playStart",
            formatNumber(clip.sourceOffsetSeconds));
        element->setAttribute(
            "playStop",
            formatNumber(
                std::min(
                    clip.sourceRangeEnd(),
                    clip.sourceOffsetSeconds
                        + clip.durationSeconds
                            * clip.playbackRate)));
        element->setAttribute("fadeTimeUnit", "seconds");
        if (clip.fadeInSeconds > 0.0)
            element->setAttribute(
                "fadeInTime",
                formatNumber(clip.fadeInSeconds));
        if (clip.fadeOutSeconds > 0.0)
            element->setAttribute(
                "fadeOutTime",
                formatNumber(clip.fadeOutSeconds));
        if (const auto info = ensureAudio(track, clip))
        {
            element->addChildElement(
                writeAudioContent(track, clip, *info).release());
        }
        return element;
    }

    std::unique_ptr<juce::XmlElement> writeMidiClip(
        const Track&,
        const MidiClip& clip)
    {
        auto element = makeElement("Clip");
        element->setAttribute("name", clip.name);
        element->setAttribute(
            "time",
            formatNumber(clip.startBeats));
        element->setAttribute(
            "duration",
            formatNumber(clip.durationBeats));
        element->setAttribute("contentTimeUnit", "beats");
        element->setAttribute("playStart", "0");
        element->setAttribute(
            "playStop",
            formatNumber(clip.durationBeats));
        auto* notes = addElement(*element, "Notes");
        notes->setAttribute("timeUnit", "beats");
        for (const auto& note : clip.notes)
        {
            auto* noteElement = addElement(*notes, "Note");
            noteElement->setAttribute(
                "time",
                formatNumber(note.actualStartBeats()));
            noteElement->setAttribute(
                "duration",
                formatNumber(note.durationBeats));
            noteElement->setAttribute("channel", note.channel - 1);
            noteElement->setAttribute("key", note.pitch);
            noteElement->setAttribute(
                "vel",
                formatNumber(
                    static_cast<double>(note.velocity) / 127.0));
            noteElement->setAttribute(
                "rel",
                formatNumber(
                    static_cast<double>(note.releaseVelocity) / 127.0));
            writeNoteExpressions(*noteElement, note);
        }
        return element;
    }

    void writeNoteExpressions(juce::XmlElement& noteElement,
                              const MidiNote& note)
    {
        if (note.expressions.empty())
            return;
        auto expressionName = [](MidiExpressionType type)
        {
            switch (type)
            {
                case MidiExpressionType::pitchBend:
                    return juce::String("pitchBend");
                case MidiExpressionType::pressure:
                    return juce::String("pressure");
                case MidiExpressionType::channelPressure:
                    return juce::String("channelPressure");
                case MidiExpressionType::timbre:
                    return juce::String("timbre");
                case MidiExpressionType::controller:
                    return juce::String("channelController");
            }
            return juce::String("pressure");
        };
        auto writePoints = [&](const MidiExpressionPoint& expression)
        {
            auto points = makeElement("Points");
            points->setAttribute("timeUnit", "beats");
            points->setAttribute("unit", "normalized");
            auto* target = addElement(*points, "Target");
            target->setAttribute(
                "expression",
                expressionName(expression.type));
            if (expression.type == MidiExpressionType::controller)
                target->setAttribute(
                    "controller",
                    expression.controller);
            auto* point = addElement(*points, "RealPoint");
            point->setAttribute(
                "time",
                formatNumber(expression.offsetBeats));
            point->setAttribute(
                "value",
                formatNumber(expression.value));
            point->setAttribute("interpolation", "linear");
            return points;
        };
        if (note.expressions.size() == 1)
        {
            noteElement.addChildElement(
                writePoints(note.expressions.front()).release());
            return;
        }
        auto* lanes = addElement(noteElement, "Lanes");
        lanes->setAttribute("timeUnit", "beats");
        for (const auto& expression : note.expressions)
            lanes->addChildElement(writePoints(expression).release());
    }

    std::unique_ptr<juce::XmlElement> writeAutomation(
        const AutomationLane& lane)
    {
        auto parameter = juce::String();
        auto expression = juce::String();
        auto unit = juce::String("linear");
        auto booleanPoints = false;
        auto normalizedPoints = false;
        switch (lane.target.type)
        {
            case AutomationTargetType::trackVolume:
            case AutomationTargetType::vcaVolume:
                parameter =
                    trackParameterId("volume", lane.target.trackId);
                unit = "decibel";
                break;
            case AutomationTargetType::trackPan:
                parameter =
                    trackParameterId("pan", lane.target.trackId);
                unit = "normalized";
                break;
            case AutomationTargetType::trackMute:
                parameter =
                    trackParameterId("mute", lane.target.trackId);
                booleanPoints = true;
                break;
            case AutomationTargetType::sendGain:
                if (!representableSend(lane.target.routeId))
                {
                    warn(
                        "unsupported.automation-target",
                        automationPath(lane),
                        "The automation targets a send that cannot be represented by DAWproject 1.0.");
                    return nullptr;
                }
                parameter =
                    routeParameterId("volume", lane.target.routeId);
                unit = "decibel";
                break;
            case AutomationTargetType::sendPan:
                if (!representableSend(lane.target.routeId))
                {
                    warn(
                        "unsupported.automation-target",
                        automationPath(lane),
                        "The automation targets a send that cannot be represented by DAWproject 1.0.");
                    return nullptr;
                }
                parameter =
                    routeParameterId("pan", lane.target.routeId);
                unit = "normalized";
                break;
            case AutomationTargetType::pluginParameter:
            case AutomationTargetType::deviceParameter:
                if (!representableDeviceTarget(lane))
                {
                    warn(
                        "unsupported.automation-target",
                        automationPath(lane),
                        "The automation targets an unavailable device parameter.");
                    return nullptr;
                }
                parameter = deviceParameterId(lane);
                unit = "normalized";
                normalizedPoints = true;
                break;
            case AutomationTargetType::midiChannelPressure:
                if (lane.target.midiChannel < 1
                    || lane.target.midiChannel > 16)
                {
                    fail(
                        "export.channel-pressure-channel",
                        automationPath(lane),
                        "Channel-pressure automation requires a MIDI channel from 1 through 16.");
                    return nullptr;
                }
                expression = "channelPressure";
                unit = "normalized";
                normalizedPoints = true;
                break;
            case AutomationTargetType::trackPolarity:
            case AutomationTargetType::sendMute:
            case AutomationTargetType::controlRoomDim:
                warn(
                    "unsupported.automation-target",
                    automationPath(lane),
                    "The automation target has no DAWproject 1.0 parameter representation.");
                return nullptr;
        }

        auto points = makeElement("Points");
        points->setAttribute(
            "id",
            ids.externalId("automation", lane.id));
        points->setAttribute("name", lane.name);
        points->setAttribute(
            "timeUnit",
            lane.timebase == AutomationTimebase::beats
                ? "beats"
                : "seconds");
        points->setAttribute("track", trackId(lane.target.trackId));
        if (!booleanPoints)
            points->setAttribute("unit", unit);
        auto* target = addElement(*points, "Target");
        if (expression.isNotEmpty())
        {
            target->setAttribute("expression", expression);
            target->setAttribute(
                "channel",
                lane.target.midiChannel - 1);
        }
        else
        {
            target->setAttribute("parameter", parameter);
        }
        for (const auto& sourcePoint : lane.points)
        {
            auto* point = addElement(
                *points,
                booleanPoints ? "BoolPoint" : "RealPoint");
            point->setAttribute(
                "time",
                formatNumber(sourcePoint.position));
            if (booleanPoints)
            {
                point->setAttribute(
                    "value",
                    boolText(
                        sourcePoint.value + lane.trimOffset >= 0.5));
            }
            else
            {
                const auto value =
                    sourcePoint.value + lane.trimOffset;
                if (normalizedPoints
                    && (!std::isfinite(value)
                        || value < 0.0
                        || value > 1.0))
                {
                    fail(
                        expression.isNotEmpty()
                            ? "export.channel-pressure-value"
                            : "export.device-automation-value",
                        automationPath(lane),
                        expression.isNotEmpty()
                            ? "Channel-pressure automation values must be normalized between 0 and 1."
                            : "Device automation values must be normalized between 0 and 1.");
                    return nullptr;
                }
                point->setAttribute(
                    "value",
                    formatNumber(value));
                point->setAttribute(
                    "interpolation",
                    lane.interpolation
                            == AutomationInterpolation::linear
                        ? "linear"
                        : "hold");
            }
        }
        return points;
    }

    bool representableSend(const juce::String& routeId) const
    {
        const auto route = project.findRoutingConnection(routeId);
        return route != nullptr
            && route->kind == RouteKind::send
            && route->signalType == SignalType::audio
            && route->destination.type == RouteEndpointType::track
            && route->enabled
            && !route->muted
            && route->sourceInsertId.isEmpty()
            && route->sourceBusIndex == 0
            && route->destination.insertId.isEmpty()
            && route->destination.busIndex == 0;
    }

    bool representableDeviceTarget(const AutomationLane& lane) const
    {
        const auto* track = project.findTrack(lane.target.trackId);
        return track != nullptr
            && std::any_of(
                track->inserts.cbegin(),
                track->inserts.cend(),
                [&lane](const auto& insert)
                {
                    return insert.id == lane.target.insertId;
                });
    }

    void writeTempoAutomation(juce::XmlElement& arrangement)
    {
        if (project.tempoChanges.empty())
            return;
        auto* points = addElement(arrangement, "TempoAutomation");
        points->setAttribute("timeUnit", "seconds");
        points->setAttribute("unit", "bpm");
        auto* target = addElement(*points, "Target");
        target->setAttribute(
            "parameter",
            ids.externalId("transport", "tempo"));
        for (const auto& change : project.tempoChanges)
        {
            auto* point = addElement(*points, "RealPoint");
            point->setAttribute(
                "time",
                formatNumber(change.timeSeconds));
            point->setAttribute(
                "value",
                formatNumber(change.bpm));
            point->setAttribute(
                "interpolation",
                change.rampToNext ? "linear" : "hold");
        }
    }

    void writeMeterAutomation(juce::XmlElement& arrangement)
    {
        if (project.meterChanges.empty())
            return;
        auto* points = addElement(
            arrangement,
            "TimeSignatureAutomation");
        points->setAttribute("timeUnit", "seconds");
        auto* target = addElement(*points, "Target");
        target->setAttribute(
            "parameter",
            ids.externalId("transport", "time-signature"));
        for (const auto& change : project.meterChanges)
        {
            auto* point = addElement(*points, "TimeSignaturePoint");
            point->setAttribute(
                "time",
                formatNumber(change.timeSeconds));
            point->setAttribute("numerator", change.numerator);
            point->setAttribute("denominator", change.denominator);
        }
    }

    void writeScenes(juce::XmlElement& root)
    {
        if (project.scenes.empty())
            return;
        auto* scenes = addElement(root, "Scenes");
        for (const auto& scene : project.scenes)
        {
            auto* sceneElement = addElement(*scenes, "Scene");
            sceneElement->setAttribute(
                "id",
                ids.externalId("scene", scene.id));
            sceneElement->setAttribute("name", scene.name);
            sceneElement->setAttribute(
                "color",
                colourText(scene.colour));
            auto* lanes = addElement(*sceneElement, "Lanes");
            for (const auto& slot : scene.slots)
            {
                auto* slotElement = addElement(*lanes, "ClipSlot");
                slotElement->setAttribute(
                    "id",
                    ids.externalId("scene-slot", slot.id));
                slotElement->setAttribute(
                    "track",
                    trackId(slot.trackId));
                slotElement->setAttribute(
                    "hasStop",
                    boolText(slot.stopTrack));
                const auto* track = project.findTrack(slot.trackId);
                if (track == nullptr)
                {
                    fail(
                        "export.scene-track",
                        scenePath(scene) + "/Slot[" + slot.id + "]",
                        "The scene slot references an unavailable track.");
                    continue;
                }
                if (slot.audioClip.has_value())
                {
                    slotElement->addChildElement(
                        writeAudioClip(*track, *slot.audioClip).release());
                }
                else if (slot.midiClip.has_value())
                {
                    slotElement->addChildElement(
                        writeMidiClip(*track, *slot.midiClip).release());
                }
            }
        }
    }

    void reportUnsupportedProjectData()
    {
        if (project.metronomeEnabled
            || project.metronomeSubdivision != 1
            || project.metronomeOutputChannel != 0)
        {
            warn(
                "unsupported.metronome",
                "/Project/Transport/Metronome",
                "DAWproject 1.0 does not store metronome routing or state.");
        }
        if (project.punchEnabled
            || project.countInBars != 0
            || project.preRollSeconds != 0.0
            || project.postRollSeconds != 0.0)
        {
            warn(
                "unsupported.recording-transport",
                "/Project/Transport/Recording",
                "Punch, count-in, pre-roll, and post-roll are not represented by DAWproject 1.0.");
        }
        if (project.loopEnabled)
        {
            warn(
                "unsupported.loop",
                "/Project/Transport/Loop",
                "The Studio Duo transport loop is not represented by DAWproject 1.0.");
        }
        for (const auto& group : project.editGroups)
            warn(
                "unsupported.edit-group",
                "/Project/EditGroup[" + group.id + "]",
                "Linked edit groups are Studio Duo-specific.");
        for (const auto& route : project.reampRoutes)
            warn(
                "unsupported.reamp",
                "/Project/ReampRoute[" + route.id + "]",
                "Reamp routes are Studio Duo-specific.");
        for (const auto& snapshot : project.toneSnapshots)
            warn(
                "unsupported.tone-snapshot",
                "/Project/ToneSnapshot[" + snapshot.id + "]",
                "Tone snapshots are Studio Duo-specific.");
        for (const auto& snapshot : project.mixerSnapshots)
            warn(
                "unsupported.mixer-snapshot",
                "/Project/MixerSnapshot[" + snapshot.id + "]",
                "Mixer snapshots are not represented by DAWproject 1.0.");
        for (const auto& render : project.renderReports)
            warn(
                "unsupported.render-report",
                "/Project/RenderReport[" + render.id + "]",
                "Render reports are not represented by DAWproject 1.0.");
        for (const auto& map : project.drumMaps)
            warn(
                "unsupported.drum-map",
                "/Project/DrumMap[" + map.id + "]",
                "Studio Duo drum maps are not represented by DAWproject 1.0.");
        for (const auto& pattern : project.midiPatterns)
            warn(
                "unsupported.midi-pattern",
                "/Project/MidiPattern[" + pattern.id + "]",
                "Studio Duo pattern aliases are not represented by DAWproject 1.0.");
        for (const auto& routing : project.midiRoutingTemplates)
            warn(
                "unsupported.midi-routing-template",
                "/Project/MidiRoutingTemplate[" + routing.id + "]",
                "Studio Duo MIDI routing templates are not represented by DAWproject 1.0.");

        for (const auto& track : project.tracks)
        {
            const auto path = trackPath(track);
            if (track.parentTrackId.isNotEmpty())
                warn(
                    "unsupported.track-version",
                    path,
                    "Take/version semantics are exported only as track hierarchy.");
            if (track.inputMonitoring)
                warn(
                    "unsupported.track-input-monitoring",
                    path,
                    "Input monitoring is not represented by DAWproject 1.0.");
            if (track.inputChannel != 0 || track.stereoInput)
                warn(
                    "unsupported.track-input",
                    path,
                    "Track input routing is not represented by DAWproject 1.0.");
            if (track.hardwareOutputChannel != 0)
                warn(
                    "unsupported.track-hardware-output",
                    path,
                    "Studio Duo hardware output selection is not represented by DAWproject 1.0.");
            if (track.polarityInverted)
                warn(
                    "unsupported.track-polarity",
                    path,
                    "Track polarity is not represented by DAWproject 1.0.");
            if (track.soloSafe)
                warn(
                    "unsupported.track-solo-safe",
                    path,
                    "Solo-safe state is not represented by DAWproject 1.0.");
            if (track.armed)
                warn(
                    "unsupported.track-arm",
                    path,
                    "Track record-arm state is not represented by DAWproject 1.0.");
            if (!track.controlledTrackIds.empty())
                warn(
                    "unsupported.vca-membership",
                    path,
                    "VCA controlled-track membership is not represented by DAWproject 1.0.");
            if (!track.compRegions.empty()
                || track.activeTakeTrackId.isNotEmpty())
                warn(
                    "unsupported.comping",
                    path,
                    "Studio Duo comp-region and active-take semantics are not represented by DAWproject 1.0.");
            for (const auto& insert : track.inserts)
            {
                const auto insertPath = devicePath(track, insert);
                if (insert.missing)
                    warn(
                        "unsupported.device-missing",
                        insertPath,
                        "The missing device descriptor is exported unloaded.");
                if (insert.bridgeMode
                    != PluginBridgeMode::sandboxed)
                    warn(
                        "unsupported.device-bridge-mode",
                        insertPath,
                        "Studio Duo bridge mode is not represented by DAWproject 1.0.");
                if (insert.latencySamples != 0
                    || insert.tailSeconds != 0.0)
                    warn(
                        "unsupported.device-runtime",
                        insertPath,
                        "Device latency and tail measurements are not represented by DAWproject 1.0.");
                if (insert.araCapable || insert.recoveryDisabled)
                    warn(
                        "unsupported.device-host-state",
                        insertPath,
                        "ARA and recovery-disable host state are not represented by DAWproject 1.0.");
                if ((!insert.stateFile.isEmpty()
                     || !insert.stateHash.isEmpty())
                    && insert.stateFormat
                           == PluginStateFormat::hostOpaque
                    && (insert.format.containsIgnoreCase("VST")
                        || insert.format.containsIgnoreCase("AU")))
                    warn(
                        "compatibility.device-state-container",
                        insertPath,
                        "Studio Duo embeds its captured opaque processor state; VST and Audio Unit preset-container portability depends on the receiving host.");
                if (deviceElementName(insert) == "Device"
                    && insert.format.isNotEmpty())
                    warn(
                        "unsupported.device-format",
                        insertPath,
                        "The device format is exported through the generic DAWproject Device element.");
            }
            for (const auto& clip : track.clips)
            {
                const auto pathForClip = clipPath(track, clip);
                if (clip.reversed)
                    warn(
                        "unsupported.clip-reverse",
                        pathForClip,
                        "Reverse playback is not represented by DAWproject 1.0.");
                if (clip.polarityInverted)
                    warn(
                        "unsupported.clip-polarity",
                        pathForClip,
                        "Clip polarity is not represented by DAWproject 1.0.");
                if (clip.muted)
                    warn(
                        "unsupported.clip-mute",
                        pathForClip,
                        "Clip enable state is not part of the tagged DAWproject 1.0 schema.");
                if (std::abs(clip.fadeInCurve) > 0.000001f
                    || std::abs(clip.fadeOutCurve) > 0.000001f)
                    warn(
                        "unsupported.clip-fade-curve",
                        pathForClip,
                        "Fade durations are exported, but Studio Duo fade curve shapes are not represented by DAWproject 1.0.");
                if (!clip.transientSourceSeconds.empty())
                    warn(
                        "unsupported.clip-transients",
                        pathForClip,
                        "Detected transient markers are not represented by DAWproject 1.0.");
                if (clip.sourceRangeStartSeconds > 0.0
                    || clip.sourceRangeEnd()
                        < clip.sourceLengthSeconds - 0.000001)
                    warn(
                        "unsupported.clip-recovery-range",
                        pathForClip,
                        "The active play range is exported, but Studio Duo recoverable source bounds are not represented by DAWproject 1.0.");
            }
            for (const auto& clip : track.midiClips)
                reportUnsupportedMidi(track, clip);
        }
        for (const auto& scene : project.scenes)
        {
            for (const auto& slot : scene.slots)
            {
                const auto* track = project.findTrack(slot.trackId);
                if (track != nullptr && slot.midiClip.has_value())
                    reportUnsupportedMidi(*track, *slot.midiClip);
            }
        }

        for (const auto& route : project.routingConnections)
        {
            const auto supportedMain = route.kind == RouteKind::mainOutput
                && route.signalType == SignalType::audio
                && route.destination.type == RouteEndpointType::track
                && route.enabled
                && !route.muted;
            const auto supportedSend = route.kind == RouteKind::send
                && route.signalType == SignalType::audio
                && route.destination.type == RouteEndpointType::track
                && route.enabled
                && !route.muted
                && route.sourceInsertId.isEmpty()
                && route.sourceBusIndex == 0
                && route.destination.insertId.isEmpty()
                && route.destination.busIndex == 0;
            if (!supportedMain && !supportedSend)
            {
                warn(
                    "unsupported.routing",
                    routePath(route),
                    "The route uses a DAWproject 1.0-unsupported signal type, endpoint, bus, sidechain, hardware, mute, or enable state.");
            }
            else if (supportedMain
                     && (std::abs(route.gainDecibels) > 0.000001f
                         || std::abs(route.pan) > 0.000001f))
            {
                warn(
                    "unsupported.main-route-gain",
                    routePath(route),
                    "Main-output route gain and pan are not represented by DAWproject 1.0.");
            }
        }
        for (const auto& lane : project.automationLanes)
        {
            if (std::abs(lane.trimOffset) > 0.000001)
                warn(
                    "unsupported.automation-trim",
                    automationPath(lane),
                    "Automation trim is baked into exported point values.");
            if (!lane.enabled)
                warn(
                    "unsupported.automation-enable",
                    automationPath(lane),
                    "Disabled automation is exported enabled because DAWproject 1.0 has no lane-enable state.");
        }
    }

    void reportUnsupportedMidi(const Track& track, const MidiClip& clip)
    {
        const auto path = midiClipPath(track, clip);
        if (clip.editorMode == MidiEditorMode::drums
            || clip.drumMapId.isNotEmpty())
            warn(
                "unsupported.midi-editor-metadata",
                path,
                "MIDI notes are exported, but Studio Duo drum-editor and drum-map metadata are not represented by DAWproject 1.0.");
        if (clip.humanizeSeed != 0
            || clip.humanizeTimingTicks != 0
            || clip.humanizeVelocity != 0)
            warn(
                "unsupported.midi-humanization",
                path,
                "Applied note timing and velocity are exported, but deterministic humanization settings are not represented by DAWproject 1.0.");
        if (clip.muted)
            warn(
                "unsupported.midi-clip-mute",
                path,
                "Clip enable state is not part of the tagged DAWproject 1.0 schema.");
        for (const auto& note : clip.notes)
        {
            const auto notePath =
                path + "/Note[" + note.id + "]";
            if (std::abs(note.probability - 1.0) > 0.000001)
                warn(
                    "unsupported.note-probability",
                    notePath,
                    "Note probability is not represented by DAWproject 1.0.");
            if (note.drumMapEntryId.isNotEmpty()
                || note.articulation.isNotEmpty()
                || note.chokeGroup.isNotEmpty()
                || note.cymbalState != CymbalState::none
                || note.footControlValue >= 0
                || note.roundRobinHint >= 0)
                warn(
                    "unsupported.note-drum-metadata",
                    notePath,
                    "The MIDI note is exported, but Studio Duo articulation, choke, cymbal, foot-control, and round-robin metadata are not represented by DAWproject 1.0.");
        }
    }

    juce::Result writeArchive(const juce::String& projectXml,
                              const juce::String& metadataXml)
    {
        struct PreparedEntry
        {
            juce::String path;
            juce::MemoryBlock inlinePayload;
            juce::File snapshot;
            std::uint32_t size = 0;
            std::uint32_t crc = 0;
            std::uint32_t offset = 0;
        };

        if (!destinationArchive.getParentDirectory().createDirectory())
            return juce::Result::fail(
                "Could not create the DAWproject destination directory.");
        const auto temporary =
            destinationArchive.getSiblingFile(
                destinationArchive.getFileName()
                + ".tmp-" + juce::Uuid().toString());
        struct StagingCleanup
        {
            juce::File file;

            ~StagingCleanup()
            {
                if (file.existsAsFile())
                    file.deleteFile();
            }
        } stagingCleanup { temporary };

        std::vector<PreparedEntry> entries;
        const auto addString = [&entries](const juce::String& text,
                                          const juce::String& path)
        {
            const auto utf8 = text.toStdString();
            PreparedEntry entry;
            entry.path = path;
            entry.inlinePayload =
                juce::MemoryBlock(utf8.data(), utf8.size());
            entries.push_back(std::move(entry));
        };
        addString(metadataXml, metadataEntryName);
        addString(projectXml, projectEntryName);
        for (const auto& [path, payload] : payloads)
        {
            PreparedEntry entry;
            entry.path = path;
            entry.snapshot = payload.snapshot;
            entry.size =
                static_cast<std::uint32_t>(payload.size);
            entry.crc = payload.crc;
            entries.push_back(std::move(entry));
        }
        if (entries.size() > 65535)
        {
            return juce::Result::fail(
                "The DAWproject archive contains too many entries for the classic ZIP container.");
        }

        juce::int64 totalPayloadBytes = 0;
        const auto prepareEntry =
            [&totalPayloadBytes](PreparedEntry& entry)
        {
            const auto fileBacked =
                entry.snapshot != juce::File();
            const auto size = fileBacked
                ? static_cast<juce::int64>(entry.size)
                : static_cast<juce::int64>(
                      entry.inlinePayload.getSize());
            if (size < 0
                || size > maxArchiveBytes
                || totalPayloadBytes > maxArchiveBytes - size)
                return juce::Result::fail(
                    "Archive payload exceeds the supported classic ZIP size: "
                    + entry.path);
            Crc32 crc;
            if (fileBacked)
            {
                if (!entry.snapshot.existsAsFile()
                    || entry.snapshot.getSize() != size)
                {
                    return juce::Result::fail(
                        "The immutable payload snapshot is missing or has changed: "
                        + entry.path);
                }
                auto input = entry.snapshot.createInputStream();
                if (input == nullptr)
                {
                    return juce::Result::fail(
                        "Could not open immutable payload snapshot "
                        + entry.path + ".");
                }
                std::array<std::uint8_t, 64 * 1024> buffer {};
                auto remaining = size;
                while (remaining > 0)
                {
                    const auto requested = static_cast<int>(
                        std::min<juce::int64>(
                            remaining,
                            static_cast<juce::int64>(
                                buffer.size())));
                    const auto read = input->read(
                        buffer.data(),
                        requested);
                    if (read <= 0)
                    {
                        return juce::Result::fail(
                            "The immutable payload snapshot is truncated: "
                            + entry.path);
                    }
                    crc.update(
                        buffer.data(),
                        static_cast<std::size_t>(read));
                    remaining -= read;
                }
                std::uint8_t extra = 0;
                if (input->read(&extra, 1) > 0
                    || crc.result() != entry.crc)
                {
                    return juce::Result::fail(
                        "The immutable payload snapshot changed before archive staging: "
                        + entry.path);
                }
            }
            else
            {
                entry.size = static_cast<std::uint32_t>(size);
                crc.update(
                    entry.inlinePayload.getData(),
                    entry.inlinePayload.getSize());
                entry.crc = crc.result();
            }
            totalPayloadBytes += size;
            return juce::Result::ok();
        };
        for (auto& entry : entries)
        {
            if (const auto result = prepareEntry(entry);
                result.failed())
                return result;
        }

        const auto writeName = [](juce::OutputStream& output,
                                  const juce::String& path)
        {
            const auto utf8 = path.toStdString();
            return output.write(utf8.data(), utf8.size());
        };
        const auto writePayload = [](juce::OutputStream& output,
                                     const PreparedEntry& entry)
        {
            if (entry.snapshot == juce::File())
            {
                return output.write(
                    entry.inlinePayload.getData(),
                    entry.inlinePayload.getSize());
            }
            auto input = entry.snapshot.createInputStream();
            if (input == nullptr)
                return false;
            std::array<std::uint8_t, 64 * 1024> buffer {};
            auto remaining =
                static_cast<juce::int64>(entry.size);
            while (remaining > 0)
            {
                const auto requested = static_cast<int>(
                    std::min<juce::int64>(
                        remaining,
                        static_cast<juce::int64>(
                            buffer.size())));
                const auto read = input->read(
                    buffer.data(),
                    requested);
                if (read <= 0
                    || !output.write(
                        buffer.data(),
                        static_cast<std::size_t>(read)))
                {
                    return false;
                }
                remaining -= read;
            }
            std::uint8_t extra = 0;
            return input->read(&extra, 1) == 0;
        };
        juce::int64 directoryOffset = -1;
        {
            auto output = temporary.createOutputStream();
            if (output == nullptr)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "Could not open the staged DAWproject archive.");
            }
            for (auto& entry : entries)
            {
                const auto pathBytes = entry.path.toStdString();
                if (pathBytes.empty() || pathBytes.size() > 65535)
                {
                    output.reset();
                    temporary.deleteFile();
                    return juce::Result::fail(
                        "Archive entry name is invalid: " + entry.path);
                }
                const auto offset = output->getPosition();
                if (offset < 0 || offset > maxArchiveBytes)
                {
                    output.reset();
                    temporary.deleteFile();
                    return juce::Result::fail(
                        "The DAWproject archive exceeds the supported classic ZIP size.");
                }
                entry.offset = static_cast<std::uint32_t>(offset);
                output->writeInt(0x04034b50);
                output->writeShort(20);
                output->writeShort(1 << 11);
                output->writeShort(0);
                output->writeShort(0);
                output->writeShort(33);
                output->writeInt(static_cast<int>(entry.crc));
                output->writeInt(static_cast<int>(entry.size));
                output->writeInt(static_cast<int>(entry.size));
                output->writeShort(
                    static_cast<short>(pathBytes.size()));
                output->writeShort(0);
                if (!writeName(*output, entry.path)
                    || !writePayload(*output, entry))
                {
                    output.reset();
                    temporary.deleteFile();
                    return juce::Result::fail(
                        "Could not write archive payload " + entry.path + ".");
                }
            }
            directoryOffset = output->getPosition();
            if (directoryOffset < 0
                || directoryOffset > maxArchiveBytes)
            {
                output.reset();
                temporary.deleteFile();
                return juce::Result::fail(
                    "The DAWproject archive exceeds the supported classic ZIP size.");
            }
            for (const auto& entry : entries)
            {
                const auto pathBytes = entry.path.toStdString();
                output->writeInt(0x02014b50);
                output->writeShort(20);
                output->writeShort(20);
                output->writeShort(1 << 11);
                output->writeShort(0);
                output->writeShort(0);
                output->writeShort(33);
                output->writeInt(static_cast<int>(entry.crc));
                output->writeInt(static_cast<int>(entry.size));
                output->writeInt(static_cast<int>(entry.size));
                output->writeShort(
                    static_cast<short>(pathBytes.size()));
                output->writeShort(0);
                output->writeShort(0);
                output->writeShort(0);
                output->writeShort(0);
                output->writeInt(0);
                output->writeInt(static_cast<int>(entry.offset));
                if (!writeName(*output, entry.path))
                {
                    output.reset();
                    temporary.deleteFile();
                    return juce::Result::fail(
                        "Could not write archive directory entry "
                        + entry.path + ".");
                }
            }
            const auto directoryEnd = output->getPosition();
            const auto directorySize =
                directoryEnd - directoryOffset;
            if (directorySize < 0
                || directoryEnd > maxArchiveBytes)
            {
                output.reset();
                temporary.deleteFile();
                return juce::Result::fail(
                    "The DAWproject archive exceeds the supported classic ZIP size.");
            }
            output->writeInt(0x06054b50);
            output->writeShort(0);
            output->writeShort(0);
            output->writeShort(
                static_cast<short>(entries.size()));
            output->writeShort(
                static_cast<short>(entries.size()));
            output->writeInt(static_cast<int>(directorySize));
            output->writeInt(static_cast<int>(directoryOffset));
            output->writeShort(0);
            output->flush();
            if (output->getStatus().failed())
            {
                const auto status = output->getStatus();
                output.reset();
                temporary.deleteFile();
                return status;
            }
        }
#if STUDIO_DUO_TESTING
        runExportTestHook(
            DawProjectIO::ExportTestPhase::beforeArchiveVerification,
            temporary);
#endif
        auto headerInput = temporary.createInputStream();
        if (headerInput == nullptr)
        {
            temporary.deleteFile();
            return juce::Result::fail(
                "Could not reopen the staged DAWproject archive headers.");
        }
        for (const auto& entry : entries)
        {
            std::array<std::uint8_t, 30> header {};
            if (!headerInput->setPosition(entry.offset)
                || headerInput->read(
                       header.data(),
                       static_cast<int>(header.size()))
                       != static_cast<int>(header.size()))
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive contains a truncated local header.");
            }
            const auto nameLength =
                readLittleEndian16(header.data() + 26);
            const auto extraLength =
                readLittleEndian16(header.data() + 28);
            if (readLittleEndian32(header.data()) != 0x04034b50u
                || readLittleEndian16(header.data() + 6)
                       != (1u << 11)
                || readLittleEndian16(header.data() + 8) != 0
                || readLittleEndian32(header.data() + 14)
                       != entry.crc
                || readLittleEndian32(header.data() + 18)
                       != entry.size
                || readLittleEndian32(header.data() + 22)
                       != entry.size
                || extraLength != 0)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive local size or CRC metadata is inconsistent.");
            }
            juce::MemoryBlock name(nameLength, false);
            if (headerInput->read(
                    name.getData(),
                    static_cast<int>(name.getSize()))
                    != static_cast<int>(name.getSize())
                || juce::String::fromUTF8(
                       static_cast<const char*>(name.getData()),
                       static_cast<int>(name.getSize()))
                       != entry.path)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive local entry name is inconsistent.");
            }
        }
        if (!headerInput->setPosition(directoryOffset))
        {
            temporary.deleteFile();
            return juce::Result::fail(
                "The staged DAWproject archive directory is unreadable.");
        }
        for (const auto& entry : entries)
        {
            std::array<std::uint8_t, 46> header {};
            if (headerInput->read(
                    header.data(),
                    static_cast<int>(header.size()))
                    != static_cast<int>(header.size()))
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive contains a truncated directory entry.");
            }
            const auto nameLength =
                readLittleEndian16(header.data() + 28);
            const auto extraLength =
                readLittleEndian16(header.data() + 30);
            const auto commentLength =
                readLittleEndian16(header.data() + 32);
            if (readLittleEndian32(header.data()) != 0x02014b50u
                || readLittleEndian16(header.data() + 8)
                       != (1u << 11)
                || readLittleEndian16(header.data() + 10) != 0
                || readLittleEndian32(header.data() + 16)
                       != entry.crc
                || readLittleEndian32(header.data() + 20)
                       != entry.size
                || readLittleEndian32(header.data() + 24)
                       != entry.size
                || readLittleEndian32(header.data() + 42)
                       != entry.offset
                || extraLength != 0
                || commentLength != 0)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive directory size or CRC metadata is inconsistent.");
            }
            juce::MemoryBlock name(nameLength, false);
            if (headerInput->read(
                    name.getData(),
                    static_cast<int>(name.getSize()))
                    != static_cast<int>(name.getSize())
                || juce::String::fromUTF8(
                       static_cast<const char*>(name.getData()),
                       static_cast<int>(name.getSize()))
                       != entry.path)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive directory entry name is inconsistent.");
            }
        }
        headerInput.reset();
        juce::ZipFile verification(temporary);
        if (verification.getNumEntries()
            != static_cast<int>(entries.size()))
        {
            temporary.deleteFile();
            return juce::Result::fail(
                "The staged DAWproject archive failed verification.");
        }
        for (auto index = 0;
             index < verification.getNumEntries();
             ++index)
        {
            const auto* verifiedEntry =
                verification.getEntry(index);
            if (verifiedEntry == nullptr)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive contains unreadable entry metadata.");
            }
            const auto& prepared =
                entries[static_cast<std::size_t>(index)];
            if (verifiedEntry->filename != prepared.path
                || verifiedEntry->uncompressedSize
                       != prepared.size)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "The staged DAWproject archive entry has an unexpected uncompressed size: "
                    + verifiedEntry->filename);
            }
            std::unique_ptr<juce::InputStream> input(
                verification.createStreamForEntry(index));
            if (input == nullptr)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "Could not reopen staged archive entry "
                    + verifiedEntry->filename + ".");
            }
            Crc32 crc;
            juce::int64 totalRead = 0;
            std::array<std::uint8_t, 64 * 1024> buffer {};
            for (;;)
            {
                const auto read = input->read(
                    buffer.data(),
                    static_cast<int>(buffer.size()));
                if (read <= 0)
                    break;
                if (totalRead
                    > static_cast<juce::int64>(prepared.size)
                        - read)
                {
                    temporary.deleteFile();
                    return juce::Result::fail(
                        "Staged archive entry expands beyond its declared size: "
                        + verifiedEntry->filename);
                }
                crc.update(
                    buffer.data(),
                    static_cast<std::size_t>(read));
                totalRead += read;
            }
            if (totalRead != prepared.size)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "Staged archive entry size verification failed: "
                    + verifiedEntry->filename);
            }
            if (crc.result() != prepared.crc)
            {
                temporary.deleteFile();
                return juce::Result::fail(
                    "Staged archive entry failed CRC verification: "
                    + verifiedEntry->filename);
            }
        }
        if (const auto cleanup = cleanupPayloadStaging();
            cleanup.failed())
        {
            temporary.deleteFile();
            return cleanup;
        }
        const auto published = destinationArchive.existsAsFile()
            ? temporary.replaceFileIn(destinationArchive)
            : temporary.moveFileTo(destinationArchive);
        if (!published)
        {
            temporary.deleteFile();
            return juce::Result::fail(
                "Could not atomically publish "
                + destinationArchive.getFullPathName() + ".");
        }
        stagingCleanup.file = juce::File();
        return juce::Result::ok();
    }
};

juce::String normalizedArchivePath(juce::String path)
{
    path = path.replaceCharacter('\\', '/');
    while (path.startsWith("./"))
        path = path.substring(2);
    return path;
}

bool safeArchivePath(const juce::String& original)
{
    const auto path = normalizedArchivePath(original);
    if (path.isEmpty()
        || path.startsWithChar('/')
        || juce::File::isAbsolutePath(path)
        || path.containsChar(':'))
    {
        return false;
    }
    const auto segments =
        juce::StringArray::fromTokens(path, "/", {});
    if (segments.isEmpty())
        return false;
    for (const auto& segment : segments)
        if (segment.isEmpty() || segment == "." || segment == "..")
            return false;
    return true;
}

juce::String textHash(const juce::String& text)
{
    const auto utf8 = text.toStdString();
    return juce::SHA256(utf8.data(), utf8.size())
        .toHexString()
        .toLowerCase();
}

class ArchiveReader
{
public:
    explicit ArchiveReader(const juce::File& archive)
        : source(archive),
          zip(archive)
    {
    }

    juce::Result validate()
    {
        if (!source.existsAsFile())
            return juce::Result::fail(
                "The selected DAWproject archive does not exist.");
        if (zip.getNumEntries() <= 0)
            return juce::Result::fail(
                "The selected file is not a readable ZIP archive.");
        if (const auto central = parseCentralDirectory();
            central.failed())
            return central;
        if (!has(projectEntryName) || !has(metadataEntryName))
            return juce::Result::fail(
                "The DAWproject archive must contain project.xml and metadata.xml at its root.");
        return juce::Result::ok();
    }

    bool has(const juce::String& path) const
    {
        return paths.find(normalizedArchivePath(path)) != paths.cend();
    }

    std::optional<juce::String> readText(const juce::String& path,
                                         juce::String& error)
    {
        const auto normalized = normalizedArchivePath(path);
        const auto found = paths.find(normalized);
        if (found == paths.cend())
        {
            error = "Archive entry is missing: " + normalized;
            return std::nullopt;
        }
        if (found->second.uncompressedSize > maxXmlBytes)
        {
            error = "XML entry is too large: " + normalized;
            return std::nullopt;
        }
        juce::MemoryBlock data;
        if (!readEntry(normalized, found->second, error,
                       [&data](const void* bytes, std::size_t size)
                       {
                           data.append(bytes, size);
                           return true;
                       }))
            return std::nullopt;
        return juce::String::fromUTF8(
            static_cast<const char*>(data.getData()),
            static_cast<int>(data.getSize()));
    }

    bool readData(const juce::String& path,
                  juce::MemoryBlock& data,
                  juce::String& error,
                  juce::int64 maximumBytes = 512 * 1024 * 1024)
    {
        const auto normalized = normalizedArchivePath(path);
        const auto found = paths.find(normalized);
        if (found == paths.cend())
        {
            error = "Archive entry is missing: " + normalized;
            return false;
        }
        if (found->second.uncompressedSize > maximumBytes)
        {
            error = "Archive entry exceeds the supported size: " + normalized;
            return false;
        }
        data.reset();
        return readEntry(
            normalized,
            found->second,
            error,
            [&data](const void* bytes, std::size_t size)
            {
                data.append(bytes, size);
                return true;
            });
    }

    bool copyTo(const juce::String& path,
                const juce::File& destination,
                juce::String& error)
    {
        const auto normalized = normalizedArchivePath(path);
        const auto found = paths.find(normalized);
        if (found == paths.cend())
        {
            error = "Archive entry is missing: " + normalized;
            return false;
        }
        if (!destination.getParentDirectory().createDirectory())
        {
            error = "Could not prepare imported media: "
                + destination.getFullPathName();
            return false;
        }
        const auto temporary = destination.getSiblingFile(
            destination.getFileName()
            + ".tmp-" + juce::Uuid().toString());
        {
            auto output = temporary.createOutputStream();
            if (output == nullptr)
            {
                error = "Could not create imported media: "
                    + temporary.getFullPathName();
                return false;
            }
            if (!readEntry(
                    normalized,
                    found->second,
                    error,
                    [&output](const void* bytes, std::size_t size)
                    {
                        return output->write(bytes, size);
                    }))
            {
                output.reset();
                temporary.deleteFile();
                return false;
            }
            output->flush();
            if (output->getStatus().failed())
            {
                error = output->getStatus().getErrorMessage();
                output.reset();
                temporary.deleteFile();
                return false;
            }
        }
        const auto published = destination.existsAsFile()
            ? temporary.replaceFileIn(destination)
            : temporary.moveFileTo(destination);
        if (!published)
        {
            temporary.deleteFile();
            error = "Could not publish imported media: "
                + destination.getFullPathName();
            return false;
        }
        return true;
    }

private:
    struct EntryMetadata
    {
        int index = -1;
        juce::int64 uncompressedSize = 0;
        std::uint32_t crc = 0;
    };

    juce::File source;
    juce::ZipFile zip;
    std::map<juce::String, EntryMetadata> paths;

    juce::Result parseCentralDirectory()
    {
        const auto fileSize = source.getSize();
        if (fileSize < 22 || fileSize > maxArchiveBytes)
            return juce::Result::fail(
                "The DAWproject archive exceeds the supported classic ZIP size.");
        auto input = source.createInputStream();
        if (input == nullptr)
            return juce::Result::fail(
                "Could not read the DAWproject archive.");
        const auto tailSize = static_cast<std::size_t>(
            std::min<juce::int64>(fileSize, 1024 * 1024));
        juce::MemoryBlock tail(tailSize, true);
        if (!input->setPosition(fileSize - static_cast<juce::int64>(tailSize))
            || input->read(
                   tail.getData(),
                   static_cast<int>(tailSize))
                   != static_cast<int>(tailSize))
        {
            return juce::Result::fail(
                "Could not read the ZIP central directory.");
        }
        const auto* tailBytes =
            static_cast<const std::uint8_t*>(tail.getData());
        std::optional<std::size_t> endOffset;
        for (auto offset = tailSize - 22;; --offset)
        {
            if (readLittleEndian32(tailBytes + offset)
                == 0x06054b50u)
            {
                endOffset = offset;
                break;
            }
            if (offset == 0)
                break;
        }
        if (!endOffset.has_value())
            return juce::Result::fail(
                "The ZIP end-of-directory record is missing.");
        const auto* end = tailBytes + *endOffset;
        if (readLittleEndian16(end + 4) != 0
            || readLittleEndian16(end + 6) != 0)
        {
            return juce::Result::fail(
                "Multi-disk ZIP archives are not supported.");
        }
        const auto entriesOnDisk = readLittleEndian16(end + 8);
        const auto entryCount = readLittleEndian16(end + 10);
        const auto directorySize = readLittleEndian32(end + 12);
        const auto directoryOffset = readLittleEndian32(end + 16);
        if (entriesOnDisk == 0xffffu
            || entryCount == 0xffffu
            || directorySize == 0xffffffffu
            || directoryOffset == 0xffffffffu)
        {
            return juce::Result::fail(
                "ZIP64 DAWproject archives are not supported by this build.");
        }
        if (entryCount != entriesOnDisk
            || entryCount == 0
            || entryCount > 10000
            || static_cast<juce::int64>(directoryOffset)
                       + directorySize
                   > fileSize
            || directorySize > 64 * 1024 * 1024)
        {
            return juce::Result::fail(
                "The ZIP central directory is invalid or too large.");
        }
        juce::MemoryBlock directory(directorySize, true);
        if (!input->setPosition(directoryOffset)
            || input->read(
                   directory.getData(),
                   static_cast<int>(directorySize))
                   != static_cast<int>(directorySize))
        {
            return juce::Result::fail(
                "The ZIP central directory is truncated.");
        }
        const auto* bytes =
            static_cast<const std::uint8_t*>(directory.getData());
        std::size_t position = 0;
        juce::int64 totalSize = 0;
        paths.clear();
        for (auto index = 0; index < entryCount; ++index)
        {
            if (position + 46 > directorySize
                || readLittleEndian32(bytes + position)
                    != 0x02014b50u)
            {
                return juce::Result::fail(
                    "The ZIP central directory contains a malformed entry.");
            }
            const auto flags =
                readLittleEndian16(bytes + position + 8);
            const auto method =
                readLittleEndian16(bytes + position + 10);
            const auto crc =
                readLittleEndian32(bytes + position + 16);
            const auto compressedSize =
                readLittleEndian32(bytes + position + 20);
            const auto uncompressedSize =
                readLittleEndian32(bytes + position + 24);
            const auto nameLength =
                readLittleEndian16(bytes + position + 28);
            const auto extraLength =
                readLittleEndian16(bytes + position + 30);
            const auto commentLength =
                readLittleEndian16(bytes + position + 32);
            const auto localOffset =
                readLittleEndian32(bytes + position + 42);
            const auto entrySize =
                static_cast<std::size_t>(46)
                + nameLength + extraLength + commentLength;
            if (position + entrySize > directorySize
                || nameLength == 0
                || (flags & 1u) != 0
                || (method != 0 && method != 8)
                || localOffset >= static_cast<std::uint64_t>(fileSize)
                || compressedSize > maxArchiveBytes
                || uncompressedSize > maxArchiveBytes
                || totalSize > maxArchiveBytes - uncompressedSize)
            {
                return juce::Result::fail(
                    "The ZIP entry is encrypted, unsupported, oversized, or malformed.");
            }
            const auto path = normalizedArchivePath(
                juce::String::fromUTF8(
                    reinterpret_cast<const char*>(
                        bytes + position + 46),
                    nameLength));
            if (!safeArchivePath(path))
                return juce::Result::fail(
                    "The DAWproject archive contains an unsafe path: "
                    + path);
            const auto* juceEntry = zip.getEntry(index);
            if (juceEntry == nullptr
                || juceEntry->isSymbolicLink
                || normalizedArchivePath(juceEntry->filename) != path
                || juceEntry->uncompressedSize
                       != static_cast<juce::int64>(uncompressedSize))
            {
                return juce::Result::fail(
                    "The ZIP entry metadata is inconsistent.");
            }
            if (!paths.emplace(
                    path,
                    EntryMetadata {
                        index,
                        static_cast<juce::int64>(uncompressedSize),
                        crc
                    })
                     .second)
            {
                return juce::Result::fail(
                    "The DAWproject archive contains a duplicate path: "
                    + path);
            }
            totalSize += uncompressedSize;
            position += entrySize;
        }
        if (zip.getNumEntries() != entryCount)
            return juce::Result::fail(
                "The ZIP entry count is inconsistent.");
        return juce::Result::ok();
    }

    template <typename Consumer>
    bool readEntry(const juce::String& path,
                   const EntryMetadata& metadata,
                   juce::String& error,
                   Consumer&& consumer)
    {
        std::unique_ptr<juce::InputStream> input(
            zip.createStreamForEntry(metadata.index));
        if (input == nullptr)
        {
            error = "Could not read archive entry: " + path;
            return false;
        }
        Crc32 crc;
        std::array<std::uint8_t, 64 * 1024> buffer {};
        juce::int64 remaining = metadata.uncompressedSize;
        while (remaining > 0)
        {
            const auto requested = static_cast<int>(
                std::min<juce::int64>(
                    remaining,
                    static_cast<juce::int64>(buffer.size())));
            const auto read = input->read(buffer.data(), requested);
            if (read <= 0)
            {
                error = "Archive entry is truncated: " + path;
                return false;
            }
            crc.update(buffer.data(), static_cast<std::size_t>(read));
            if (!consumer(buffer.data(), static_cast<std::size_t>(read)))
            {
                error = "Could not consume archive entry: " + path;
                return false;
            }
            remaining -= read;
        }
        std::uint8_t extra = 0;
        if (input->read(&extra, 1) > 0)
        {
            error = "Archive entry expands beyond its declared size: " + path;
            return false;
        }
        if (crc.result() != metadata.crc)
        {
            error = "Archive entry failed its CRC check: " + path;
            return false;
        }
        return true;
    }
};

enum class ParameterDomain
{
    raw,
    gainDecibels,
    pan,
    boolean
};

double convertDawValue(double value,
                       ParameterDomain domain,
                       const juce::String& unit,
                       double minimum,
                       double maximum)
{
    if (domain == ParameterDomain::gainDecibels)
    {
        if (unit == "decibel")
            return value;
        if (unit == "percent")
            value /= 100.0;
        return juce::Decibels::gainToDecibels(
            std::max(0.0, value),
            -100.0);
    }
    if (domain == ParameterDomain::pan)
    {
        if (unit == "percent")
        {
            minimum = 0.0;
            maximum = 100.0;
        }
        if (maximum <= minimum)
        {
            minimum = unit == "normalized" ? 0.0 : -1.0;
            maximum = 1.0;
        }
        const auto normalized =
            (value - minimum) / (maximum - minimum);
        return juce::jlimit(-1.0, 1.0, normalized * 2.0 - 1.0);
    }
    return value;
}

double convertedElementValue(const juce::XmlElement& element,
                             ParameterDomain domain,
                             double fallback)
{
    const auto unit = element.getStringAttribute(
        "unit",
        domain == ParameterDomain::gainDecibels
            ? "linear"
            : domain == ParameterDomain::pan
                ? "normalized"
                : "linear");
    const auto defaultMinimum =
        domain == ParameterDomain::pan && unit == "normalized"
        ? 0.0
        : -1.0;
    const auto defaultMaximum = 1.0;
    return convertDawValue(
        element.getDoubleAttribute("value", fallback),
        domain,
        unit,
        element.getDoubleAttribute("min", defaultMinimum),
        element.getDoubleAttribute("max", defaultMaximum));
}

struct ParameterBinding
{
    AutomationTarget target;
    bool booleanValue = false;
    juce::String name;
    ParameterDomain domain = ParameterDomain::raw;
    juce::String unit;
    double minimum = 0.0;
    double maximum = 1.0;
    juce::String path;
    bool validDomain = true;
    juce::String domainError;
};

struct PendingMainRoute
{
    juce::String sourceTrackId;
    juce::String destinationChannelId;
    juce::String path;
};

struct PendingSend
{
    juce::String id;
    juce::String name;
    juce::String sourceTrackId;
    juce::String destinationChannelId;
    RouteTap tap = RouteTap::postFader;
    float gainDecibels = 0.0f;
    float pan = 0.0f;
    juce::String path;
};

std::vector<const juce::XmlElement*> xmlChildren(
    const juce::XmlElement& parent)
{
    std::vector<const juce::XmlElement*> children;
    for (auto* child = parent.getFirstChildElement();
         child != nullptr;
         child = child->getNextElement())
    {
        if (!child->isTextElement())
            children.push_back(child);
    }
    return children;
}

juce::String xmlChildPath(
    const juce::String& parentPath,
    const std::vector<const juce::XmlElement*>& children,
    std::size_t index)
{
    auto occurrence = 0;
    const auto name = children[index]->getTagName();
    for (std::size_t candidate = 0; candidate <= index; ++candidate)
        if (children[candidate]->hasTagName(name))
            ++occurrence;
    return parentPath + "/" + name + "[" + juce::String(occurrence)
        + "]";
}

bool boolAttribute(const juce::XmlElement& element,
                   const char* name,
                   bool fallback)
{
    if (!element.hasAttribute(name))
        return fallback;
    const auto value = element.getStringAttribute(name);
    return value == "true" || value == "1";
}

class Importer
{
public:
    Importer(const juce::File& source,
             const juce::File& destination)
        : sourceArchive(source),
          destinationPackage(
              ProjectFile::normalisePackagePath(destination)),
          report(makeReport("import", source, destinationPackage)),
          archive(source)
    {
        formats.registerBasicFormats();
    }

    DawProjectImportResult run()
    {
        DawProjectImportResult output;
        output.package = destinationPackage;
        if (const auto archiveResult = archive.validate();
            archiveResult.failed())
        {
            fail(
                "import.archive",
                "/",
                archiveResult.getErrorMessage());
            return finishFailure(std::move(output));
        }
        juce::String error;
        const auto projectText =
            archive.readText(projectEntryName, error);
        if (!projectText.has_value())
        {
            fail("import.project-xml", "/project.xml", error);
            return finishFailure(std::move(output));
        }
        const auto metadataText =
            archive.readText(metadataEntryName, error);
        if (!metadataText.has_value())
        {
            fail("import.metadata-xml", "/metadata.xml", error);
            return finishFailure(std::move(output));
        }

        const auto projectValidation =
            DawProjectSchemaValidator::validateProjectXml(*projectText);
        const auto metadataValidation =
            DawProjectSchemaValidator::validateMetadataXml(*metadataText);
        appendSchemaIssues(
            projectValidation,
            "schema.project");
        appendSchemaIssues(
            metadataValidation,
            "schema.metadata");
        if (!projectValidation.valid || !metadataValidation.valid)
            return finishFailure(std::move(output));

        juce::XmlDocument projectDocument(*projectText);
        juce::XmlDocument metadataDocument(*metadataText);
        projectXml = projectDocument.getDocumentElement();
        metadataXml = metadataDocument.getDocumentElement();
        if (projectXml == nullptr || metadataXml == nullptr)
        {
            fail(
                "import.xml",
                "/",
                "The schema-valid XML could not be parsed.");
            return finishFailure(std::move(output));
        }
        validateSemantics(*projectXml, "/Project");
        if (fatal)
            return finishFailure(std::move(output));
        if (destinationPackage.exists())
        {
            fail(
                "import.destination-exists",
                "/",
                "The import destination already exists; choose a new Studio Duo project path.");
            return finishFailure(std::move(output));
        }

        stagingPackage = destinationPackage.getSiblingFile(
            destinationPackage.getFileNameWithoutExtension()
            + ".import-" + juce::Uuid().toString()
            + ".studioduo");
        if (!stagingPackage.createDirectory())
        {
            fail(
                "import.staging",
                "/",
                "Could not create an import staging package.");
            return finishFailure(std::move(output));
        }

        translateMetadata();
        translateProject();
        if (fatal)
            return finishFailure(std::move(output));
        ensureMasterTrack();
        resolveRoutes();
        if (fatal)
            return finishFailure(std::move(output));

        project.compatibilityReports.push_back(report);
        juce::String validationError;
        const auto validated =
            Project::fromVar(project.toVar(), validationError);
        if (!validated.has_value())
        {
            fail(
                "import.project-model",
                "/Project",
                "The translated Studio Duo project is invalid: "
                    + validationError);
            return finishFailure(std::move(output));
        }
        project = std::move(*validated);

        if (const auto saveResult =
                ProjectFile::save(project, stagingPackage);
            saveResult.failed())
        {
            fail(
                "import.native-save",
                "/Project",
                saveResult.getErrorMessage());
            return finishFailure(std::move(output));
        }
        juce::String reopenError;
        const auto reopened =
            ProjectFile::load(stagingPackage, reopenError);
        if (!reopened.has_value() || reopened->id != project.id)
        {
            fail(
                "import.native-verify",
                "/Project",
                reopenError.isNotEmpty()
                    ? reopenError
                    : juce::String(
                          "The staged native project failed verification."));
            return finishFailure(std::move(output));
        }
        if (!publishStaging())
            return finishFailure(std::move(output));

        output.result = juce::Result::ok();
        output.project = project;
        output.report = report;
        return output;
    }

private:
    juce::File sourceArchive;
    juce::File destinationPackage;
    CompatibilityReport report;
    ArchiveReader archive;
    DawProjectIdMapper ids;
    Project project;
    std::unique_ptr<juce::XmlElement> projectXml;
    std::unique_ptr<juce::XmlElement> metadataXml;
    juce::File stagingPackage;
    std::map<juce::String, ParameterBinding> parameterBindings;
    std::vector<PendingMainRoute> pendingMainRoutes;
    std::vector<PendingSend> pendingSends;
    std::map<juce::String, juce::String> importedMedia;
    juce::AudioFormatManager formats;
    bool fatal = false;
    juce::String firstError;

    void fail(const juce::String& code,
              const juce::String& path,
              const juce::String& message)
    {
        addIssue(
            report,
            CompatibilitySeverity::error,
            code,
            path,
            message);
        fatal = true;
        if (firstError.isEmpty())
            firstError = message;
    }

    void warn(const juce::String& code,
              const juce::String& path,
              const juce::String& message)
    {
        addIssue(
            report,
            CompatibilitySeverity::warning,
            code,
            path,
            message);
    }

    DawProjectImportResult finishFailure(
        DawProjectImportResult output)
    {
        if (stagingPackage.exists())
            stagingPackage.deleteRecursively();
        output.result = juce::Result::fail(
            firstError.isNotEmpty()
                ? firstError
                : juce::String("DAWproject import failed."));
        output.report = report;
        output.project.reset();
        output.package = juce::File();
        return output;
    }

    void appendSchemaIssues(
        const DawProjectValidationResult& validation,
        const juce::String& code)
    {
        for (const auto& issue : validation.issues)
        {
            fail(
                code,
                issue.objectPath,
                "Official schema validation failed: "
                    + issue.message);
        }
    }

    static bool isTimelineContent(const juce::XmlElement& element)
    {
        return element.hasTagName("Timeline")
            || element.hasTagName("Lanes")
            || element.hasTagName("Notes")
            || element.hasTagName("Clips")
            || element.hasTagName("ClipSlot")
            || element.hasTagName("markers")
            || element.hasTagName("Warps")
            || element.hasTagName("Audio")
            || element.hasTagName("Video")
            || element.hasTagName("Points");
    }

    void validateSemantics(const juce::XmlElement& element,
                           const juce::String& path)
    {
        const auto children = xmlChildren(element);
        const auto finiteAttribute =
            [this, &element, &path](
                const char* name,
                bool required,
                bool positive)
        {
            if (!element.hasAttribute(name))
            {
                if (required)
                    fail(
                        "semantic.number",
                        path + "/@" + name,
                        "The numeric attribute is required.");
                return;
            }
            double value = 0.0;
            if (!parseFiniteNumber(
                    element.getStringAttribute(name),
                    value)
                || (positive && value <= 0.0))
            {
                fail(
                    "semantic.number",
                    path + "/@" + name,
                    "The numeric attribute must be finite"
                        + juce::String(
                              positive ? " and positive." : "."));
            }
        };
        if (element.hasTagName("TempoAutomation"))
        {
            for (const auto* child : children)
            {
                if (!child->hasTagName("RealPoint"))
                    continue;
                double bpm = 0.0;
                if (!parseFiniteNumber(
                        child->getStringAttribute("value"),
                        bpm)
                    || bpm < 20.0
                    || bpm > 400.0)
                {
                    fail(
                        "semantic.tempo",
                        path,
                        "Tempo automation values must be finite and between 20 and 400 BPM.");
                }
            }
        }
        if (element.hasTagName("Clip"))
        {
            finiteAttribute("time", true, false);
            finiteAttribute("duration", false, true);
            finiteAttribute("playStart", false, false);
            finiteAttribute("playStop", false, false);
            finiteAttribute("loopStart", false, false);
            finiteAttribute("loopEnd", false, false);
            finiteAttribute("fadeInTime", false, false);
            finiteAttribute("fadeOutTime", false, false);
            const auto contentCount = std::count_if(
                children.cbegin(),
                children.cend(),
                [](const auto* child)
                {
                    return isTimelineContent(*child);
                });
            if (element.hasAttribute("reference") && contentCount != 0)
                fail(
                    "semantic.clip-reference",
                    path,
                    "A clip cannot contain timeline content and a reference.");
        }
        else if (element.hasTagName("Target"))
        {
            const auto hasParameter =
                element.hasAttribute("parameter");
            const auto hasExpression =
                element.hasAttribute("expression");
            if (hasParameter == hasExpression)
                fail(
                    "semantic.automation-target",
                    path,
                    "An automation target must identify exactly one parameter or expression.");
            if ((element.hasAttribute("channel")
                 && (element.getIntAttribute("channel") < 0
                     || element.getIntAttribute("channel") > 15))
                || (element.hasAttribute("key")
                    && (element.getIntAttribute("key") < 0
                        || element.getIntAttribute("key") > 127))
                || (element.hasAttribute("controller")
                    && (element.getIntAttribute("controller") < 0
                        || element.getIntAttribute("controller") > 127)))
            {
                fail(
                    "semantic.automation-target-range",
                    path,
                    "Automation target channel, key, or controller values are outside MIDI ranges.");
            }
        }
        else if (element.hasTagName("Points"))
        {
            juce::String pointType;
            for (const auto* child : children)
            {
                if (child->hasTagName("Target"))
                    continue;
                if (pointType.isEmpty())
                    pointType = child->getTagName();
                else if (pointType != child->getTagName())
                    fail(
                        "semantic.point-types",
                        path,
                        "An automation timeline must use one homogeneous point type.");
            }
        }
        else if (element.hasTagName("Warps"))
        {
            auto warpCount = 0;
            auto previousTime = -std::numeric_limits<double>::infinity();
            for (const auto* child : children)
            {
                if (!child->hasTagName("Warp"))
                    continue;
                ++warpCount;
                double time = 0.0;
                double contentTime = 0.0;
                if (!parseFiniteNumber(
                        child->getStringAttribute("time"),
                        time)
                    || !parseFiniteNumber(
                        child->getStringAttribute("contentTime"),
                        contentTime))
                {
                    fail(
                        "semantic.warp-number",
                        path,
                        "Warp positions must be finite numbers.");
                }
                if (!std::isfinite(time) || time <= previousTime)
                    fail(
                        "semantic.warp-order",
                        path,
                        "Warp timeline positions must be finite and strictly increasing.");
                previousTime = time;
            }
            if (warpCount < 2)
                fail(
                    "semantic.warp-count",
                    path,
                    "A usable DAWproject warp mapping requires at least two points.");
        }
        else if (element.hasTagName("Note"))
        {
            finiteAttribute("time", true, false);
            finiteAttribute("duration", true, true);
            finiteAttribute("vel", false, false);
            finiteAttribute("rel", false, false);
            const auto channel = element.getIntAttribute("channel");
            const auto key = element.getIntAttribute("key");
            const auto velocity =
                element.getDoubleAttribute("vel", 1.0);
            const auto release =
                element.getDoubleAttribute("rel", 0.5);
            if (channel < 0
                || channel > 15
                || key < 0
                || key > 127
                || velocity < 0.0
                || velocity > 1.0
                || release < 0.0
                || release > 1.0)
            {
                fail(
                    "semantic.note-range",
                    path,
                    "MIDI channels, keys, and normalized velocities are outside supported ranges.");
            }
        }
        else if (element.hasTagName("TimeSignature")
                 || element.hasTagName("TimeSignaturePoint"))
        {
            const auto numerator =
                element.getIntAttribute("numerator");
            const auto denominator =
                element.getIntAttribute("denominator");
            const auto denominatorSupported =
                denominator == 1
                || denominator == 2
                || denominator == 4
                || denominator == 8
                || denominator == 16
                || denominator == 32;
            if (numerator < 1
                || numerator > 32
                || !denominatorSupported)
            {
                fail(
                    "semantic.time-signature",
                    path,
                    "The time signature is outside Studio Duo's supported range.");
            }
        }
        else if (element.hasTagName("File")
                 || element.hasTagName("State"))
        {
            const auto reference =
                element.getStringAttribute("path");
            if (!boolAttribute(element, "external", false)
                && !safeArchivePath(reference))
            {
                fail(
                    "semantic.file-path",
                    path,
                    "Embedded file references must use safe relative archive paths.");
            }
            if (element.hasTagName("State")
                && boolAttribute(element, "external", false))
            {
                fail(
                    "semantic.device-state",
                    path,
                    "DAWproject plug-in state must be embedded.");
            }
        }
        else if (element.hasTagName("Audio")
                 || element.hasTagName("Video"))
        {
            finiteAttribute("duration", true, true);
            if (element.getIntAttribute("channels") <= 0
                || element.getIntAttribute("sampleRate") <= 0)
            {
                fail(
                    "semantic.media-format",
                    path,
                    "Media channels and sample rate must be positive.");
            }
        }
        else if (element.hasTagName("Channel")
                 && element.hasAttribute("audioChannels")
                 && element.getIntAttribute("audioChannels") <= 0)
        {
            fail(
                "semantic.channel-layout",
                path,
                "Channel audioChannels must be positive.");
        }
        else if (element.hasTagName("RealPoint"))
        {
            finiteAttribute("time", true, false);
            finiteAttribute("value", true, false);
        }
        else if (element.hasTagName("EnumPoint")
                 || element.hasTagName("BoolPoint")
                 || element.hasTagName("IntegerPoint")
                 || element.hasTagName("TimeSignaturePoint"))
        {
            finiteAttribute("time", true, false);
        }
        else if (element.hasTagName("RealParameter"))
        {
            finiteAttribute("value", false, false);
            for (const auto* name : { "min", "max" })
            {
                if (!element.hasAttribute(name))
                    continue;
                double value = 0.0;
                if (!parseFiniteNumber(
                        element.getStringAttribute(name),
                        value))
                {
                    fail(
                        "semantic.parameter-domain",
                        path + "/@" + name,
                        "Parameter domain bounds must be finite numbers.");
                }
            }
            if (element.getStringAttribute("unit") == "bpm"
                && element.hasAttribute("value"))
            {
                const auto bpm =
                    element.getDoubleAttribute("value");
                if (bpm < 20.0 || bpm > 400.0)
                    fail(
                        "semantic.tempo",
                        path,
                        "Tempo values must be between 20 and 400 BPM.");
            }
        }
        for (std::size_t index = 0; index < children.size(); ++index)
            validateSemantics(
                *children[index],
                xmlChildPath(path, children, index));
    }

    void translateMetadata()
    {
        project = Project {};
        juce::String mappingError;
        project.id = ids.importedId(
            "project",
            juce::SHA256(sourceArchive).toHexString(),
            mappingError);
        if (project.id.isEmpty())
        {
            fail("import.project-id", "/Project", mappingError);
            return;
        }
        project.name = sourceArchive.getFileNameWithoutExtension();
        const auto read = [this](const char* name)
        {
            if (const auto* element =
                    metadataXml->getChildByName(name))
                return element->getAllSubText();
            return juce::String();
        };
        const auto title = read("Title");
        if (title.isNotEmpty())
            project.name = title;
        project.metadata.artist = read("Artist");
        project.metadata.album = read("Album");
        project.metadata.originalArtist = read("OriginalArtist");
        project.metadata.composer = read("Composer");
        project.metadata.songwriter = read("Songwriter");
        project.metadata.producer = read("Producer");
        project.metadata.arranger = read("Arranger");
        project.metadata.year = read("Year");
        project.metadata.genre = read("Genre");
        project.metadata.copyright = read("Copyright");
        project.metadata.website = read("Website");
        project.metadata.comment = read("Comment");
    }

    void translateProject()
    {
        translateTransport();
        if (const auto* structure =
                projectXml->getChildByName("Structure"))
            translateStructure(*structure);
        if (const auto* arrangement =
                projectXml->getChildByName("Arrangement"))
            translateArrangement(*arrangement);
        if (const auto* scenes =
                projectXml->getChildByName("Scenes"))
            translateScenes(*scenes);
    }

    void translateTransport()
    {
        const auto* transport =
            projectXml->getChildByName("Transport");
        if (transport == nullptr)
            return;
        if (const auto* tempo = transport->getChildByName("Tempo"))
        {
            project.tempo =
                juce::jlimit(
                    20.0,
                    400.0,
                    tempo->getDoubleAttribute("value", 120.0));
            bindParameter(
                *tempo,
                {
                    AutomationTargetType::deviceParameter,
                    {},
                    {},
                    {},
                    "transport-tempo",
                    -1
                },
                false,
                ParameterDomain::raw,
                "Tempo",
                "/Project/Transport/Tempo");
        }
        if (const auto* signature =
                transport->getChildByName("TimeSignature"))
        {
            project.timeSignatureNumerator =
                signature->getIntAttribute("numerator", 4);
            project.timeSignatureDenominator =
                signature->getIntAttribute("denominator", 4);
            bindParameter(
                *signature,
                {
                    AutomationTargetType::deviceParameter,
                    {},
                    {},
                    {},
                    "transport-time-signature",
                    -1
                },
                false,
                ParameterDomain::raw,
                "Time signature",
                "/Project/Transport/TimeSignature");
        }
    }

    void translateStructure(const juce::XmlElement& structure)
    {
        const auto children = xmlChildren(structure);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            const auto path =
                xmlChildPath("/Project/Structure", children, index);
            if (children[index]->hasTagName("Track"))
                translateTrack(*children[index], {}, path);
            else if (children[index]->hasTagName("Channel"))
                translateStandaloneChannel(*children[index], path);
        }
    }

    juce::String externalOrPathId(
        const juce::XmlElement& element,
        const juce::String& path) const
    {
        const auto id = element.getStringAttribute("id");
        return id.isNotEmpty() ? id : path;
    }

    void translateTrack(const juce::XmlElement& element,
                        const juce::String& parentTrackId,
                        const juce::String& path)
    {
        juce::String mappingError;
        const auto externalId = externalOrPathId(element, path);
        Track track;
        track.id = ids.importedId(
            "track",
            externalId,
            mappingError);
        if (track.id.isEmpty())
        {
            fail("import.track-id", path, mappingError);
            return;
        }
        track.name =
            element.getStringAttribute("name", "Track");
        track.colour = colourFromText(
            element.getStringAttribute("color"),
            track.colour);
        if (!boolAttribute(element, "loaded", true))
        {
            warn(
                "unsupported.track-loaded-state",
                path,
                "Studio Duo imports available track content and does not preserve an unloaded-track state.");
        }
        if (element.getStringAttribute("comment").isNotEmpty())
        {
            warn(
                "unsupported.track-comment",
                path,
                "Track comments are not stored by the current Studio Duo project model.");
        }
        if (parentTrackId.isNotEmpty())
        {
            const auto* parent = project.findTrack(parentTrackId);
            if (parent != nullptr && parent->type == TrackType::folder)
            {
                track.folderTrackId = parentTrackId;
            }
            else
            {
                warn(
                    "unsupported.channel-track-hierarchy",
                    path,
                    "Studio Duo flattened a child of a channel-bearing track because native folder membership requires a folder track.");
            }
        }
        const auto* channel = element.getChildByName("Channel");
        const auto childTracks =
            element.getChildWithTagNameIterator("Track");
        if (channel == nullptr)
        {
            track.type = TrackType::folder;
        }
        else
        {
            translateChannel(*channel, track, path + "/Channel[1]");
            if (element.getStringAttribute("contentType")
                    .containsWholeWord("notes")
                && track.type == TrackType::audio)
            {
                track.type = std::any_of(
                                 track.inserts.cbegin(),
                                 track.inserts.cend(),
                                 [](const auto& insert)
                                 {
                                     return insert.format
                                         .containsIgnoreCase(
                                             "instrument");
                                 })
                    ? TrackType::instrument
                    : TrackType::midi;
            }
        }
        const auto internalId = track.id;
        project.tracks.push_back(std::move(track));

        auto childIndex = 0;
        for (const auto* child : childTracks)
        {
            ++childIndex;
            translateTrack(
                *child,
                internalId,
                path + "/Track[" + juce::String(childIndex) + "]");
        }
    }

    void translateStandaloneChannel(
        const juce::XmlElement& element,
        const juce::String& path)
    {
        juce::String mappingError;
        const auto externalId = externalOrPathId(element, path);
        Track track;
        track.id = ids.importedId(
            "track",
            externalId,
            mappingError);
        if (track.id.isEmpty())
        {
            fail("import.channel-id", path, mappingError);
            return;
        }
        translateChannel(element, track, path);
        project.tracks.push_back(std::move(track));
    }

    void translateChannel(const juce::XmlElement& channel,
                          Track& track,
                          const juce::String& path)
    {
        const auto channelExternalId =
            externalOrPathId(channel, path);
        juce::String mappingError;
        if (!ids.bindImportedId(
                "channel",
                channelExternalId,
                track.id,
                mappingError))
        {
            fail("import.channel-id", path, mappingError);
            return;
        }
        track.name = channel.getStringAttribute(
            "name",
            track.name.isNotEmpty() ? track.name : "Track");
        track.colour = colourFromText(
            channel.getStringAttribute("color"),
            track.colour);
        track.channelLayout =
            channel.getIntAttribute("audioChannels", 2) == 1
            ? ChannelLayout::mono
            : ChannelLayout::stereo;
        if (channel.getIntAttribute("audioChannels", 2) > 2)
        {
            warn(
                "unsupported.channel-layout",
                path,
                "Multichannel DAWproject channels import as stereo Studio Duo channels.");
        }
        if (channel.getStringAttribute("comment").isNotEmpty())
        {
            warn(
                "unsupported.channel-comment",
                path,
                "Channel comments are not stored by the current Studio Duo project model.");
        }
        track.solo = boolAttribute(channel, "solo", false);
        const auto role =
            channel.getStringAttribute("role", "regular");
        if (role == "master")
            track.type = TrackType::master;
        else if (role == "effect")
            track.type = TrackType::aux;
        else if (role == "submix")
            track.type = TrackType::bus;
        else if (role == "vca")
            track.type = TrackType::vca;
        else
            track.type = TrackType::audio;
        if (channel.hasAttribute("destination"))
        {
            pendingMainRoutes.push_back({
                track.id,
                channel.getStringAttribute("destination"),
                path + "/@destination"
            });
        }
        if (const auto* devices = channel.getChildByName("Devices"))
            translateDevices(*devices, track, path + "/Devices[1]");
        const auto hasInstrument = std::any_of(
            track.inserts.cbegin(),
            track.inserts.cend(),
            [](const auto& insert)
            {
                return insert.format.containsIgnoreCase("instrument");
            });
            if (role == "regular" && hasInstrument)
                track.type = TrackType::instrument;
            if (const auto* mute = channel.getChildByName("Mute"))
        {
            track.muted = boolAttribute(*mute, "value", false);
            AutomationTarget target;
            target.type = AutomationTargetType::trackMute;
            target.trackId = track.id;
            bindParameter(
                *mute,
                target,
                true,
                ParameterDomain::boolean,
                "Mute",
                path + "/Mute[1]");
        }
        if (const auto* pan = channel.getChildByName("Pan"))
        {
            track.pan = static_cast<float>(
                convertedElementValue(
                    *pan,
                    ParameterDomain::pan,
                    0.5));
            AutomationTarget target;
            target.type = AutomationTargetType::trackPan;
            target.trackId = track.id;
            bindParameter(
                *pan,
                target,
                false,
                ParameterDomain::pan,
                "Pan",
                path + "/Pan[1]");
        }
        if (const auto* sends = channel.getChildByName("Sends"))
            translateSends(*sends, track, path + "/Sends[1]");
        if (const auto* volume = channel.getChildByName("Volume"))
        {
            track.volumeDecibels = static_cast<float>(
                convertedElementValue(
                    *volume,
                    ParameterDomain::gainDecibels,
                    1.0));
            AutomationTarget target;
            target.type = track.type == TrackType::vca
                ? AutomationTargetType::vcaVolume
                : AutomationTargetType::trackVolume;
            target.trackId = track.id;
            bindParameter(
                *volume,
                target,
                false,
                ParameterDomain::gainDecibels,
                "Volume",
                path + "/Volume[1]");
        }
    }

    void bindParameter(const juce::XmlElement& element,
                       AutomationTarget target,
                       bool booleanValue,
                       ParameterDomain domain,
                       const juce::String& fallbackName,
                       const juce::String& path)
    {
        const auto externalId =
            element.getStringAttribute("id");
        if (externalId.isEmpty())
            return;
        const auto unit = element.getStringAttribute(
            "unit",
            domain == ParameterDomain::gainDecibels
                ? "linear"
                : domain == ParameterDomain::pan
                    ? "normalized"
                    : "linear");
        auto minimum =
            domain == ParameterDomain::pan && unit == "normalized"
            ? 0.0
            : -1.0;
        auto maximum = 1.0;
        auto validDomain = true;
        auto domainError = juce::String();
        if (domain == ParameterDomain::raw)
        {
            const auto hasNaturalDomain =
                unit == "linear"
                || unit == "normalized"
                || unit == "percent";
            if ((!element.hasAttribute("min")
                 || !element.hasAttribute("max"))
                && !hasNaturalDomain)
            {
                validDomain = false;
                domainError =
                    "Device parameter unit '" + unit
                    + "' requires finite min and max values before automation can be normalized.";
            }
            minimum = 0.0;
            maximum = unit == "percent" ? 100.0 : 1.0;
        }
        if (element.hasAttribute("min"))
            minimum = element.getDoubleAttribute("min");
        if (element.hasAttribute("max"))
            maximum = element.getDoubleAttribute("max");
        if (domain == ParameterDomain::raw
            && (!std::isfinite(minimum)
                || !std::isfinite(maximum)
                || maximum <= minimum))
        {
            validDomain = false;
            domainError =
                "Device parameter automation requires a finite, ordered, non-zero-width min/max domain.";
        }
        if (!parameterBindings
                 .emplace(
                     externalId,
                     ParameterBinding {
                         std::move(target),
                         booleanValue,
                         element.getStringAttribute(
                             "name",
                             fallbackName),
                         domain,
                         unit,
                         minimum,
                         maximum,
                         path,
                         validDomain,
                         domainError
                     })
                 .second)
        {
            fail(
                "import.parameter-id",
                path,
                "A parameter ID is bound more than once.");
        }
    }

    void translateDevices(const juce::XmlElement& devices,
                          Track& track,
                          const juce::String& path)
    {
        const auto children = xmlChildren(devices);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            const auto* element = children[index];
            const auto objectPath =
                xmlChildPath(path, children, index);
            PluginInsert insert;
            juce::String mappingError;
            insert.id = ids.importedId(
                "device",
                externalOrPathId(*element, objectPath),
                mappingError);
            if (insert.id.isEmpty())
            {
                fail("import.device-id", objectPath, mappingError);
                continue;
            }
            insert.pluginIdentifier =
                element->getStringAttribute("deviceID");
            if (insert.pluginIdentifier.isEmpty())
                insert.pluginIdentifier = insert.id;
            insert.fileOrIdentifier = insert.pluginIdentifier;
            insert.name = element->getStringAttribute(
                "deviceName",
                element->getStringAttribute("name", "Device"));
            insert.manufacturer =
                element->getStringAttribute("deviceVendor");
            insert.version =
                element->getStringAttribute("pluginVersion");
            insert.missing =
                !boolAttribute(*element, "loaded", true);
            if (element->hasTagName("ClapPlugin"))
                insert.format = "CLAP";
            else if (element->hasTagName("Vst3Plugin"))
                insert.format = "VST3";
            else if (element->hasTagName("Vst2Plugin"))
                insert.format = "VST";
            else if (element->hasTagName("AuPlugin"))
                insert.format = "AU";
            else if (element->hasTagName("BuiltinDevice"))
            {
                if (const auto* descriptor =
                        DeviceRegistry::descriptor(
                            insert.pluginIdentifier))
                {
                    insert.format = "Studio Duo";
                    insert.name = descriptor->name;
                    insert.bundledDevice = true;
                    insert.bridgeMode =
                        PluginBridgeMode::trustedInProcess;
                    insert.missing =
                        !boolAttribute(*element, "loaded", true);
                }
                else
                {
                    insert.format = "DAWproject Builtin";
                    insert.missing = true;
                    warn(
                        "unsupported.builtin-device",
                        objectPath,
                        "The source built-in device is not available in Studio Duo and was preserved as a missing placeholder.");
                }
            }
            else
            {
                insert.format = "DAWproject";
                const auto comment =
                    element->getStringAttribute("comment");
                if (comment.startsWith("studio-duo-format:"))
                {
                    insert.format = comment.fromFirstOccurrenceOf(
                        "studio-duo-format:",
                        false,
                        false);
                }
                else
                {
                    insert.missing = true;
                    insert.stateFormat =
                        PluginStateFormat::generic;
                    warn(
                        "unsupported.generic-device",
                        objectPath,
                        "The generic or standard built-in DAWproject device has no directly loadable Studio Duo implementation and was preserved as a missing placeholder.");
                }
            }
            if (element->getStringAttribute("comment").isNotEmpty()
                && !element->getStringAttribute("comment")
                        .startsWith("studio-duo-format:"))
            {
                warn(
                    "unsupported.device-comment",
                    objectPath,
                    "Device comments are not stored by the current Studio Duo project model.");
            }
            if (element->getStringAttribute("deviceRole") == "instrument")
                insert.format += " instrument";
            if (const auto* enabled =
                    element->getChildByName("Enabled"))
                insert.bypassed =
                    !boolAttribute(*enabled, "value", true);
            if (const auto* state =
                    element->getChildByName("State"))
            {
                translateDeviceState(
                    *state,
                    insert,
                    objectPath + "/State[1]");
            }
            if (const auto* parameters =
                    element->getChildByName("Parameters"))
            {
                translateDeviceParameters(
                    *parameters,
                    track.id,
                    insert,
                    objectPath + "/Parameters[1]");
            }
            track.inserts.push_back(std::move(insert));
        }
    }

    void translateDeviceParameters(
        const juce::XmlElement& parameters,
        const juce::String& trackIdValue,
        const PluginInsert& insert,
        const juce::String& path)
    {
        const auto children = xmlChildren(parameters);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            const auto* parameter = children[index];
            const auto objectPath =
                xmlChildPath(path, children, index);
            AutomationTarget target;
            target.type = AutomationTargetType::pluginParameter;
            target.trackId = trackIdValue;
            target.insertId = insert.id;
            target.parameterId =
                parameter->getStringAttribute(
                    "name",
                    "Parameter");
            target.parameterIndex =
                parameter->getIntAttribute("parameterID", -1);
            bindParameter(
                *parameter,
                target,
                parameter->hasTagName("BoolParameter"),
                parameter->hasTagName("BoolParameter")
                    ? ParameterDomain::boolean
                    : ParameterDomain::raw,
                target.parameterId,
                objectPath);
            if (parameter->hasAttribute("value")
                && insert.stateFile.isEmpty())
            {
                warn(
                    "unsupported.device-static-parameter",
                    objectPath,
                    "The parameter value is retained only when the device state or automation restores it.");
            }
        }
    }

    void translateDeviceState(const juce::XmlElement& state,
                              PluginInsert& insert,
                              const juce::String& path)
    {
        const auto reference =
            state.getStringAttribute("path");
        juce::MemoryBlock data;
        juce::String error;
        if (!archive.readData(reference, data, error))
        {
            fail(
                "import.device-state",
                path,
                "Could not read embedded device state: " + error);
            return;
        }
        const auto stored =
            PluginStateStore::store(stagingPackage, data, error);
        if (!stored.has_value())
        {
            fail(
                "import.device-state",
                path,
                "Could not store imported device state: " + error);
            return;
        }
        insert.stateFile = stored->relativePath;
        insert.stateHash = stored->hash;
        if (insert.format.containsIgnoreCase("VST3")
            && reference.endsWithIgnoreCase(".vstpreset"))
        {
            insert.stateFormat = PluginStateFormat::vst3Preset;
        }
        else if (insert.format.containsIgnoreCase("VST")
                 && reference.endsWithIgnoreCase(".fxp"))
        {
            insert.stateFormat = PluginStateFormat::vst2Preset;
        }
        else if (insert.format.containsIgnoreCase("CLAP")
                 && reference.endsWithIgnoreCase(".clap-preset"))
        {
            insert.stateFormat = PluginStateFormat::clapPreset;
        }
        else if (insert.format.containsIgnoreCase("AU")
                 && reference.endsWithIgnoreCase(".aupreset"))
        {
            insert.stateFormat = PluginStateFormat::auPreset;
        }
        else
        {
            insert.stateFormat = insert.bundledDevice
                ? PluginStateFormat::hostOpaque
                : insert.stateFormat;
        }
        if (insert.stateFormat == PluginStateFormat::vst2Preset
            || insert.stateFormat == PluginStateFormat::vst3Preset
            || insert.stateFormat == PluginStateFormat::auPreset)
        {
            insert.missing = true;
            warn(
                "compatibility.device-state-container",
                path,
                "The format-specific preset container is preserved byte-for-byte, but this build does not adapt it to JUCE processor state; the device remains a missing placeholder for safe re-export.");
        }
    }

    void translateSends(const juce::XmlElement& sends,
                        const Track& track,
                        const juce::String& path)
    {
        const auto children = xmlChildren(sends);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            const auto* send = children[index];
            if (!send->hasTagName("Send"))
                continue;
            const auto objectPath =
                xmlChildPath(path, children, index);
            juce::String mappingError;
            PendingSend pending;
            pending.id = ids.importedId(
                "route",
                externalOrPathId(*send, objectPath),
                mappingError);
            if (pending.id.isEmpty())
            {
                fail("import.send-id", objectPath, mappingError);
                continue;
            }
            pending.name =
                send->getStringAttribute("name", "Send");
            pending.sourceTrackId = track.id;
            pending.destinationChannelId =
                send->getStringAttribute("destination");
            pending.tap =
                send->getStringAttribute("type", "post") == "pre"
                ? RouteTap::preFader
                : RouteTap::postFader;
            if (const auto* pan = send->getChildByName("Pan"))
            {
                pending.pan = static_cast<float>(
                    convertedElementValue(
                        *pan,
                        ParameterDomain::pan,
                        0.5));
                AutomationTarget target;
                target.type = AutomationTargetType::sendPan;
                target.trackId = track.id;
                target.routeId = pending.id;
                bindParameter(
                    *pan,
                    target,
                    false,
                    ParameterDomain::pan,
                    "Send pan",
                    objectPath + "/Pan[1]");
            }
            if (const auto* volume =
                    send->getChildByName("Volume"))
            {
                pending.gainDecibels = static_cast<float>(
                    convertedElementValue(
                        *volume,
                        ParameterDomain::gainDecibels,
                        1.0));
                AutomationTarget target;
                target.type = AutomationTargetType::sendGain;
                target.trackId = track.id;
                target.routeId = pending.id;
                bindParameter(
                    *volume,
                    target,
                    false,
                    ParameterDomain::gainDecibels,
                    "Send volume",
                    objectPath + "/Volume[1]");
            }
            pending.path = objectPath;
            pendingSends.push_back(std::move(pending));
        }
    }

    void translateArrangement(const juce::XmlElement& arrangement)
    {
        if (const auto* tempo =
                arrangement.getChildByName("TempoAutomation"))
        {
            translateTempoAutomation(
                *tempo,
                "/Project/Arrangement/TempoAutomation[1]");
        }
        if (const auto* signature =
                arrangement.getChildByName(
                    "TimeSignatureAutomation"))
        {
            translateMeterAutomation(
                *signature,
                "/Project/Arrangement/TimeSignatureAutomation[1]");
        }
        if (const auto* lanes =
                arrangement.getChildByName("Lanes"))
        {
            translateArrangementLanes(
                *lanes,
                {},
                "beats",
                "/Project/Arrangement/Lanes[1]");
        }
        if (const auto* markers =
                arrangement.getChildByName("Markers"))
        {
            translateMarkers(
                *markers,
                "beats",
                "/Project/Arrangement/Markers[1]");
        }
        for (auto& track : project.tracks)
        {
            if (!track.midiClips.empty()
                && track.type == TrackType::audio)
            {
                const auto instrument = std::any_of(
                    track.inserts.cbegin(),
                    track.inserts.cend(),
                    [](const auto& insert)
                    {
                        return insert.format.containsIgnoreCase(
                            "instrument");
                    });
                track.type = instrument
                    ? TrackType::instrument
                    : TrackType::midi;
            }
        }
    }

    struct RawTempoPoint
    {
        double position = 0.0;
        double bpm = 120.0;
        bool rampToNext = false;
    };

    static double rampDurationSeconds(double beats,
                                      double startBpm,
                                      double endBpm,
                                      bool ramp)
    {
        if (!ramp
            || std::abs(endBpm - startBpm) < 0.000001)
            return beats * 60.0 / startBpm;
        const auto slope = (endBpm - startBpm) / beats;
        if (std::abs(slope) < 0.000001)
            return beats * 60.0 / startBpm;
        return 60.0 * std::log(endBpm / startBpm) / slope;
    }

    void translateTempoAutomation(const juce::XmlElement& points,
                                   const juce::String& path)
    {
        const auto* target = points.getChildByName("Target");
        const auto targetBinding = target != nullptr
            ? parameterBindings.find(
                  target->getStringAttribute("parameter"))
            : parameterBindings.cend();
        if (target == nullptr
            || !target->hasAttribute("parameter")
            || targetBinding == parameterBindings.cend()
            || targetBinding->second.target.parameterId
                   != "transport-tempo")
        {
            fail(
                "semantic.tempo-target",
                path,
                "Tempo automation must target the Transport Tempo parameter.");
            return;
        }
        const auto timeUnit =
            points.getStringAttribute("timeUnit", "beats");
        std::vector<RawTempoPoint> raw;
        for (const auto* child : xmlChildren(points))
        {
            if (!child->hasTagName("RealPoint"))
                continue;
            raw.push_back({
                child->getDoubleAttribute("time"),
                child->getDoubleAttribute("value", project.tempo),
                child->getStringAttribute(
                    "interpolation",
                    "hold") == "linear"
            });
        }
        std::stable_sort(
            raw.begin(),
            raw.end(),
            [](const auto& left, const auto& right)
            {
                return left.position < right.position;
            });
        project.tempoChanges.clear();
        if (timeUnit == "seconds")
        {
            for (const auto& point : raw)
            {
                project.tempoChanges.push_back({
                    point.position,
                    point.bpm,
                    point.rampToNext
                });
            }
        }
        else
        {
            auto previousBeat = 0.0;
            auto previousSeconds = 0.0;
            auto previousBpm = project.tempo;
            auto previousRamp = false;
            for (const auto& point : raw)
            {
                const auto deltaBeats =
                    std::max(0.0, point.position - previousBeat);
                previousSeconds += rampDurationSeconds(
                    deltaBeats,
                    previousBpm,
                    point.bpm,
                    previousRamp);
                project.tempoChanges.push_back({
                    previousSeconds,
                    point.bpm,
                    point.rampToNext
                });
                previousBeat = point.position;
                previousBpm = point.bpm;
                previousRamp = point.rampToNext;
            }
        }
        if (!project.tempoChanges.empty()
            && std::abs(project.tempoChanges.front().timeSeconds)
                   < 0.000001)
        {
            project.tempo = project.tempoChanges.front().bpm;
        }
        if (std::any_of(
                project.tempoChanges.cbegin(),
                project.tempoChanges.cend(),
                [](const auto& change)
                {
                    return change.timeSeconds < 0.0
                        || change.bpm < 20.0
                        || change.bpm > 400.0;
                }))
        {
            fail(
                "import.tempo",
                path,
                "Tempo automation contains unsupported values.");
        }
    }

    void translateMeterAutomation(const juce::XmlElement& points,
                                   const juce::String& path)
    {
        const auto* target = points.getChildByName("Target");
        const auto targetBinding = target != nullptr
            ? parameterBindings.find(
                  target->getStringAttribute("parameter"))
            : parameterBindings.cend();
        if (target == nullptr
            || !target->hasAttribute("parameter")
            || targetBinding == parameterBindings.cend()
            || targetBinding->second.target.parameterId
                   != "transport-time-signature")
        {
            fail(
                "semantic.time-signature-target",
                path,
                "Time-signature automation must target the Transport TimeSignature parameter.");
            return;
        }
        const auto timeUnit =
            points.getStringAttribute("timeUnit", "beats");
        project.meterChanges.clear();
        for (const auto* child : xmlChildren(points))
        {
            if (!child->hasTagName("TimeSignaturePoint"))
                continue;
            const auto position =
                child->getDoubleAttribute("time");
            project.meterChanges.push_back({
                timeUnit == "seconds"
                    ? position
                    : project.secondsAtBeat(position),
                child->getIntAttribute("numerator", 4),
                child->getIntAttribute("denominator", 4)
            });
        }
        std::stable_sort(
            project.meterChanges.begin(),
            project.meterChanges.end(),
            [](const auto& left, const auto& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
        if (!project.meterChanges.empty()
            && std::abs(project.meterChanges.front().timeSeconds)
                   < 0.000001)
        {
            project.timeSignatureNumerator =
                project.meterChanges.front().numerator;
            project.timeSignatureDenominator =
                project.meterChanges.front().denominator;
        }
        juce::String validationError;
        if (!project.validateTransport(validationError))
            fail("import.meter", path, validationError);
    }

    void translateMarkers(const juce::XmlElement& markers,
                          const juce::String& inheritedTimeUnit,
                          const juce::String& path)
    {
        const auto timeUnit =
            markers.getStringAttribute(
                "timeUnit",
                inheritedTimeUnit.isNotEmpty()
                    ? inheritedTimeUnit
                    : juce::String("beats"));
        auto index = 0;
        for (const auto* child : xmlChildren(markers))
        {
            if (!child->hasTagName("Marker"))
                continue;
            ++index;
            const auto position =
                child->getDoubleAttribute("time");
            SongSection section;
            juce::String mappingError;
            section.id = ids.importedId(
                "marker",
                path + "/" + juce::String(index),
                mappingError);
            section.name =
                child->getStringAttribute("name", "Marker");
            section.timeSeconds = timeUnit == "seconds"
                ? position
                : project.secondsAtBeat(position);
            if (std::any_of(
                    project.sections.cbegin(),
                    project.sections.cend(),
                    [&section](const auto& existing)
                    {
                        return std::abs(
                                   existing.timeSeconds
                                   - section.timeSeconds)
                            < 0.0001;
                    }))
            {
                warn(
                    "unsupported.duplicate-marker",
                    path + "/Marker[" + juce::String(index) + "]",
                    "Studio Duo keeps the first marker at a timeline position.");
                continue;
            }
            project.sections.push_back(std::move(section));
        }
        std::stable_sort(
            project.sections.begin(),
            project.sections.end(),
            [](const auto& left, const auto& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
    }

    juce::String resolvedTrackId(
        const juce::String& externalTrackId,
        const juce::String& path)
    {
        if (externalTrackId.isEmpty())
            return {};
        const auto internal =
            ids.internalId("track", externalTrackId);
        if (internal.isEmpty())
        {
            warn(
                "unsupported.timeline-track",
                path,
                "The timeline references an unavailable track ID '"
                    + externalTrackId + "'.");
        }
        return internal;
    }

    void translateArrangementLanes(
        const juce::XmlElement& lanes,
        juce::String currentTrackId,
        const juce::String& inheritedTimeUnit,
        const juce::String& path)
    {
        const auto timeUnit = lanes.getStringAttribute(
            "timeUnit",
            inheritedTimeUnit.isNotEmpty()
                ? inheritedTimeUnit
                : juce::String("beats"));
        if (lanes.hasAttribute("track"))
        {
            currentTrackId = resolvedTrackId(
                lanes.getStringAttribute("track"),
                path + "/@track");
        }
        const auto children = xmlChildren(lanes);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            const auto* child = children[index];
            const auto childPathValue =
                xmlChildPath(path, children, index);
            if (child->hasTagName("Lanes"))
            {
                translateArrangementLanes(
                    *child,
                    currentTrackId,
                    timeUnit,
                    childPathValue);
            }
            else if (child->hasTagName("Clips"))
            {
                translateClips(
                    *child,
                    currentTrackId,
                    timeUnit,
                    childPathValue,
                    nullptr);
            }
            else if (child->hasTagName("Notes"))
            {
                translateLooseNotes(
                    *child,
                    currentTrackId,
                    timeUnit,
                    childPathValue);
            }
            else if (child->hasTagName("Points"))
            {
                translateAutomation(
                    *child,
                    currentTrackId,
                    timeUnit,
                    childPathValue);
            }
            else if (child->hasTagName("markers"))
            {
                translateMarkers(*child, timeUnit, childPathValue);
            }
            else if (child->hasTagName("Video"))
            {
                warn(
                    "unsupported.video",
                    childPathValue,
                    "Video timelines are not supported by Studio Duo.");
            }
            else if (child->hasTagName("ClipSlot"))
            {
                warn(
                    "unsupported.arrangement-clip-slot",
                    childPathValue,
                    "Arrangement clip slots have no Studio Duo arrangement equivalent.");
            }
            else if (child->hasTagName("Audio")
                     || child->hasTagName("Warps"))
            {
                translateLooseAudio(
                    *child,
                    currentTrackId,
                    timeUnit,
                    childPathValue);
            }
            else
            {
                warn(
                    "unsupported.timeline",
                    childPathValue,
                    "The DAWproject timeline construct is not supported.");
            }
        }
    }

    void translateClips(const juce::XmlElement& clips,
                        const juce::String& trackIdValue,
                        const juce::String& inheritedTimeUnit,
                        const juce::String& path,
                        ProjectScene* scene)
    {
        const auto effectiveTrackId = clips.hasAttribute("track")
            ? resolvedTrackId(
                  clips.getStringAttribute("track"),
                  path + "/@track")
            : trackIdValue;
        if (effectiveTrackId.isEmpty())
        {
            warn(
                "unsupported.clip-track",
                path,
                "Clips without a resolvable track cannot be imported.");
            return;
        }
        const auto timeUnit =
            clips.getStringAttribute(
                "timeUnit",
                inheritedTimeUnit.isNotEmpty()
                    ? inheritedTimeUnit
                    : juce::String("beats"));
        const auto children = xmlChildren(clips);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            if (!children[index]->hasTagName("Clip"))
                continue;
            translateClip(
                *children[index],
                effectiveTrackId,
                timeUnit,
                xmlChildPath(path, children, index),
                scene,
                false);
        }
    }

    const juce::XmlElement* findTimeline(
        const juce::XmlElement& parent,
        const juce::String& tag) const
    {
        for (const auto* child : xmlChildren(parent))
        {
            if (child->hasTagName(tag))
                return child;
            if (child->hasTagName("Lanes")
                || child->hasTagName("Warps")
                || child->hasTagName("Clips")
                || child->hasTagName("Clip")
                || child->hasTagName("ClipSlot"))
            {
                if (const auto* nested = findTimeline(*child, tag))
                    return nested;
            }
        }
        return nullptr;
    }

    int countTimelines(const juce::XmlElement& parent,
                       const juce::String& tag) const
    {
        auto count = 0;
        for (const auto* child : xmlChildren(parent))
        {
            if (child->hasTagName(tag))
                ++count;
            if (child->hasTagName("Lanes")
                || child->hasTagName("Warps")
                || child->hasTagName("Clips")
                || child->hasTagName("Clip")
                || child->hasTagName("ClipSlot"))
            {
                count += countTimelines(*child, tag);
            }
        }
        return count;
    }

    bool hasUnsupportedNestedTimeline(
        const juce::XmlElement& clip) const
    {
        return findTimeline(clip, "Video") != nullptr
            || findTimeline(clip, "Clips") != nullptr
            || findTimeline(clip, "ClipSlot") != nullptr
            || findTimeline(clip, "markers") != nullptr;
    }

    void translateClip(const juce::XmlElement& clip,
                       const juce::String& trackIdValue,
                       const juce::String& parentTimeUnit,
                       const juce::String& path,
                       ProjectScene* scene,
                       bool stopTrack)
    {
        if (clip.getStringAttribute("comment").isNotEmpty())
        {
            warn(
                "unsupported.clip-comment",
                path,
                "Clip comments are not stored by the current Studio Duo project model.");
        }
        if (clip.hasAttribute("loopStart")
            || clip.hasAttribute("loopEnd"))
        {
            warn(
                "unsupported.clip-loop",
                path,
                "Clip loop bounds are not represented by the current Studio Duo clip model.");
        }
        if (clip.hasAttribute("reference"))
        {
            warn(
                "unsupported.clip-reference",
                path,
                "Shared timeline references are imported only when inline content is also available.");
            if (xmlChildren(clip).empty())
                return;
        }
        if (hasUnsupportedNestedTimeline(clip))
        {
            warn(
                "unsupported.clip-content",
                path,
                "Nested video, clip, marker, or clip-slot content is not represented in Studio Duo.");
        }
        const auto* notes = findTimeline(clip, "Notes");
        const auto* audio = findTimeline(clip, "Audio");
        const auto* warps = findTimeline(clip, "Warps");
        const auto noteTimelines = countTimelines(clip, "Notes");
        const auto audioTimelines = countTimelines(clip, "Audio");
        if (noteTimelines + audioTimelines > 1)
        {
            warn(
                "unsupported.clip-layering",
                path,
                "Studio Duo imported the first supported audio or note timeline from a layered clip.");
        }
        if (notes != nullptr)
        {
            if (clip.hasAttribute("color"))
            {
                warn(
                    "unsupported.midi-clip-color",
                    path,
                    "MIDI clip color is not stored by the current Studio Duo MIDI clip model.");
            }
            auto midi = parseMidiClip(
                clip,
                *notes,
                parentTimeUnit,
                path);
            if (!midi.has_value())
                return;
            if (scene != nullptr)
            {
                if (std::any_of(
                        scene->slots.cbegin(),
                        scene->slots.cend(),
                        [&trackIdValue](const auto& slot)
                        {
                            return slot.trackId == trackIdValue;
                        }))
                {
                    warn(
                        "unsupported.duplicate-scene-slot",
                        path,
                        "Studio Duo keeps the first scene slot for each track.");
                    return;
                }
                SceneSlot slot;
                juce::String mappingError;
                slot.id = ids.importedId(
                    "scene-slot",
                    path,
                    mappingError);
                slot.trackId = trackIdValue;
                slot.stopTrack = stopTrack;
                slot.midiClip = std::move(*midi);
                scene->slots.push_back(std::move(slot));
            }
            else if (auto* track = project.findTrack(trackIdValue))
            {
                track->midiClips.push_back(std::move(*midi));
            }
            return;
        }
        if (audio != nullptr)
        {
            auto imported = parseAudioClip(
                clip,
                *audio,
                warps,
                parentTimeUnit,
                path);
            if (!imported.has_value())
                return;
            if (scene != nullptr)
            {
                if (std::any_of(
                        scene->slots.cbegin(),
                        scene->slots.cend(),
                        [&trackIdValue](const auto& slot)
                        {
                            return slot.trackId == trackIdValue;
                        }))
                {
                    warn(
                        "unsupported.duplicate-scene-slot",
                        path,
                        "Studio Duo keeps the first scene slot for each track.");
                    return;
                }
                SceneSlot slot;
                juce::String mappingError;
                slot.id = ids.importedId(
                    "scene-slot",
                    path,
                    mappingError);
                slot.trackId = trackIdValue;
                slot.stopTrack = stopTrack;
                slot.audioClip = std::move(*imported);
                scene->slots.push_back(std::move(slot));
            }
            else if (auto* track = project.findTrack(trackIdValue))
            {
                track->clips.push_back(std::move(*imported));
            }
            return;
        }
        warn(
            "unsupported.clip-empty",
            path,
            "The clip does not contain supported audio or note content.");
    }

    double timelineSeconds(double position,
                           const juce::String& timeUnit) const
    {
        return timeUnit == "seconds"
            ? position
            : project.secondsAtBeat(position);
    }

    double timelineBeats(double position,
                         const juce::String& timeUnit) const
    {
        return timeUnit == "beats"
            ? position
            : project.beatsAt(position);
    }

    std::optional<MidiClip> parseMidiClip(
        const juce::XmlElement& clip,
        const juce::XmlElement& notes,
        const juce::String& parentTimeUnit,
        const juce::String& path)
    {
        MidiClip result;
        juce::String mappingError;
        result.id = ids.importedId("midi-clip", path, mappingError);
        result.name =
            clip.getStringAttribute("name", "MIDI clip");
        const auto clipStart =
            clip.getDoubleAttribute("time");
        const auto clipDuration =
            clip.getDoubleAttribute("duration", 0.0);
        const auto clipStartSeconds =
            timelineSeconds(clipStart, parentTimeUnit);
        result.startBeats =
            timelineBeats(clipStart, parentTimeUnit);
        if (parentTimeUnit == "beats")
            result.durationBeats = clipDuration;
        else
            result.durationBeats =
                project.beatsAt(clipStart + clipDuration)
                - project.beatsAt(clipStart);
        const auto noteTimeUnit =
            notes.getStringAttribute(
                "timeUnit",
                clip.getStringAttribute(
                    "contentTimeUnit",
                    parentTimeUnit.isNotEmpty()
                        ? parentTimeUnit
                        : juce::String("beats")));
        const auto contentTimeUnit =
            clip.getStringAttribute(
                "contentTimeUnit",
                noteTimeUnit);
        const auto playStart =
            clip.getDoubleAttribute("playStart", 0.0);
        const auto playStartBeats = contentTimeUnit == "beats"
            ? playStart
            : project.beatsAt(clipStartSeconds + playStart)
                - result.startBeats;
        const auto playStartSeconds = contentTimeUnit == "seconds"
            ? playStart
            : project.secondsAtBeat(result.startBeats + playStart)
                - clipStartSeconds;
        auto noteIndex = 0;
        for (const auto* note : xmlChildren(notes))
        {
            if (!note->hasTagName("Note"))
                continue;
            ++noteIndex;
            MidiNote imported;
            imported.id = ids.importedId(
                "note",
                path + "/Note[" + juce::String(noteIndex) + "]",
                mappingError);
            const auto noteTime =
                note->getDoubleAttribute("time");
            const auto noteDuration =
                note->getDoubleAttribute("duration");
            double absoluteNoteSeconds = 0.0;
            if (noteTimeUnit == "beats")
            {
                imported.startBeats =
                    noteTime - playStartBeats;
                imported.durationBeats = noteDuration;
                absoluteNoteSeconds = project.secondsAtBeat(
                    result.startBeats + imported.startBeats);
            }
            else
            {
                absoluteNoteSeconds =
                    clipStartSeconds + noteTime - playStartSeconds;
                imported.startBeats =
                    project.beatsAt(absoluteNoteSeconds)
                    - result.startBeats;
                imported.durationBeats =
                    project.beatsAt(
                        absoluteNoteSeconds + noteDuration)
                    - project.beatsAt(absoluteNoteSeconds);
            }
            imported.pitch =
                note->getIntAttribute("key", 60);
            imported.channel = juce::jlimit(
                1,
                16,
                note->getIntAttribute("channel", 0) + 1);
            imported.velocity = juce::jlimit(
                1,
                127,
                static_cast<int>(std::round(
                    note->getDoubleAttribute("vel", 1.0) * 127.0)));
            imported.releaseVelocity = juce::jlimit(
                0,
                127,
                static_cast<int>(std::round(
                    note->getDoubleAttribute("rel", 0.5) * 127.0)));
            const auto leadingTrimBeats =
                std::max(0.0, -imported.startBeats);
            translateNoteExpressions(
                *note,
                imported,
                noteTimeUnit,
                absoluteNoteSeconds,
                path + "/Note[" + juce::String(noteIndex) + "]");
            if (imported.endBeats() <= 0.0
                || (result.durationBeats > 0.0
                    && imported.startBeats
                           >= result.durationBeats - 0.0000001))
            {
                warn(
                    "unsupported.note-outside-play-range",
                    path + "/Note[" + juce::String(noteIndex) + "]",
                    "The note falls outside the clip play range and was not imported.");
                continue;
            }
            if (imported.startBeats < 0.0)
            {
                imported.durationBeats -= leadingTrimBeats;
                imported.startBeats = 0.0;
            }
            if (result.durationBeats > 0.0
                && imported.endBeats() > result.durationBeats)
                imported.durationBeats =
                    result.durationBeats - imported.startBeats;
            if (imported.durationBeats <= 0.0000001)
            {
                warn(
                    "unsupported.note-outside-play-range",
                    path + "/Note[" + juce::String(noteIndex) + "]",
                    "The note falls outside the clip play range and was not imported.");
                continue;
            }
            imported.expressions.erase(
                std::remove_if(
                    imported.expressions.begin(),
                    imported.expressions.end(),
                    [leadingTrimBeats,
                     finalDuration = imported.durationBeats](
                        auto& expression)
                    {
                        const auto adjusted =
                            expression.offsetBeats
                            - leadingTrimBeats;
                        if (adjusted < -0.0000001
                            || adjusted
                                > finalDuration + 0.0000001)
                        {
                            return true;
                        }
                        expression.offsetBeats = juce::jlimit(
                            0.0,
                            finalDuration,
                            adjusted);
                        return false;
                    }),
                imported.expressions.end());
            result.notes.push_back(std::move(imported));
        }
        if (result.durationBeats <= 0.0)
        {
            result.durationBeats = 0.25;
            for (const auto& note : result.notes)
                result.durationBeats =
                    std::max(result.durationBeats, note.endBeats());
        }
        return result;
    }

    void translateNoteExpressions(const juce::XmlElement& noteElement,
                                  MidiNote& note,
                                  const juce::String& inheritedTimeUnit,
                                  double absoluteNoteSeconds,
                                  const juce::String& path)
    {
        struct ExpressionTimeline
        {
            const juce::XmlElement* points = nullptr;
            juce::String timeUnit;
            juce::String path;
        };
        std::vector<ExpressionTimeline> pointTimelines;
        const auto collect =
            [&pointTimelines](
                const auto& self,
                const juce::XmlElement& parent,
                const juce::String& parentTimeUnit,
                const juce::String& parentPath) -> void
        {
            const auto children = xmlChildren(parent);
            for (std::size_t index = 0; index < children.size(); ++index)
            {
                const auto* child = children[index];
                const auto childPath =
                    xmlChildPath(parentPath, children, index);
                const auto childTimeUnit =
                    child->getStringAttribute(
                        "timeUnit",
                        parentTimeUnit.isNotEmpty()
                            ? parentTimeUnit
                            : juce::String("beats"));
                if (child->hasTagName("Points"))
                {
                    pointTimelines.push_back({
                        child,
                        childTimeUnit,
                        childPath
                    });
                }
                else if (isTimelineContent(*child))
                {
                    self(
                        self,
                        *child,
                        childTimeUnit,
                        childPath);
                }
            }
        };
        collect(
            collect,
            noteElement,
            inheritedTimeUnit,
            path);
        auto expressionIndex = 0;
        for (const auto& timeline : pointTimelines)
        {
            const auto& points = *timeline.points;
            const auto* target = points.getChildByName("Target");
            if (target == nullptr
                || !target->hasAttribute("expression"))
                continue;
            const auto expression =
                target->getStringAttribute("expression");
            MidiExpressionType type;
            if (expression == "pitchBend")
                type = MidiExpressionType::pitchBend;
            else if (expression == "pressure"
                     || expression == "polyPressure")
                type = MidiExpressionType::pressure;
            else if (expression == "channelPressure")
                type = MidiExpressionType::channelPressure;
            else if (expression == "timbre")
                type = MidiExpressionType::timbre;
            else if (expression == "channelController")
                type = MidiExpressionType::controller;
            else
            {
                warn(
                    "unsupported.note-expression",
                    timeline.path,
                    "Note expression '" + expression
                        + "' has no Studio Duo note-expression equivalent.");
                continue;
            }
            const auto unit =
                points.getStringAttribute("unit", "normalized");
            const auto supportedUnit =
                unit == "normalized"
                || unit == "linear"
                || unit == "percent";
            if (!supportedUnit)
            {
                warn(
                    "unsupported.note-expression-unit",
                    timeline.path,
                    "Note expression unit '" + unit
                        + "' has no Studio Duo conversion.");
                continue;
            }
            const auto children = xmlChildren(points);
            for (std::size_t index = 0; index < children.size(); ++index)
            {
                const auto* point = children[index];
                if (!point->hasTagName("RealPoint"))
                    continue;
                ++expressionIndex;
                const auto pointPath =
                    xmlChildPath(timeline.path, children, index);
                MidiExpressionPoint imported;
                juce::String mappingError;
                imported.id = ids.importedId(
                    "note-expression",
                    pointPath + "/Expression["
                        + juce::String(expressionIndex) + "]",
                    mappingError);
                imported.type = type;
                const auto sourceTime =
                    point->getDoubleAttribute("time");
                imported.offsetBeats =
                    timeline.timeUnit == "seconds"
                    ? project.beatsAt(
                          absoluteNoteSeconds + sourceTime)
                        - project.beatsAt(absoluteNoteSeconds)
                    : sourceTime;
                const auto sourceValue =
                    point->getDoubleAttribute("value");
                imported.value = unit == "percent"
                    ? sourceValue / 100.0
                    : sourceValue;
                const auto minimum =
                    type == MidiExpressionType::pitchBend
                    ? -1.0
                    : 0.0;
                if (imported.offsetBeats < 0.0
                    || imported.value < minimum
                    || imported.value > 1.0)
                {
                    fail(
                        "import.note-expression-value",
                        pointPath,
                        "Note expression time or value is outside Studio Duo's supported domain.");
                    continue;
                }
                imported.controller = type
                        == MidiExpressionType::controller
                    ? target->getIntAttribute("controller", -1)
                    : -1;
                note.expressions.push_back(std::move(imported));
            }
        }
    }

    std::optional<AudioClip> parseAudioClip(
        const juce::XmlElement& clip,
        const juce::XmlElement& audio,
        const juce::XmlElement* warps,
        const juce::String& parentTimeUnit,
        const juce::String& path)
    {
        const auto* file = audio.getChildByName("File");
        if (file == nullptr)
        {
            fail(
                "import.media-reference",
                path,
                "Audio content does not contain a File reference.");
            return std::nullopt;
        }
        const auto importedFile =
            materializeMedia(*file, path + "/Audio/File[1]");
        if (!importedFile.has_value())
            return std::nullopt;
        const auto relativeMedia =
            importedFile->getRelativePathFrom(destinationPackage);
        const auto stagedMedia =
            stagingPackage.getChildFile(relativeMedia);
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(stagedMedia));
        if (reader == nullptr
            || reader->sampleRate <= 0.0
            || reader->numChannels == 0)
        {
            fail(
                "import.media-invalid",
                path,
                "The referenced audio payload is not a supported audio file.");
            return std::nullopt;
        }
        const auto actualDuration =
            static_cast<double>(reader->lengthInSamples)
            / reader->sampleRate;
        const auto declaredDuration =
            audio.getDoubleAttribute("duration", actualDuration);
        if (std::abs(actualDuration - declaredDuration)
                > std::max(0.001, 1.0 / reader->sampleRate)
            || static_cast<int>(reader->numChannels)
                   != audio.getIntAttribute("channels")
            || static_cast<int>(std::round(reader->sampleRate))
                   != audio.getIntAttribute("sampleRate"))
        {
            warn(
                "compatibility.media-metadata",
                path,
                "The embedded audio header differs from the DAWproject metadata; Studio Duo uses the decoded file properties.");
        }

        AudioClip result;
        juce::String mappingError;
        result.id = ids.importedId("audio-clip", path, mappingError);
        result.name =
            clip.getStringAttribute("name", "Audio clip");
        result.colour = colourFromText(
            clip.getStringAttribute("color"),
            result.colour);
        const auto start =
            clip.getDoubleAttribute("time");
        const auto duration =
            clip.getDoubleAttribute("duration", 0.0);
        const auto clipStartBeats =
            timelineBeats(start, parentTimeUnit);
        result.startSeconds =
            timelineSeconds(start, parentTimeUnit);
        result.durationSeconds = parentTimeUnit == "seconds"
            ? duration
            : project.secondsAtBeat(start + duration)
                - project.secondsAtBeat(start);
        result.sourceFile = *importedFile;
        result.sourceLengthSeconds = actualDuration;
        result.sourceRangeStartSeconds = 0.0;
        result.sourceRangeEndSeconds = result.sourceLengthSeconds;
        const auto contentTimeUnit =
            clip.getStringAttribute("contentTimeUnit", "beats");
        auto playStart =
            clip.getDoubleAttribute("playStart", 0.0);
        auto playStop =
            clip.getDoubleAttribute(
                "playStop",
                playStart + result.durationSeconds);
        if (contentTimeUnit == "beats")
        {
            playStart = project.secondsAtBeat(playStart)
                - project.secondsAtBeat(0.0);
            playStop = project.secondsAtBeat(playStop)
                - project.secondsAtBeat(0.0);
        }
        result.sourceOffsetSeconds =
            std::max(0.0, playStart);
        if (result.durationSeconds <= 0.0)
            result.durationSeconds =
                std::max(0.001, playStop - playStart);
        if (result.durationSeconds > 0.0
            && playStop > playStart)
        {
            const auto rate =
                (playStop - playStart) / result.durationSeconds;
            if (rate < 0.25 || rate > 4.0)
            {
                warn(
                    "unsupported.clip-play-range",
                    path,
                    "The clip play range requires a playback rate outside Studio Duo's supported range and was clamped.");
            }
            result.playbackRate =
                juce::jlimit(0.25, 4.0, rate);
        }
        result.fadeInSeconds =
            clip.getDoubleAttribute("fadeInTime", 0.0);
        result.fadeOutSeconds =
            clip.getDoubleAttribute("fadeOutTime", 0.0);
        if (result.fadeInSeconds < 0.0
            || result.fadeOutSeconds < 0.0)
        {
            warn(
                "unsupported.clip-crossfade-overlap",
                path,
                "Negative DAWproject fade overlap is imported as a positive Studio Duo fade length.");
            result.fadeInSeconds = std::abs(result.fadeInSeconds);
            result.fadeOutSeconds = std::abs(result.fadeOutSeconds);
        }
        if (clip.getStringAttribute("fadeTimeUnit", parentTimeUnit)
            == "beats")
        {
            result.fadeInSeconds =
                project.secondsAtBeat(
                    clipStartBeats + result.fadeInSeconds)
                - result.startSeconds;
            result.fadeOutSeconds =
                project.secondsAtBeat(
                    clipStartBeats + result.fadeOutSeconds)
                - result.startSeconds;
        }
        const auto stretch =
            stretchModeForAlgorithm(
                audio.getStringAttribute("algorithm"));
        if (stretch.has_value())
            result.stretchMode = *stretch;
        else
            warn(
                "unsupported.warp-algorithm",
                path,
                "The audio warp algorithm is preserved as a generic polyphonic stretch mode.");
        if (warps != nullptr)
        {
            const auto warpTimeUnit =
                warps->getStringAttribute(
                    "timeUnit",
                    contentTimeUnit);
            const auto warpContentTimeUnit =
                warps->getStringAttribute(
                    "contentTimeUnit",
                    "seconds");
            for (const auto* warp : xmlChildren(*warps))
            {
                if (!warp->hasTagName("Warp"))
                    continue;
                const auto timelinePosition =
                    warp->getDoubleAttribute("time");
                const auto contentPosition =
                    warp->getDoubleAttribute("contentTime");
                result.warpMarkers.push_back({
                    warpTimeUnit == "seconds"
                        ? timelinePosition
                        : project.secondsAtBeat(
                              clipStartBeats + timelinePosition)
                            - result.startSeconds,
                    warpContentTimeUnit == "seconds"
                        ? contentPosition
                        : project.secondsAtBeat(contentPosition)
                            - project.secondsAtBeat(0.0)
                });
            }
            if (result.warpMarkers.size() >= 2)
            {
                const auto& first = result.warpMarkers.front();
                const auto& last = result.warpMarkers.back();
                if (last.timelineOffsetSeconds
                    > first.timelineOffsetSeconds)
                {
                    result.playbackRate =
                        (last.sourceSeconds - first.sourceSeconds)
                        / (last.timelineOffsetSeconds
                           - first.timelineOffsetSeconds);
                    result.playbackRate = juce::jlimit(
                        0.25,
                        4.0,
                        result.playbackRate);
                }
            }
        }
        result.gainDecibels =
            static_cast<float>(clipGain(clip, path));
        return result;
    }

    double clipGain(const juce::XmlElement& clip,
                    const juce::String& path)
    {
        std::vector<const juce::XmlElement*> points;
        const auto collect =
            [&points](
                const auto& self,
                const juce::XmlElement& parent) -> void
        {
            for (const auto* child : xmlChildren(parent))
            {
                if (child->hasTagName("Points"))
                    points.push_back(child);
                else if (child->hasTagName("Lanes"))
                    self(self, *child);
            }
        };
        collect(collect, clip);
        for (const auto* lane : points)
        {
            const auto* target = lane->getChildByName("Target");
            if (target == nullptr
                || target->getStringAttribute("expression") != "gain")
                continue;
            std::vector<double> values;
            for (const auto* point : xmlChildren(*lane))
            {
                if (point->hasTagName("RealPoint"))
                {
                    values.push_back(convertDawValue(
                        point->getDoubleAttribute("value"),
                        ParameterDomain::gainDecibels,
                        lane->getStringAttribute("unit", "linear"),
                        0.0,
                        1.0));
                }
            }
            if (values.size() > 1
                && std::any_of(
                    values.cbegin() + 1,
                    values.cend(),
                    [&values](double value)
                    {
                        return std::abs(value - values.front())
                            > 0.000001;
                    }))
            {
                warn(
                    "unsupported.clip-gain-automation",
                    path,
                    "Time-varying clip gain is imported at its first value because Studio Duo stores one clip gain.");
            }
            if (!values.empty())
                return values.front();
        }
        return 0.0;
    }

    std::optional<juce::File> materializeMedia(
        const juce::XmlElement& file,
        const juce::String& path)
    {
        const auto reference =
            file.getStringAttribute("path");
        const auto external =
            boolAttribute(file, "external", false);
        const auto mediaKey =
            (external ? "external:" : "embedded:") + reference;
        if (const auto existing = importedMedia.find(mediaKey);
            existing != importedMedia.cend())
        {
            return destinationPackage.getChildFile(existing->second);
        }
        auto extension =
            juce::File(reference).getFileExtension().toLowerCase();
        if (extension.isEmpty()
            || extension.length() > 12
            || extension.containsAnyOf("/\\:"))
            extension = ".bin";
        const auto relative =
            "media/" + textHash(mediaKey).substring(0, 40)
            + extension;
        const auto staged = stagingPackage.getChildFile(relative);
        juce::String error;
        if (external)
        {
            auto source = juce::File(reference);
            if (!juce::File::isAbsolutePath(reference))
                source = sourceArchive.getParentDirectory()
                             .getChildFile(reference);
            if (!source.existsAsFile()
                || !staged.getParentDirectory().createDirectory()
                || !source.copyFileTo(staged))
            {
                fail(
                    "import.external-media",
                    path,
                    "Could not copy external media reference '"
                        + reference + "'.");
                return std::nullopt;
            }
        }
        else if (!archive.copyTo(reference, staged, error))
        {
            fail(
                "import.embedded-media",
                path,
                "Could not extract embedded media: " + error);
            return std::nullopt;
        }
        importedMedia.emplace(mediaKey, relative);
        return destinationPackage.getChildFile(relative);
    }

    void translateLooseNotes(const juce::XmlElement& notes,
                             const juce::String& trackIdValue,
                             const juce::String& inheritedTimeUnit,
                             const juce::String& path)
    {
        const auto effectiveTrackId = notes.hasAttribute("track")
            ? resolvedTrackId(
                  notes.getStringAttribute("track"),
                  path + "/@track")
            : trackIdValue;
        if (effectiveTrackId.isEmpty())
        {
            warn(
                "unsupported.notes-track",
                path,
                "A note timeline without a resolvable track was ignored.");
            return;
        }
        juce::XmlElement syntheticClip("Clip");
        syntheticClip.setAttribute("time", "0");
        syntheticClip.setAttribute("duration", "0");
        syntheticClip.setAttribute("name", "Imported notes");
        syntheticClip.addChildElement(
            new juce::XmlElement(notes));
        translateClip(
            syntheticClip,
            effectiveTrackId,
            notes.getStringAttribute(
                "timeUnit",
                inheritedTimeUnit.isNotEmpty()
                    ? inheritedTimeUnit
                    : juce::String("beats")),
            path,
            nullptr,
            false);
    }

    void translateLooseAudio(const juce::XmlElement& content,
                             const juce::String& trackIdValue,
                             const juce::String& inheritedTimeUnit,
                             const juce::String& path)
    {
        const auto effectiveTrackId = content.hasAttribute("track")
            ? resolvedTrackId(
                  content.getStringAttribute("track"),
                  path + "/@track")
            : trackIdValue;
        if (effectiveTrackId.isEmpty())
        {
            warn(
                "unsupported.audio-track",
                path,
                "An audio timeline without a resolvable track was ignored.");
            return;
        }
        juce::XmlElement syntheticClip("Clip");
        syntheticClip.setAttribute("time", "0");
        syntheticClip.setAttribute(
            "duration",
            content.getDoubleAttribute("duration", 0.001));
        syntheticClip.setAttribute("contentTimeUnit", "seconds");
        syntheticClip.setAttribute("name", "Imported audio");
        syntheticClip.addChildElement(
            new juce::XmlElement(content));
        translateClip(
            syntheticClip,
            effectiveTrackId,
            content.getStringAttribute(
                "timeUnit",
                inheritedTimeUnit.isNotEmpty()
                    ? inheritedTimeUnit
                    : juce::String("beats")),
            path,
            nullptr,
            false);
    }

    void translateAutomation(
        const juce::XmlElement& points,
        juce::String trackIdValue,
        const juce::String& inheritedTimeUnit,
        const juce::String& path)
    {
        if (points.hasAttribute("track"))
        {
            trackIdValue = resolvedTrackId(
                points.getStringAttribute("track"),
                path + "/@track");
        }
        const auto* target = points.getChildByName("Target");
        if (target == nullptr)
            return;
        if (target->hasAttribute("expression"))
        {
            const auto expression =
                target->getStringAttribute("expression");
            if (expression != "channelPressure")
            {
                warn(
                    "unsupported.track-expression",
                    path,
                    "Track-level expression '" + expression
                        + "' has no Studio Duo automation target.");
                return;
            }
            if (trackIdValue.isEmpty())
            {
                fail(
                    "import.channel-pressure-track",
                    path,
                    "Channel-pressure automation requires a valid track.");
                return;
            }
            const auto externalChannel =
                target->getIntAttribute("channel", -1);
            if (externalChannel < 0 || externalChannel > 15)
            {
                fail(
                    "import.channel-pressure-channel",
                    path + "/Target[1]/@channel",
                    "Channel-pressure automation requires a zero-based MIDI channel from 0 through 15.");
                return;
            }
            const auto pointUnit =
                points.getStringAttribute("unit", "normalized");
            if (pointUnit != "normalized"
                && pointUnit != "linear"
                && pointUnit != "percent")
            {
                warn(
                    "unsupported.track-expression-unit",
                    path,
                    "Channel-pressure unit '" + pointUnit
                        + "' has no Studio Duo conversion.");
                return;
            }
            AutomationLane lane;
            juce::String mappingError;
            lane.id = ids.importedId(
                "automation",
                externalOrPathId(points, path),
                mappingError);
            lane.name = points.getStringAttribute(
                "name",
                "Channel pressure");
            lane.target.type =
                AutomationTargetType::midiChannelPressure;
            lane.target.trackId = trackIdValue;
            lane.target.midiChannel = externalChannel + 1;
            lane.timebase =
                points.getStringAttribute(
                    "timeUnit",
                    inheritedTimeUnit.isNotEmpty()
                        ? inheritedTimeUnit
                        : juce::String("beats"))
                        == "seconds"
                ? AutomationTimebase::seconds
                : AutomationTimebase::beats;
            auto pointIndex = 0;
            std::optional<AutomationInterpolation>
                importedInterpolation;
            const auto children = xmlChildren(points);
            for (std::size_t childIndex = 0;
                 childIndex < children.size();
                 ++childIndex)
            {
                const auto* point = children[childIndex];
                if (point->hasTagName("Target"))
                    continue;
                const auto pointPath =
                    xmlChildPath(path, children, childIndex);
                if (!point->hasTagName("RealPoint"))
                {
                    warn(
                        "unsupported.automation-point-type",
                        pointPath,
                        "Channel-pressure automation supports RealPoint values.");
                    continue;
                }
                ++pointIndex;
                AutomationPoint imported;
                imported.id = ids.importedId(
                    "automation-point",
                    path + "/Point["
                        + juce::String(pointIndex) + "]",
                    mappingError);
                imported.position =
                    point->getDoubleAttribute("time");
                const auto sourceValue =
                    point->getDoubleAttribute("value");
                imported.value = pointUnit == "percent"
                    ? sourceValue / 100.0
                    : sourceValue;
                if (!std::isfinite(imported.position)
                    || imported.position < 0.0
                    || !std::isfinite(imported.value)
                    || imported.value < 0.0
                    || imported.value > 1.0)
                {
                    fail(
                        "import.channel-pressure-value",
                        pointPath,
                        "Channel-pressure time or value is outside Studio Duo's supported domain.");
                    continue;
                }
                const auto pointInterpolation =
                    point->getStringAttribute(
                        "interpolation",
                        "hold") == "linear"
                    ? AutomationInterpolation::linear
                    : AutomationInterpolation::step;
                if (importedInterpolation.has_value()
                    && *importedInterpolation
                        != pointInterpolation)
                {
                    warn(
                        "unsupported.automation-interpolation",
                        path,
                        "Mixed per-point interpolation is imported using the first point's lane interpolation.");
                }
                else if (!importedInterpolation.has_value())
                {
                    importedInterpolation =
                        pointInterpolation;
                    lane.interpolation =
                        pointInterpolation;
                }
                lane.points.push_back(std::move(imported));
            }
            project.automationLanes.push_back(std::move(lane));
            return;
        }
        const auto parameter =
            target->getStringAttribute("parameter");
        const auto binding = parameterBindings.find(parameter);
        if (binding == parameterBindings.cend())
        {
            warn(
                "unsupported.automation-parameter",
                path,
                "Automation targets unknown parameter ID '"
                    + parameter + "'.");
            return;
        }
        AutomationLane lane;
        juce::String mappingError;
        lane.id = ids.importedId(
            "automation",
            externalOrPathId(points, path),
            mappingError);
        lane.name = points.getStringAttribute(
            "name",
            binding->second.name);
        lane.target = binding->second.target;
        if (trackIdValue.isNotEmpty())
            lane.target.trackId = trackIdValue;
        if (binding->second.domain == ParameterDomain::raw
            && !binding->second.validDomain)
        {
            fail(
                "import.parameter-domain",
                binding->second.path,
                binding->second.domainError);
            return;
        }
        const auto pointUnit = points.getStringAttribute(
            "unit",
            binding->second.unit);
        auto pointMinimum = binding->second.minimum;
        auto pointMaximum = binding->second.maximum;
        if (binding->second.domain == ParameterDomain::raw
            && pointUnit != binding->second.unit)
        {
            if (pointUnit == "normalized")
            {
                pointMinimum = 0.0;
                pointMaximum = 1.0;
            }
            else if (pointUnit == "percent")
            {
                pointMinimum = 0.0;
                pointMaximum = 100.0;
            }
            else
            {
                fail(
                    "import.automation-unit",
                    path,
                    "Device automation unit '" + pointUnit
                        + "' cannot be converted from parameter unit '"
                        + binding->second.unit + "'.");
                return;
            }
        }
        lane.timebase =
            points.getStringAttribute(
                "timeUnit",
                inheritedTimeUnit.isNotEmpty()
                    ? inheritedTimeUnit
                    : juce::String("beats"))
                    == "seconds"
            ? AutomationTimebase::seconds
            : AutomationTimebase::beats;
        auto pointIndex = 0;
        std::optional<AutomationInterpolation> importedInterpolation;
        const auto children = xmlChildren(points);
        for (std::size_t childIndex = 0;
             childIndex < children.size();
             ++childIndex)
        {
            const auto* point = children[childIndex];
            if (point->hasTagName("Target"))
                continue;
            const auto pointPath =
                xmlChildPath(path, children, childIndex);
            if (point->hasTagName("TimeSignaturePoint"))
            {
                warn(
                    "unsupported.automation-point-type",
                    path,
                    "Time-signature parameter automation is supported only by the arrangement time-signature map.");
                continue;
            }
            ++pointIndex;
            AutomationPoint imported;
            imported.id = ids.importedId(
                "automation-point",
                path + "/Point[" + juce::String(pointIndex) + "]",
                mappingError);
            imported.position =
                point->getDoubleAttribute("time");
            if (point->hasTagName("BoolPoint"))
            {
                imported.value =
                    boolAttribute(*point, "value", false)
                    ? 1.0
                    : 0.0;
            }
            else
            {
                const auto sourceValue =
                    point->getDoubleAttribute("value");
                if (binding->second.domain == ParameterDomain::raw)
                {
                    imported.value =
                        (sourceValue - pointMinimum)
                        / (pointMaximum - pointMinimum);
                    if (!std::isfinite(imported.value)
                        || imported.value < 0.0
                        || imported.value > 1.0)
                    {
                        fail(
                            "import.automation-value",
                            pointPath,
                            "Device automation value is outside its declared parameter domain.");
                        continue;
                    }
                }
                else
                {
                    imported.value = convertDawValue(
                        sourceValue,
                        binding->second.domain,
                        pointUnit,
                        pointMinimum,
                        pointMaximum);
                }
            }
            const auto pointInterpolation =
                point->hasTagName("RealPoint")
                    && point->getStringAttribute(
                           "interpolation",
                           "hold") == "linear"
                ? AutomationInterpolation::linear
                : AutomationInterpolation::step;
            if (importedInterpolation.has_value()
                && *importedInterpolation != pointInterpolation)
            {
                warn(
                    "unsupported.automation-interpolation",
                    path,
                    "Mixed per-point interpolation is imported using the first point's lane interpolation.");
            }
            else if (!importedInterpolation.has_value())
            {
                importedInterpolation = pointInterpolation;
                lane.interpolation = pointInterpolation;
            }
            lane.points.push_back(std::move(imported));
        }
        project.automationLanes.push_back(std::move(lane));
    }

    void translateScenes(const juce::XmlElement& scenes)
    {
        const auto children = xmlChildren(scenes);
        for (std::size_t index = 0; index < children.size(); ++index)
        {
            const auto* element = children[index];
            if (!element->hasTagName("Scene"))
                continue;
            const auto path =
                xmlChildPath("/Project/Scenes", children, index);
            ProjectScene scene;
            juce::String mappingError;
            scene.id = ids.importedId(
                "scene",
                externalOrPathId(*element, path),
                mappingError);
            scene.name =
                element->getStringAttribute("name", "Scene");
            scene.colour = colourFromText(
                element->getStringAttribute("color"),
                scene.colour);
            if (element->getStringAttribute("comment").isNotEmpty())
            {
                warn(
                    "unsupported.scene-comment",
                    path,
                    "Scene comments are not stored by the current Studio Duo scene model.");
            }
            const auto sceneChildren = xmlChildren(*element);
            if (sceneChildren.empty())
            {
                warn(
                    "unsupported.scene-empty",
                    path,
                    "The scene does not contain a timeline.");
            }
            else
            {
                translateSceneTimeline(
                    *sceneChildren.front(),
                    scene,
                    "beats",
                    path + "/" + sceneChildren.front()->getTagName()
                        + "[1]");
            }
            project.scenes.push_back(std::move(scene));
        }
    }

    void translateSceneTimeline(const juce::XmlElement& timeline,
                                ProjectScene& scene,
                                const juce::String& inheritedTimeUnit,
                                const juce::String& path)
    {
        const auto timeUnit = timeline.getStringAttribute(
            "timeUnit",
            inheritedTimeUnit.isNotEmpty()
                ? inheritedTimeUnit
                : juce::String("beats"));
        if (timeline.hasTagName("Lanes"))
        {
            const auto children = xmlChildren(timeline);
            for (std::size_t index = 0; index < children.size(); ++index)
            {
                const auto childPathValue =
                    xmlChildPath(path, children, index);
                if (children[index]->hasTagName("ClipSlot"))
                {
                    translateSceneSlot(
                        *children[index],
                        scene,
                        timeUnit,
                        childPathValue);
                }
                else if (children[index]->hasTagName("Lanes"))
                {
                    translateSceneTimeline(
                        *children[index],
                        scene,
                        timeUnit,
                        childPathValue);
                }
                else
                {
                    warn(
                        "unsupported.scene-timeline",
                        childPathValue,
                        "Only track clip slots are represented in Studio Duo scenes.");
                }
            }
            return;
        }
        if (timeline.hasTagName("ClipSlot"))
        {
            translateSceneSlot(
                timeline,
                scene,
                timeUnit,
                path);
            return;
        }
        if (timeline.hasTagName("Clips"))
        {
            const auto trackIdValue = resolvedTrackId(
                timeline.getStringAttribute("track"),
                path + "/@track");
            translateClips(
                timeline,
                trackIdValue,
                timeUnit,
                path,
                &scene);
            return;
        }
        if (timeline.hasTagName("Notes")
            || timeline.hasTagName("Audio")
            || timeline.hasTagName("Warps"))
        {
            const auto trackIdValue = resolvedTrackId(
                timeline.getStringAttribute("track"),
                path + "/@track");
            if (trackIdValue.isEmpty())
            {
                warn(
                    "unsupported.scene-track",
                    path,
                    "The scene timeline does not identify an available track.");
                return;
            }
            juce::XmlElement syntheticClip("Clip");
            syntheticClip.setAttribute("time", "0");
            syntheticClip.setAttribute(
                "duration",
                timeline.hasTagName("Notes")
                    ? 0.0
                    : timeline.getDoubleAttribute(
                          "duration",
                          0.001));
            syntheticClip.setAttribute(
                "contentTimeUnit",
                timeUnit);
            syntheticClip.setAttribute("name", scene.name);
            syntheticClip.addChildElement(
                new juce::XmlElement(timeline));
            translateClip(
                syntheticClip,
                trackIdValue,
                timeUnit,
                path,
                &scene,
                false);
            return;
        }
        warn(
            "unsupported.scene-timeline",
            path,
            "Only clip-slot scene timelines are supported.");
    }

    void translateSceneSlot(const juce::XmlElement& slot,
                            ProjectScene& scene,
                            const juce::String& inheritedTimeUnit,
                            const juce::String& path)
    {
        const auto trackIdValue = resolvedTrackId(
            slot.getStringAttribute("track"),
            path + "/@track");
        if (trackIdValue.isEmpty())
            return;
        const auto* clip = slot.getChildByName("Clip");
        if (clip == nullptr)
        {
            if (std::any_of(
                    scene.slots.cbegin(),
                    scene.slots.cend(),
                    [&trackIdValue](const auto& existing)
                    {
                        return existing.trackId == trackIdValue;
                    }))
            {
                warn(
                    "unsupported.duplicate-scene-slot",
                    path,
                    "Studio Duo keeps the first scene slot for each track.");
                return;
            }
            SceneSlot imported;
            juce::String mappingError;
            imported.id = ids.importedId(
                "scene-slot",
                externalOrPathId(slot, path),
                mappingError);
            imported.trackId = trackIdValue;
            imported.stopTrack =
                boolAttribute(slot, "hasStop", false);
            scene.slots.push_back(std::move(imported));
            return;
        }
        translateClip(
            *clip,
            trackIdValue,
            slot.getStringAttribute(
                "timeUnit",
                inheritedTimeUnit.isNotEmpty()
                    ? inheritedTimeUnit
                    : juce::String("beats")),
            path + "/Clip[1]",
            &scene,
            boolAttribute(slot, "hasStop", false));
    }

    void ensureMasterTrack()
    {
        auto foundMaster = false;
        for (auto& track : project.tracks)
        {
            if (track.type != TrackType::master)
                continue;
            if (!foundMaster)
            {
                foundMaster = true;
                continue;
            }
            warn(
                "unsupported.multiple-master",
                trackPath(track),
                "Studio Duo keeps the first master channel and imports additional masters as submix buses.");
            track.type = TrackType::bus;
        }
        if (foundMaster)
            return;
        juce::String mappingError;
        Track master;
        master.id = ids.importedId(
            "track",
            "studio-duo-synthesized-master",
            mappingError);
        master.name = "Master";
        master.type = TrackType::master;
        master.colour = juce::Colour(0xff78c6a3);
        project.tracks.push_back(std::move(master));
        warn(
            "import.synthesized-master",
            "/Project/Structure",
            "The source has no master channel; Studio Duo created one.");
    }

    void resolveRoutes()
    {
        for (const auto& pending : pendingMainRoutes)
        {
            const auto destination =
                ids.internalId(
                    "channel",
                    pending.destinationChannelId);
            if (destination.isEmpty())
            {
                warn(
                    "unsupported.routing-destination",
                    pending.path,
                    "The main output references unavailable channel ID '"
                        + pending.destinationChannelId + "'.");
                continue;
            }
            RoutingConnection route;
            juce::String mappingError;
            route.id = ids.importedId(
                "route",
                "main:"
                    + pending.sourceTrackId
                    + ":" + pending.destinationChannelId,
                mappingError);
            route.name = "Main output";
            route.kind = RouteKind::mainOutput;
            route.sourceTrackId = pending.sourceTrackId;
            route.destination.trackId = destination;
            project.routingConnections.push_back(std::move(route));
        }
        for (const auto& pending : pendingSends)
        {
            const auto destination =
                ids.internalId(
                    "channel",
                    pending.destinationChannelId);
            if (destination.isEmpty())
            {
                warn(
                    "unsupported.send-destination",
                    pending.path,
                    "The send references unavailable channel ID '"
                        + pending.destinationChannelId + "'.");
                continue;
            }
            RoutingConnection route;
            route.id = pending.id;
            route.name = pending.name;
            route.kind = RouteKind::send;
            route.tap = pending.tap;
            route.sourceTrackId = pending.sourceTrackId;
            route.destination.trackId = destination;
            route.gainDecibels = pending.gainDecibels;
            route.pan = pending.pan;
            project.routingConnections.push_back(std::move(route));
        }
    }

    bool publishStaging()
    {
        if (destinationPackage.exists())
        {
            fail(
                "import.destination-exists",
                "/",
                "The import destination already exists; choose a new Studio Duo project path.");
            return false;
        }
        if (!stagingPackage.moveFileTo(destinationPackage))
        {
            fail(
                "import.publish",
                "/",
                "Could not atomically publish the imported project.");
            return false;
        }
        stagingPackage = juce::File();
        return true;
    }
};

juce::Result writeJsonAtomically(const juce::File& destination,
                                 const juce::var& value)
{
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail(
            "Could not create the report destination directory.");
    const auto temporary = destination.getSiblingFile(
        destination.getFileName()
        + ".tmp-" + juce::Uuid().toString());
    {
        auto output = temporary.createOutputStream();
        if (output == nullptr)
            return juce::Result::fail(
                "Could not open a temporary compatibility report.");
        output->writeText(
            juce::JSON::toString(value, true),
            false,
            false,
            "\n");
        output->writeText("\n", false, false, "\n");
        output->flush();
        if (output->getStatus().failed())
        {
            const auto result = output->getStatus();
            output.reset();
            temporary.deleteFile();
            return result;
        }
    }
    const auto published = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);
    if (!published)
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not atomically publish the compatibility report.");
    }
    return juce::Result::ok();
}
}

juce::File DawProjectIO::normaliseArchivePath(
    const juce::File& requestedPath)
{
    if (requestedPath.hasFileExtension("dawproject"))
        return requestedPath;
    return requestedPath.withFileExtension("dawproject");
}

#if STUDIO_DUO_TESTING
void DawProjectIO::setExportTestHookForTesting(ExportTestHook hook)
{
    exportTestHook() = std::move(hook);
}
#endif

DawProjectExportResult DawProjectIO::exportProject(
    const Project& project,
    const juce::File& sourcePackage,
    const juce::File& requestedDestination)
{
    const auto destination =
        normaliseArchivePath(requestedDestination);
    try
    {
        Exporter exporter(project, sourcePackage, destination);
        return exporter.run();
    }
    catch (const std::bad_alloc&)
    {
        DawProjectExportResult result;
        result.result = juce::Result::fail(
            "DAWproject export ran out of memory while preparing bounded metadata; no archive was published.");
        result.report = makeReport(
            "export",
            sourcePackage,
            destination);
        addIssue(
            result.report,
            CompatibilitySeverity::error,
            "export.memory",
            "/Archive",
            result.result.getErrorMessage());
        return result;
    }
}

DawProjectImportResult DawProjectIO::importProject(
    const juce::File& sourceArchive,
    const juce::File& destinationPackage)
{
    Importer importer(sourceArchive, destinationPackage);
    return importer.run();
}

juce::Result DawProjectIO::saveCompatibilityReport(
    const CompatibilityReport& report,
    const juce::File& destination)
{
    return writeJsonAtomically(destination, report.toVar());
}
}
