#include "TestHarness.h"
#include "TestSuites.h"

#include "project_io/ProjectFile.h"
#include "project_io/ProjectCollectionService.h"
#include "project_io/ProjectArchiveReader.h"
#include "project_io/ProjectImportService.h"
#include "dawproject_io/DawProjectIO.h"
#include "studio_one_io/StudioOneProjectIO.h"
#include "ui/StartupHubComponent.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace
{
struct SongFixture
{
    juce::File root = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile(
            "studio-duo-song-test-" + juce::Uuid().toString());
    juce::File source = root.getChildFile("Original");
    juce::File song = source.getChildFile("Synthetic.song");
    juce::File destination = root.getChildFile("Imported.studioduo");
    std::map<juce::String, juce::String> entries {
        { "metainfo.xml", R"xml(<MetaInformation>
<Attribute id="Document:Title" value="Synthetic Song"/>
<Attribute id="Media:Tempo" value="120"/>
<Attribute id="Media:TimeSignatureNumerator" value="4"/>
<Attribute id="Media:TimeSignatureDenominator" value="4"/>
</MetaInformation>)xml" },
        { "Song/song.xml", R"xml(<Song>
<Attributes x:id="Root" defaultTimeFormat="2">
<Attributes x:id="timeContext">
<TempoMap x:id="tempoMap">
<TempoMapSegment curveType="0" start="0" tempo="0.5"/>
</TempoMap>
<TimeSignatureMap x:id="timeSignatureMap">
<TimeSignatureMapSegment start="0" numerator="4" denominator="4"/>
</TimeSignatureMap>
</Attributes>
<List x:id="Tracks">
<MediaTrack mediaType="Audio" trackID="audio-track" trackNumber="1" name="Synthetic Audio" color="FF336699" timeFormat="2">
<List x:id="Events">
<AudioEvent clipID="audio-media" timeFormat="2" start="2" length="2" offset="0.25" name="Synthetic Clip" speed="1"/>
</List>
</MediaTrack>
</List>
</Attributes>
</Song>)xml" },
        { "Song/mediapool.xml", R"xml(<MediaPool>
<Attributes x:id="rootFolder">
<MediaFolder name="Audio">
<AudioClip mediaID="audio-media">
<Url x:id="path" type="1" url="Media/clip.wav"/>
</AudioClip>
</MediaFolder>
</Attributes>
</MediaPool>)xml" }
    };

    SongFixture()
    {
        expect(source.getChildFile("Media").createDirectory().wasOk(),
               "Synthetic Studio One source directory can be created.");
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream =
            source.getChildFile("Media/clip.wav").createOutputStream();
        auto writer = format.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions {}
                .withSampleRate(48000.0)
                .withNumChannels(1)
                .withBitsPerSample(16));
        juce::AudioBuffer<float> audio(1, 48000 * 4);
        audio.clear();
        audio.setSample(0, 100, 0.25f);
        expect(writer != nullptr
                   && writer->writeFromAudioSampleBuffer(
                       audio, 0, audio.getNumSamples()),
               "Synthetic Studio One audio can be written.");
    }

    ~SongFixture()
    {
        expect(root.deleteRecursively(),
               "Synthetic Studio One test files can be removed.");
    }

    void write()
    {
        expect(song.deleteFile(),
               "Previous synthetic Studio One archive can be replaced.");
        juce::ZipFile::Builder builder;
        for (const auto& [path, xml] : entries)
        {
            const auto bytes = xml.toStdString();
            builder.addEntry(
                std::make_unique<juce::MemoryInputStream>(
                    bytes.data(), bytes.size(), true),
                9,
                path,
                juce::Time(1980, 0, 1, 0, 0));
        }
        auto output = song.createOutputStream();
        expect(output != nullptr
                   && builder.writeToStream(*output, nullptr),
               "Synthetic Studio One archive can be written.");
    }
};

struct ScopedResourceTestHooks
{
    ~ScopedResourceTestHooks()
    {
        studio::StudioOneProjectIO::setMediaReadTestHookForTesting({});
        studio::ProjectCollectionService::setResourceTestHookForTesting({});
    }
};

void nativeAudioImport()
{
    SongFixture fixture;
    fixture.write();
    const auto result = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(result.succeeded(),
           "A native Studio One song imports without a DAWproject export.");
    if (!result.project.has_value())
        return;
    const auto& project = *result.project;
    const auto track = std::find_if(
        project.tracks.begin(), project.tracks.end(),
        [](const auto& candidate)
        {
            return candidate.name == "Synthetic Audio";
        });
    expect(project.name == "Synthetic Song"
               && track != project.tracks.end()
               && track->clips.size() == 1
               && std::abs(track->clips.front().startSeconds - 1.0) < 0.000001
               && std::abs(track->clips.front().durationSeconds - 1.0) < 0.000001
               && std::abs(track->clips.front().sourceOffsetSeconds - 0.25) < 0.000001
               && track->clips.front().sourceFile.existsAsFile()
               && track->clips.front().sourceFile.isAChildOf(result.package),
           "Native song metadata, beat-based audio placement and collected media are preserved.");
}

const studio::Track* trackNamed(
    const studio::Project& project,
    const juce::String& name)
{
    const auto found = std::find_if(
        project.tracks.begin(), project.tracks.end(),
        [&name](const auto& track) { return track.name == name; });
    return found != project.tracks.end() ? &*found : nullptr;
}

void nativeMixerImport()
{
    SongFixture fixture;
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"]
            .replace("<List x:id=\"Tracks\">",
                     "<List x:id=\"Tracks\"><FolderTrack trackID=\"folder-track\" name=\"Synthetic Folder\"/>")
            .replace("trackNumber=\"1\"",
                     "trackNumber=\"1\" parentFolder=\"folder-track\"")
            .replace("<List x:id=\"Events\">",
                     "<UID x:id=\"channelID\" uid=\"audio-channel\"/><List x:id=\"Events\">");
    fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer>
<ChannelGroup>
<AudioTrackChannel label="Synthetic Audio" gain="0.5" pan="0.75" mute="1" solo="1" soloSafe="1">
<UID x:id="uniqueID" uid="audio-channel"/>
<SpeakerSetup x:id="speakerType" type="Mono"/>
<Connection x:id="destination" objectID="bus-channel/Input"/>
<Attributes x:id="Sends">
<Attributes name="Send01" level="0.25" pan="0.5" prefader="1" bypass="0">
<Connection x:id="destination" objectID="effect-channel/Input"/>
</Attributes>
</Attributes>
</AudioTrackChannel>
<AudioGroupChannel label="Synthetic Bus" gain="1">
<UID x:id="uniqueID" uid="bus-channel"/>
<Connection x:id="destination" objectID="main-channel/Input"/>
</AudioGroupChannel>
<AudioEffectChannel label="Synthetic FX" gain="1">
<UID x:id="uniqueID" uid="effect-channel"/>
<Connection x:id="destination" objectID="main-channel/Input"/>
</AudioEffectChannel>
<AudioOutputChannel label="Synthetic Main" gain="1">
<UID x:id="uniqueID" uid="main-channel"/>
</AudioOutputChannel>
</ChannelGroup>
</AudioMixer>)xml";
    fixture.write();
    const auto result = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(result.succeeded(), "A native Studio One mixer imports successfully.");
    if (!result.project.has_value())
        return;
    const auto& project = *result.project;
    const auto* audio = trackNamed(project, "Synthetic Audio");
    const auto* folder = trackNamed(project, "Synthetic Folder");
    const auto* bus = trackNamed(project, "Synthetic Bus");
    const auto* effect = trackNamed(project, "Synthetic FX");
    const auto* main = trackNamed(project, "Synthetic Main");
    expect(audio != nullptr && folder != nullptr && bus != nullptr
               && effect != nullptr && main != nullptr
               && folder->type == studio::TrackType::folder
               && audio->folderTrackId == folder->id
               && audio->outputTrackId == bus->id
               && bus->type == studio::TrackType::bus
               && bus->outputTrackId == main->id
               && effect->type == studio::TrackType::aux
               && main->type == studio::TrackType::master
               && std::abs(audio->volumeDecibels + 6.0206f) < 0.001f
               && std::abs(audio->pan - 0.5f) < 0.000001f
               && audio->muted && audio->solo && audio->soloSafe
               && audio->channelLayout == studio::ChannelLayout::mono
               && std::any_of(
                   project.routingConnections.begin(), project.routingConnections.end(),
                   [audio, effect](const auto& route)
                   {
                       return route.kind == studio::RouteKind::send
                           && route.sourceTrackId == audio->id
                           && route.destination.trackId == effect->id
                           && route.tap == studio::RouteTap::preFader
                           && std::abs(route.gainDecibels + 12.0412f) < 0.001f;
                   }),
           "Native folder hierarchy, mixer values, buses, FX returns and pre-fader sends retain their relationships.");
}

void nativeTransportAndMetadata()
{
    SongFixture fixture;
    fixture.entries["metainfo.xml"] =
        fixture.entries["metainfo.xml"]
            .replace("value=\"120\"", "value=\"160\"")
            .replace("Numerator\" value=\"4\"", "Numerator\" value=\"7\"")
            .replace("Denominator\" value=\"4\"", "Denominator\" value=\"8\"")
            .replace("</MetaInformation>", R"xml(
<Attribute id="Media:Artist" value="Synthetic Artist"/>
<Attribute id="Media:Album" value="Synthetic Album"/>
<Attribute id="Media:Comment" value="Synthetic Comment"/>
</MetaInformation>)xml");
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"]
            .replace("tempo=\"0.5\"", "tempo=\"0.375\"")
            .replace("numerator=\"4\" denominator=\"4\"",
                     "numerator=\"7\" denominator=\"8\"")
            .replace("timeFormat=\"2\" start=\"2\" length=\"2\"",
                     "timeFormat=\"0\" start=\"2\" length=\"0.75\"")
            .replace("speed=\"1\"", "speed=\"2\"")
            .replace("<List x:id=\"Tracks\">", R"xml(<List x:id="Tracks">
<MarkerTrack trackID="markers" timeFormat="2">
<MarkerEvent name="Synthetic Start" start="4" markerType="0"/>
<MarkerEvent name="Synthetic End" start="12" markerType="3"/>
</MarkerTrack>)xml");
    fixture.write();
    const auto result = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(result.succeeded(), "A native Studio One transport imports successfully.");
    if (!result.project.has_value())
        return;
    const auto& project = *result.project;
    const auto* audio = trackNamed(project, "Synthetic Audio");
    expect(project.tempo == 160.0
               && project.timeSignatureNumerator == 7
               && project.timeSignatureDenominator == 8
               && project.metadata.artist == "Synthetic Artist"
               && project.metadata.album == "Synthetic Album"
               && project.metadata.comment == "Synthetic Comment"
               && project.markers.size() == 2
               && std::abs(project.markers[0].timeSeconds - 1.5) < 0.000001
               && std::abs(project.markers[1].timeSeconds - 4.5) < 0.000001
               && audio != nullptr && audio->clips.size() == 1
               && audio->colour == juce::Colour(0xff336699)
               && audio->clips.front().colour == audio->colour
               && audio->clips.front().startSeconds == 2.0
               && audio->clips.front().durationSeconds == 0.75
               && audio->clips.front().playbackRate == 2.0,
           "Native metadata, meter, inherited marker time units, seconds-based clips, speed and colours are preserved.");
}

void relocatedNativeMedia()
{
    SongFixture fixture;
    const auto movedAudio = fixture.source.getChildFile("Media/Nested Space/clip.wav");
    expect(movedAudio.getParentDirectory().createDirectory().wasOk()
               && fixture.source.getChildFile("Media/clip.wav").moveFileTo(movedAudio),
           "Synthetic relocated audio can be prepared.");
    fixture.entries["Song/mediapool.xml"] =
        fixture.entries["Song/mediapool.xml"].replace(
            "Media/clip.wav", "file:///Z:/Synthetic%20Song/Media/Nested%20Space/clip.wav");
    fixture.write();
    const auto songHash = juce::SHA256(fixture.song).toHexString();
    const auto audioHash = juce::SHA256(movedAudio).toHexString();
    const auto result = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(result.succeeded(),
           "A relocated native song resolves old Windows paths from its current Media folder.");
    expect(juce::SHA256(fixture.song).toHexString() == songHash
               && juce::SHA256(movedAudio).toHexString() == audioHash,
           "Native import never rewrites the source song or external audio.");
    if (!result.project.has_value())
        return;
    const auto* track = trackNamed(*result.project, "Synthetic Audio");
    expect(track != nullptr && track->clips.size() == 1
               && track->clips.front().sourceFile.isAChildOf(result.package)
               && juce::SHA256(track->clips.front().sourceFile).toHexString() == audioHash,
           "Relocated native media is copied byte-for-byte into the portable Studio Duo package.");
}

bool hasIssue(const studio::CompatibilityReport& report,
              const juce::String& code)
{
    return std::any_of(
        report.issues.begin(), report.issues.end(),
        [&code](const auto& issue) { return issue.code == code; });
}

void nativeChannelIdsMustBeUnique()
{
    const std::vector<std::function<void(SongFixture&)>> invalidCases {
        [](auto& fixture)
        {
            fixture.entries["Song/song.xml"] =
                fixture.entries["Song/song.xml"].replace("uid=\"audio-channel\"", "uid=\" \"");
        },
        [](auto& fixture)
        {
            fixture.entries["Song/song.xml"] =
                fixture.entries["Song/song.xml"].replace(
                    "</MediaTrack>", R"xml(</MediaTrack>
<MediaTrack mediaType="Audio" trackID="second-track" name="Synthetic Second">
<UID x:id="channelID" uid="AUDIO-CHANNEL"/>
</MediaTrack>)xml");
        },
        [](auto& fixture)
        {
            fixture.entries["Song/song.xml"] =
                fixture.entries["Song/song.xml"].replace(
                    "<List x:id=\"Events\">",
                    "<UID x:id=\"channelID\" uid=\"second-channel\"/><List x:id=\"Events\">");
        },
        [](auto& fixture)
        {
            fixture.entries["Devices/audiomixer.xml"] =
                fixture.entries["Devices/audiomixer.xml"].replace("uid=\"audio-channel\"", "uid=\"\"");
        },
        [](auto& fixture)
        {
            fixture.entries["Devices/audiomixer.xml"] =
                fixture.entries["Devices/audiomixer.xml"].replace(
                    "</ChannelGroup>", R"xml(<AudioTrackChannel label="Synthetic Duplicate" gain="0.5">
<UID x:id="uniqueID" uid="AUDIO-CHANNEL"/>
</AudioTrackChannel></ChannelGroup>)xml");
        },
        [](auto& fixture)
        {
            fixture.entries["Devices/audiomixer.xml"] =
                fixture.entries["Devices/audiomixer.xml"].replace(
                    "<UID x:id=\"uniqueID\" uid=\"audio-channel\"/>",
                    "<UID x:id=\"uniqueID\" uid=\"audio-channel\"/>"
                    "<UID x:id=\"uniqueID\" uid=\"second-channel\"/>");
        },
        [](auto& fixture)
        {
            fixture.entries["Song/song.xml"] =
                fixture.entries["Song/song.xml"]
                    .replace("uid=\"audio-channel\"", "uid=\"{01234567-89AB-4CDE-8F01-23456789ABCD}\"")
                    .replace("</MediaTrack>", R"xml(</MediaTrack>
<MediaTrack mediaType="Audio" trackID="second-track" name="Synthetic Second">
<UID x:id="channelID" uid="0123456789ab4cde8f0123456789abcd"/>
</MediaTrack>)xml");
        }
    };
    for (const auto& modify : invalidCases)
    {
        SongFixture fixture;
        fixture.entries["Song/song.xml"] =
            fixture.entries["Song/song.xml"].replace(
                "<List x:id=\"Events\">",
                "<UID x:id=\"channelID\" uid=\"audio-channel\"/><List x:id=\"Events\">");
        fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer><ChannelGroup>
<AudioTrackChannel label="Synthetic Audio" gain="1">
<UID x:id="uniqueID" uid="audio-channel"/>
</AudioTrackChannel>
</ChannelGroup></AudioMixer>)xml";
        modify(fixture);
        fixture.write();
        const auto sourceHash = juce::SHA256(fixture.song).toHexString();
        const auto result = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination, true);
        expect(!result.succeeded() && !result.project.has_value()
                   && !result.requiresCompatibilityConfirmation
                   && !fixture.destination.exists()
                   && hasIssue(result.report, "import.channel-id")
                   && std::any_of(
                       result.report.issues.begin(), result.report.issues.end(),
                       [](const auto& issue)
                       {
                           return issue.code == "import.channel-id"
                               && issue.severity == studio::CompatibilitySeverity::error
                               && issue.objectPath.isNotEmpty();
                       })
                   && juce::SHA256(fixture.song).toHexString() == sourceHash,
               "Empty, repeated, and normalized-equivalent native channel IDs fail without publishing or changing the source.");
    }
}

void nativeFolderChannelMapping()
{
    SongFixture fixture;
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"]
            .replace("<List x:id=\"Tracks\">", R"xml(<List x:id="Tracks">
<FolderTrack trackID="folder-track" name="Synthetic Folder">
<UID x:id="channelID" uid="folder-channel"/>
</FolderTrack>)xml")
            .replace("trackNumber=\"1\"", "trackNumber=\"1\" parentFolder=\"folder-track\"")
            .replace("<List x:id=\"Events\">",
                     "<UID x:id=\"channelID\" uid=\"audio-channel\"/><List x:id=\"Events\">");
    fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer><ChannelGroup>
<AudioGroupChannel label="Synthetic Folder Bus" gain="1">
<UID x:id="uniqueID" uid="folder-channel"/>
</AudioGroupChannel>
<AudioTrackChannel label="Synthetic Audio" gain="1">
<UID x:id="uniqueID" uid="audio-channel"/>
<Connection x:id="destination" objectID="folder-channel/Input"/>
</AudioTrackChannel>
</ChannelGroup></AudioMixer>)xml";
    fixture.write();
    const auto result = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(result.succeeded(), "A unique folder-to-mixer channel reference remains valid.");
    if (!result.project.has_value())
        return;
    const auto* folder = trackNamed(*result.project, "Synthetic Folder");
    const auto* bus = trackNamed(*result.project, "Synthetic Folder Bus");
    const auto* audio = trackNamed(*result.project, "Synthetic Audio");
    expect(folder != nullptr && bus != nullptr && audio != nullptr
               && folder->type == studio::TrackType::folder
               && bus->type == studio::TrackType::bus
               && audio->folderTrackId == folder->id
               && bus->folderTrackId == folder->id
               && audio->outputTrackId == bus->id,
           "Folder channel bindings still create the correct bus and preserve audio routing.");
}

void nativeColourLossRequiresConsent()
{
    const std::vector<std::function<void(SongFixture&)>> invalidColours {
        [](auto& fixture)
        {
            fixture.entries["Song/song.xml"] =
                fixture.entries["Song/song.xml"].replace("color=\"FF336699\"", "color=\"invalid\"");
        },
        [](auto& fixture)
        {
            fixture.entries["Song/song.xml"] =
                fixture.entries["Song/song.xml"].replace("speed=\"1\"", "speed=\"1\" color=\"\"");
        },
        [](auto& fixture)
        {
            fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer><ChannelGroup>
<AudioTrackChannel label="Synthetic Colour Channel" gain="1" color="FF12345">
<UID x:id="uniqueID" uid="colour-channel"/>
</AudioTrackChannel>
</ChannelGroup></AudioMixer>)xml";
        }
    };
    for (const auto& modify : invalidColours)
    {
        SongFixture fixture;
        modify(fixture);
        fixture.write();
        const auto before = juce::SHA256(fixture.song).toHexString();
        const auto blocked = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination);
        expect(!blocked.succeeded() && !blocked.project.has_value()
                   && blocked.requiresCompatibilityConfirmation
                   && !blocked.report.hasErrors() && !fixture.destination.exists()
                   && hasIssue(blocked.report, "unsupported.colour"),
               "Unsupported track, event, or mixer colours cannot publish before partial-import consent.");
        if (!blocked.requiresCompatibilityConfirmation)
            continue;
        const auto accepted = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination, true);
        expect(accepted.succeeded() && !accepted.requiresCompatibilityConfirmation
                   && accepted.project->compatibilityReports.size() == 1
                   && hasIssue(accepted.project->compatibilityReports.front(), "unsupported.colour")
                   && juce::SHA256(fixture.song).toHexString() == before,
               "Explicit colour-loss consent publishes a native package with its compatibility notice preserved.");
    }
}

void repeatedNativeMediaIsReadOncePerPhase()
{
    SongFixture fixture;
    constexpr auto eventCount = 32;
    juce::String events;
    for (auto index = 0; index < eventCount; ++index)
    {
        const auto mediaId = index % 2 == 0 ? "audio-media" : "alias-media";
        events += "<AudioEvent clipID=\"" + juce::String(mediaId)
            + "\" start=\"" + juce::String(index * 2)
            + "\" timeFormat=\"2\" length=\"2\" offset=\"0.25\" speed=\"1\"/>";
    }
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"].replace(
            R"xml(<AudioEvent clipID="audio-media" timeFormat="2" start="2" length="2" offset="0.25" name="Synthetic Clip" speed="1"/>)xml",
            events);
    fixture.entries["Song/mediapool.xml"] =
        fixture.entries["Song/mediapool.xml"].replace(
            "</MediaFolder>",
            "<AudioClip mediaID=\"alias-media\"><Url x:id=\"path\" url=\""
                + fixture.source.getChildFile("Media/clip.wav").getFullPathName()
                + "\"/></AudioClip></MediaFolder>");
    fixture.write();
    const auto expectedHash = juce::SHA256(fixture.source.getChildFile("Media/clip.wav")).toHexString();
    const ScopedResourceTestHooks hooks;
    auto nativeReads = 0;
    std::array<int, 3> hashReads {};
    studio::StudioOneProjectIO::setMediaReadTestHookForTesting(
        [&nativeReads](const auto&) { ++nativeReads; });
    studio::ProjectCollectionService::setResourceTestHookForTesting(
        [&hashReads](auto phase, const auto&)
        {
            ++hashReads[static_cast<std::size_t>(phase)];
        });
    for (auto attempt = 1; attempt <= 2; ++attempt)
    {
        const auto destination = fixture.root.getChildFile(
            "Repeated-" + juce::String(attempt) + ".studioduo");
        const auto result = studio::StudioOneProjectIO::importSong(fixture.song, destination);
        expect(result.succeeded(), "Repeated native media references import successfully.");
        expect(nativeReads == attempt && hashReads == std::array<int, 3> { attempt, attempt, attempt },
               "Metadata and full-file digest reads scale with unique files per import/collection/manifest/verification phase, not event count.");
        if (!result.project.has_value())
            continue;
        const auto* track = trackNamed(*result.project, "Synthetic Audio");
        expect(track != nullptr && track->clips.size() == eventCount
                   && result.package.getChildFile("media").getNumberOfChildFiles(juce::File::findFiles) == 1
                   && std::all_of(
                       track->clips.begin(), track->clips.end(),
                       [&expectedHash](const auto& clip)
                       {
                           return clip.sourceHash == expectedHash
                               && clip.sourceFile.existsAsFile()
                               && juce::SHA256(clip.sourceFile).toHexString() == expectedHash;
                       }),
               "All alias references retain their independently verified bytes and share one portable media file.");
    }
    const auto package = fixture.root.getChildFile("Repeated-2.studioduo");
    juce::String error;
    const auto validation = studio::ProjectCollectionService::validatePortableCopy(package, error);
    expect(validation.has_value() && validation->invalidResources == 0
               && hashReads == std::array<int, 3> { 2, 2, 3 },
           "Each independent verification rehashes every unique file instead of trusting an earlier phase's cache.");
    const auto manifestFile = package.getChildFile("portable-manifest.json");
    auto manifest = juce::JSON::parse(manifestFile.loadFileAsString());
    auto* object = manifest.getDynamicObject();
    auto* resources = object != nullptr ? object->getProperty("resources").getArray() : nullptr;
    expect(resources != nullptr && resources->size() == eventCount,
           "Every logical reference remains represented in the portable manifest.");
    if (resources == nullptr || resources->size() != eventCount)
        return;
    resources->getReference(eventCount - 1).getDynamicObject()->setProperty(
        "sha256", juce::String::repeatedString("f", 64));
    expect(manifestFile.replaceWithText(juce::JSON::toString(manifest)),
           "Synthetic per-reference digest corruption can be written.");
    const auto corrupted = studio::ProjectCollectionService::validatePortableCopy(package, error);
    expect(corrupted.has_value() && corrupted->invalidResources > 0
               && hashReads == std::array<int, 3> { 2, 2, 4 },
           "A cached file digest is still compared against every manifest reference.");
}

void cachedMediaKeepsSourceIntegrity()
{
    SongFixture fixture;
    const auto source = fixture.source.getChildFile("Media/clip.wav");
    const auto sourceHash = juce::SHA256(source).toHexString();
    auto project = studio::Project::createDefault();
    studio::AudioClip clip;
    clip.sourceFile = source;
    clip.sourceHash = sourceHash;
    clip.durationSeconds = 1.0;
    clip.sourceLengthSeconds = 4.0;
    clip.sourceRangeEndSeconds = 4.0;
    auto other = clip;
    other.id = juce::Uuid().toString();
    other.sourceHash = juce::String::repeatedString("f", 64);
    project.tracks.front().clips = { clip, other };
    const ScopedResourceTestHooks hooks;
    auto sourceReads = 0;
    studio::ProjectCollectionService::setResourceTestHookForTesting(
        [&sourceReads](auto phase, const auto&)
        {
            if (phase == studio::ProjectCollectionService::ResourceTestPhase::sourceHash)
                ++sourceReads;
        });
    juce::String error;
    const auto conflicting = studio::ProjectCollectionService::savePortableCopy(
        project, fixture.source, fixture.destination, error);
    expect(!conflicting.has_value() && error.isNotEmpty()
               && !fixture.destination.exists() && sourceReads == 1,
           "Repeated source references with conflicting expected digests are rejected even on a cache hit.");

    other.sourceHash = sourceHash;
    project.tracks.front().clips = { clip, other };
    auto corruptedOnce = false;
    studio::ProjectCollectionService::setResourceTestHookForTesting(
        [&corruptedOnce](auto phase, const auto& file)
        {
            if (phase != studio::ProjectCollectionService::ResourceTestPhase::manifestHash || corruptedOnce)
                return;
            corruptedOnce = true;
            juce::MemoryBlock bytes;
            expect(file.loadFileAsData(bytes) && bytes.getSize() > 0,
                   "Collected synthetic audio can be read for the corruption fixture.");
            if (bytes.isEmpty())
                return;
            auto* data = static_cast<unsigned char*>(bytes.getData());
            data[bytes.getSize() - 1] ^= 1;
            expect(file.replaceWithData(bytes.getData(), bytes.getSize()),
                   "Collected synthetic audio can be corrupted without changing its size.");
        });
    const auto corrupted = studio::ProjectCollectionService::savePortableCopy(
        project, fixture.source, fixture.destination, error);
    expect(corruptedOnce && !corrupted.has_value() && !fixture.destination.exists()
               && error.isNotEmpty() && juce::SHA256(source).toHexString() == sourceHash,
           "Staged bytes must match their source reference digest, not merely a regenerated manifest digest.");
}

void nativeCompatibilityRequiresConfirmation()
{
    SongFixture fixture;
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"]
            .replace("</MediaTrack>", R"xml(
<Attributes x:id="automation">
<AutomationRegion name="Synthetic Volume">
<Url x:id="data" url="Envelopes/Synthetic/volume.envelopex"/>
</AutomationRegion>
</Attributes>
</MediaTrack>
<MediaTrack mediaType="Music" trackID="music-track" name="Synthetic MIDI">
<List x:id="Events">
<MusicPart name="Synthetic Notes" clipID="native-music" start="4" length="4" timeFormat="2"/>
</List>
</MediaTrack>)xml");
    fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer>
<ChannelGroup>
<AudioTrackChannel label="Synthetic FX Track" gain="1">
<UID x:id="uniqueID" uid="processor-channel"/>
<Attributes x:id="Inserts">
<Attributes name="FX01" bypass="0">
<Attributes x:id="deviceData" name="Synthetic Processor"/>
<String x:id="presetPath" text="Presets/Synthetic.preset"/>
</Attributes>
</Attributes>
</AudioTrackChannel>
</ChannelGroup>
</AudioMixer>)xml";
    fixture.entries["Envelopes/Synthetic/volume.envelopex"] = "Synthetic private envelope data";
    fixture.entries["Presets/Synthetic.preset"] = "Synthetic private processor data";
    fixture.write();
    const auto before = juce::SHA256(fixture.song).toHexString();
    const auto blocked = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(!blocked.succeeded() && blocked.requiresCompatibilityConfirmation
               && !fixture.destination.exists()
               && hasIssue(blocked.report, "unsupported.native-midi")
               && hasIssue(blocked.report, "unsupported.automation")
               && hasIssue(blocked.report, "unsupported.plugin-state"),
           "Private native MIDI, automation and processor state require explicit confirmation instead of silent loss.");
    if (!blocked.requiresCompatibilityConfirmation)
        return;
    const auto accepted = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination, true);
    expect(accepted.succeeded() && accepted.project.has_value()
               && !accepted.requiresCompatibilityConfirmation,
           "Explicitly accepted partial native import publishes the supported project.");
    if (!accepted.project.has_value())
        return;
    const auto* music = trackNamed(*accepted.project, "Synthetic MIDI");
    const auto* processor = trackNamed(*accepted.project, "Synthetic FX Track");
    expect(music != nullptr && music->type == studio::TrackType::instrument
               && music->midiClips.empty()
               && processor != nullptr && processor->inserts.size() == 1
               && processor->inserts.front().missing
               && processor->inserts.front().name == "Synthetic Processor"
               && accepted.project->compatibilityReports.size() == 1
               && hasIssue(accepted.project->compatibilityReports.front(), "unsupported.native-midi")
               && juce::SHA256(fixture.song).toHexString() == before,
           "Partial native import preserves missing processor descriptors and its compatibility report without inventing notes or editing the source.");
}

void invalidNativeSourcesAreTransactional()
{
    const std::vector<std::pair<juce::String, std::function<void(SongFixture&)>>> cases {
        { "unsupported.tempo-map", [](auto& fixture)
          {
              fixture.entries["Song/song.xml"] =
                  fixture.entries["Song/song.xml"].replace(
                      "</TempoMap>", "<TempoMapSegment start=\"4\" tempo=\"0.25\" curveType=\"0\"/></TempoMap>");
          } },
        { "import.audio-bounds", [](auto& fixture)
          {
              fixture.entries["Song/song.xml"] =
                  fixture.entries["Song/song.xml"].replace("offset=\"0.25\"", "offset=\"5\"");
          } },
        { "import.media-id", [](auto& fixture)
          {
              fixture.entries["Song/mediapool.xml"] =
                  fixture.entries["Song/mediapool.xml"].replace(
                      "</MediaFolder>", R"xml(<AudioClip mediaID="audio-media"><Url x:id="path" url="Media/other.wav"/></AudioClip></MediaFolder>)xml");
          } },
        { "import.xml-limits", [](auto& fixture)
          {
              juce::String nested;
              for (auto depth = 0; depth < 160; ++depth)
                  nested += "<Attributes>";
              for (auto depth = 0; depth < 160; ++depth)
                  nested += "</Attributes>";
              fixture.entries["Song/song.xml"] =
                  fixture.entries["Song/song.xml"].replace("<Song>", "<Song>" + nested);
          } },
        { "import.archive", [](auto& fixture)
          {
              fixture.entries["../outside.txt"] = "Synthetic unsafe entry";
          } },
        { "import.archive", [](auto& fixture)
          {
              fixture.entries["Song\\song.xml"] = fixture.entries["Song/song.xml"];
          } }
    };
    for (const auto& [code, modify] : cases)
    {
        SongFixture fixture;
        modify(fixture);
        fixture.write();
        const auto before = juce::SHA256(fixture.song).toHexString();
        const auto result = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination, true);
        expect(!result.succeeded() && !result.requiresCompatibilityConfirmation
                   && !result.project.has_value()
                   && !fixture.destination.exists()
                   && result.report.hasErrors()
                   && hasIssue(result.report, code)
                   && juce::SHA256(fixture.song).toHexString() == before,
               ("Invalid native source is rejected transactionally: " + code).toRawUTF8());
    }
}

void unknownNativeContentIsReported()
{
    SongFixture fixture;
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"]
            .replace("speed=\"1\"", "speed=\"1\" transpose=\"12\"")
            .replace("</MediaTrack>", R"xml(</MediaTrack>
<UnrecognizedTrack name="Synthetic Unknown"><UnrecognizedData/></UnrecognizedTrack>)xml");
    fixture.entries["Song/song.xml"] =
        fixture.entries["Song/song.xml"].replace(
            "</List>\n</MediaTrack>", "<UnrecognizedEvent name=\"Synthetic Event\"/></List>\n</MediaTrack>");
    fixture.write();
    const auto result = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(!result.succeeded() && result.requiresCompatibilityConfirmation
               && !fixture.destination.exists()
               && hasIssue(result.report, "unsupported.audio-processing")
               && hasIssue(result.report, "unsupported.event")
               && hasIssue(result.report, "unsupported.track"),
           "Unknown native tracks, events and audio processing cannot disappear silently.");
}

void sharedProjectSourceWorkflow()
{
    SongFixture native;
    native.write();
    using Format = studio::ProjectSourceFormat;
    expect(studio::ProjectImportService::sourceFormat(native.song) == Format::studioOneSong
               && studio::ProjectImportService::sourceFormat(native.song.withFileExtension("SONG")) == Format::studioOneSong
               && studio::ProjectImportService::sourceFormat(native.destination) == Format::studioDuo
               && studio::ProjectImportService::sourceFormat(native.song.withFileExtension("DAWPROJECT")) == Format::dawProject
               && !studio::ProjectImportService::supportsProjectSource(native.song.withFileExtension("project")),
           "Open, startup and drag/drop share case-insensitive project source recognition without mislabeling mastering projects.");
    const auto nativeResult = studio::ProjectImportService::importProject(
        native.song, native.destination);
    expect(nativeResult.succeeded(),
           "The shared project source workflow dispatches native .song import.");

    SongFixture interchange;
    studio::Project sourceProject;
    sourceProject.name = "Synthetic Studio One Exchange";
    studio::Track music;
    music.id = "synthetic-midi-track";
    music.name = "Synthetic Notes";
    music.type = studio::TrackType::midi;
    studio::MidiClip clip;
    clip.name = "Synthetic MIDI Clip";
    clip.startBeats = 8.0;
    clip.durationBeats = 4.0;
    studio::MidiNote note;
    note.pitch = 64;
    note.startBeats = 0.5;
    note.durationBeats = 0.25;
    note.velocity = 90;
    clip.notes.push_back(note);
    music.midiClips.push_back(clip);
    studio::Track master;
    master.id = "synthetic-master";
    master.name = "Synthetic Main";
    master.type = studio::TrackType::master;
    music.outputTrackId = master.id;
    sourceProject.tracks = { music, master };
    const auto exported = studio::DawProjectIO::exportProject(
        sourceProject, {}, interchange.root.getChildFile("Source.dawproject"));
    expect(exported.succeeded(), "Synthetic DAWproject source can be exported.");
    if (!exported.succeeded())
        return;
    studio::ProjectArchiveReader archive(exported.archive, "DAWproject");
    juce::String error;
    expect(archive.validate({ "project.xml", "metadata.xml" }).wasOk(),
           "Synthetic interchange source is a valid archive.");
    const auto xml = archive.readText("project.xml", error);
    const auto metadata = archive.readText("metadata.xml", error);
    expect(xml.has_value() && metadata.has_value(),
           "Synthetic interchange XML can be read.");
    if (!xml.has_value() || !metadata.has_value())
        return;
    interchange.entries.clear();
    interchange.entries["project.xml"] = xml->replace(
        "name=\"Studio Duo\"", "name=\"Studio One\"");
    interchange.entries["metadata.xml"] = *metadata;
    interchange.song = interchange.song.withFileExtension("dawproject");
    interchange.write();
    const auto result = studio::ProjectImportService::importProject(
        interchange.song, interchange.destination);
    expect(result.succeeded(),
           "The shared source workflow imports a Studio One DAWproject export.");
    if (!result.project.has_value())
        return;
    const auto* imported = trackNamed(*result.project, "Synthetic Notes");
    expect(imported != nullptr && imported->midiClips.size() == 1
               && imported->midiClips.front().startBeats == 8.0
               && imported->midiClips.front().notes.size() == 1
               && imported->midiClips.front().notes.front().pitch == 64
               && imported->midiClips.front().notes.front().velocity == 90,
           "Studio One interchange dispatch preserves actual editable MIDI data rather than using the limited native path.");
}

void startupImportAction()
{
    SongFixture fixture;
    studio::StudioPreferences preferences(fixture.root.getChildFile("preferences.json"));
    studio::StartupHubComponent hub(preferences);
    auto activated = false;
    hub.onImportStudioOne = [&activated] { activated = true; };
    juce::TextButton* importButton = nullptr;
    for (auto* child : hub.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child);
            button != nullptr && button->getButtonText() == "IMPORT FROM STUDIO ONE")
            importButton = button;
    expect(importButton != nullptr && importButton->isVisible() && importButton->getWantsKeyboardFocus(),
           "Startup exposes a visible, keyboard-focusable Studio One import action.");
    if (importButton == nullptr)
        return;
    for (const auto size : { juce::Point<int>(900, 700), juce::Point<int>(650, 500) })
    {
        hub.setSize(size.x, size.y);
        expect(hub.getLocalBounds().contains(importButton->getBounds())
                   && importButton->getHeight() >= 32 && importButton->getWidth() >= 160,
               "The Studio One startup action stays usable at supported compact sizes.");
    }
    importButton->onClick();
    expect(activated, "The startup import action invokes the shared Studio One workflow.");
}

void nativeStateIsNotSilentlyChanged()
{
    {
        SongFixture fixture;
        fixture.entries["Song/song.xml"] =
            fixture.entries["Song/song.xml"].replace("speed=\"1\"", "speed=\"1\" volume=\"0.5\"");
        fixture.write();
        const auto result = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination);
        expect(result.requiresCompatibilityConfirmation && !fixture.destination.exists()
                   && hasIssue(result.report, "unsupported.audio-processing"),
               "Additional native audio event attributes require confirmation rather than disappearing.");
    }
    {
        SongFixture fixture;
        fixture.entries["Song/song.xml"] =
            fixture.entries["Song/song.xml"].replace(
                "<List x:id=\"Events\">", "<UID x:id=\"channelID\" uid=\"audio-channel\"/><List x:id=\"Events\">");
        fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer><ChannelGroup>
<AudioTrackChannel label="Synthetic Audio" gain="1" mute="0" disabled="1">
<UID x:id="uniqueID" uid="audio-channel"/>
</AudioTrackChannel>
</ChannelGroup></AudioMixer>)xml";
        fixture.write();
        const auto result = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination);
        const auto* track = result.project.has_value()
            ? trackNamed(*result.project, "Synthetic Audio") : nullptr;
        expect(result.succeeded() && track != nullptr && track->muted,
               "A disabled native channel cannot become audible after import.");
    }
    {
        SongFixture fixture;
        fixture.entries["Devices/audiomixer.xml"] = R"xml(<AudioMixer><ChannelGroup>
<AudioSynthChannel label="Synthetic Synth" gain="1">
<UID x:id="uniqueID" uid="synth-channel"/>
</AudioSynthChannel>
</ChannelGroup></AudioMixer>)xml";
        fixture.write();
        const auto result = studio::StudioOneProjectIO::importSong(
            fixture.song, fixture.destination);
        expect(result.requiresCompatibilityConfirmation && !fixture.destination.exists()
                   && hasIssue(result.report, "unsupported.native-instrument"),
               "Native synth channels cannot be silently omitted.");
    }
}

void existingNativeDestinationsArePreserved()
{
    SongFixture fixture;
    fixture.write();
    expect(fixture.destination.createDirectory().wasOk(),
           "Synthetic existing destination can be created.");
    const auto empty = studio::StudioOneProjectIO::importSong(
        fixture.song, fixture.destination);
    expect(!empty.succeeded() && fixture.destination.isDirectory()
               && fixture.destination.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0
               && hasIssue(empty.report, "import.destination-exists"),
           "Native import refuses even an existing empty destination.");
    const auto sentinel = fixture.destination.getChildFile("synthetic-existing.txt");
    expect(sentinel.replaceWithText("Synthetic existing project data"),
           "Synthetic existing project data can be prepared.");
    const auto hash = juce::SHA256(sentinel).toHexString();
    const auto result = studio::ProjectImportService::importProject(
        fixture.song, fixture.destination, true);
    expect(!result.succeeded() && sentinel.existsAsFile()
               && juce::SHA256(sentinel).toHexString() == hash
               && hasIssue(result.report, "import.destination-exists"),
           "Explicit partial native import still cannot overwrite an existing project.");
}
}

void studioOneProjectTests()
{
    nativeAudioImport();
    nativeMixerImport();
    nativeTransportAndMetadata();
    relocatedNativeMedia();
    nativeChannelIdsMustBeUnique();
    nativeFolderChannelMapping();
    nativeColourLossRequiresConsent();
    repeatedNativeMediaIsReadOncePerPhase();
    cachedMediaKeepsSourceIntegrity();
    nativeCompatibilityRequiresConfirmation();
    invalidNativeSourcesAreTransactional();
    unknownNativeContentIsReported();
    sharedProjectSourceWorkflow();
    startupImportAction();
    nativeStateIsNotSilentlyChanged();
    existingNativeDestinationsArePreserved();
}
