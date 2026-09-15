#include "TestHarness.h"
#include "TestSuites.h"

#include "dawproject_io/DawProjectIdMapper.h"
#include "dawproject_io/DawProjectIO.h"
#include "dawproject_io/DawProjectSchemaValidator.h"
#include "model/ProjectModel.h"
#include "plugin_host/PluginStateStore.h"
#include "project_io/ProjectFile.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace
{
juce::File createAudioFixture(const juce::File& directory)
{
    const auto file = directory.getChildFile("fixture.wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(2)
            .withBitsPerSample(24));
    juce::AudioBuffer<float> audio(2, 4800);
    for (auto sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto value =
            static_cast<float>(std::sin(sample * 0.01) * 0.2);
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, -value);
    }
    expect(writer != nullptr
               && writer->writeFromAudioSampleBuffer(
                   audio,
                   0,
                   audio.getNumSamples()),
           "DAWproject audio fixture can be written.");
    if (writer != nullptr)
        writer->flush();
    return file;
}

juce::String archiveEntryText(const juce::File& archive,
                              const juce::String& path)
{
    juce::ZipFile zip(archive);
    const auto index = zip.getIndexOfFileName(path, false);
    if (index < 0)
        return {};
    std::unique_ptr<juce::InputStream> stream(
        zip.createStreamForEntry(index));
    return stream != nullptr ? stream->readEntireStreamAsString()
                             : juce::String();
}

bool archiveContainsPrefix(const juce::File& archive,
                           const juce::String& prefix)
{
    juce::ZipFile zip(archive);
    for (auto index = 0; index < zip.getNumEntries(); ++index)
    {
        const auto* entry = zip.getEntry(index);
        if (entry != nullptr && entry->filename.startsWith(prefix))
            return true;
    }
    return false;
}

juce::MemoryBlock archiveEntryDataWithSuffix(
    const juce::File& archive,
    const juce::String& suffix)
{
    juce::ZipFile zip(archive);
    for (auto index = 0; index < zip.getNumEntries(); ++index)
    {
        const auto* entry = zip.getEntry(index);
        if (entry == nullptr
            || !entry->filename.endsWithIgnoreCase(suffix))
            continue;
        std::unique_ptr<juce::InputStream> stream(
            zip.createStreamForEntry(index));
        juce::MemoryBlock data;
        if (stream != nullptr)
            stream->readIntoMemoryBlock(data);
        return data;
    }
    return {};
}

bool reportContains(const studio::CompatibilityReport& report,
                    const juce::String& code,
                    const juce::String& objectPathFragment)
{
    return std::any_of(
        report.issues.cbegin(),
        report.issues.cend(),
        [&code, &objectPathFragment](const auto& issue)
        {
            return issue.code == code
                && issue.objectPath.contains(objectPathFragment);
        });
}

std::uint32_t littleEndian32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(
        bytes[0]
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24));
}

bool patchCentralUncompressedSize(juce::MemoryBlock& archive,
                                  const juce::String& path,
                                  std::uint32_t size)
{
    auto* bytes = static_cast<std::uint8_t*>(archive.getData());
    const auto dataSize = archive.getSize();
    const auto expectedPath = path.toStdString();
    for (std::size_t position = 0;
         position + 46 + expectedPath.size() <= dataSize;
         ++position)
    {
        if (littleEndian32(bytes + position) != 0x02014b50u)
            continue;
        const auto nameLength = static_cast<std::uint16_t>(
            bytes[position + 28]
            | (static_cast<std::uint16_t>(bytes[position + 29])
               << 8));
        if (nameLength != expectedPath.size()
            || std::memcmp(
                   bytes + position + 46,
                   expectedPath.data(),
                   expectedPath.size())
                   != 0)
            continue;
        bytes[position + 24] =
            static_cast<std::uint8_t>(size & 0xff);
        bytes[position + 25] =
            static_cast<std::uint8_t>((size >> 8) & 0xff);
        bytes[position + 26] =
            static_cast<std::uint8_t>((size >> 16) & 0xff);
        bytes[position + 27] =
            static_cast<std::uint8_t>((size >> 24) & 0xff);
        return true;
    }
    return false;
}

void nativeSceneAndReportPersistence()
{
    auto project = studio::Project::createDefault();
    project.metadata.artist = "Fixture Artist";
    project.metadata.album = "Fixture Album";

    studio::ProjectScene scene;
    scene.id = "scene-persistence";
    scene.name = "Heavy section";
    scene.colour = juce::Colour(0xff5d7fa3);

    studio::SceneSlot slot;
    slot.id = "slot-persistence";
    slot.trackId = project.tracks.front().id;
    slot.stopTrack = true;
    studio::AudioClip sceneClip;
    sceneClip.id = "scene-audio-clip";
    sceneClip.name = "Scene audio";
    sceneClip.durationSeconds = 2.0;
    sceneClip.sourceLengthSeconds = 2.0;
    sceneClip.sourceRangeEndSeconds = 2.0;
    slot.audioClip = sceneClip;
    scene.slots.push_back(std::move(slot));
    project.scenes.push_back(std::move(scene));

    studio::CompatibilityReport report;
    report.id = "report-persistence";
    report.format = "DAWproject 1.0";
    report.operation = "import";
    report.source = "fixture.dawproject";
    report.destination = "fixture.studioduo";
    report.createdAt = "2026-01-02T03:04:05Z";
    report.issues.push_back({
        studio::CompatibilitySeverity::warning,
        "unsupported.video",
        "/Project/Arrangement/Lanes/Video[1]",
        "Video is not supported."
    });
    project.compatibilityReports.push_back(std::move(report));
    studio::PluginInsert stateDescriptor;
    stateDescriptor.id = "state-format-persistence";
    stateDescriptor.pluginIdentifier = "fixture.vst3";
    stateDescriptor.name = "State format fixture";
    stateDescriptor.format = "VST3";
    stateDescriptor.stateFormat =
        studio::PluginStateFormat::vst3Preset;
    project.tracks.front().inserts.push_back(
        std::move(stateDescriptor));

    const auto package = juce::File::getSpecialLocation(
                             juce::File::tempDirectory)
                             .getNonexistentChildFile(
                                 "StudioDuoDawProjectPersistence",
                                 ".studioduo",
                                 false);
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "Projects containing scenes and compatibility reports can be saved.");

    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value(), error.toRawUTF8());
    expect(loaded.has_value()
               && loaded->metadata.artist == "Fixture Artist"
               && loaded->metadata.album == "Fixture Album",
           "Project metadata survives native save and reopen.");
    expect(loaded.has_value()
               && loaded->scenes.size() == 1
               && loaded->scenes.front().slots.size() == 1
               && loaded->scenes.front().slots.front().stopTrack
               && loaded->scenes.front().slots.front().audioClip.has_value()
               && loaded->scenes.front().slots.front().audioClip->id
                      == "scene-audio-clip",
           "Scenes and their clip slots survive native save and reopen.");
    expect(loaded.has_value()
               && loaded->compatibilityReports.size() == 1
               && loaded->compatibilityReports.front().issues.size() == 1
               && loaded->compatibilityReports.front().issues.front().objectPath
                      == "/Project/Arrangement/Lanes/Video[1]",
           "Structured compatibility reports survive native save and reopen.");
    expect(loaded.has_value()
               && loaded->tracks.front().inserts.size() == 1
               && loaded->tracks.front().inserts.front().stateFormat
                      == studio::PluginStateFormat::vst3Preset,
           "Plugin state-container encoding survives native save and reopen.");
    package.deleteRecursively();
}

void officialSchemaValidation()
{
    const auto validProject = studio::DawProjectSchemaValidator::validateProjectXml(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<Project version="1.0">
  <Application name="Fixture DAW" version="1.0"/>
  <Transport>
    <Tempo unit="bpm" value="120"/>
    <TimeSignature numerator="4" denominator="4"/>
  </Transport>
  <Structure>
    <Track id="track-1" name="Audio" contentType="audio">
      <Channel id="channel-1" role="regular" audioChannels="2">
        <Volume id="volume-1" unit="decibel" value="0"/>
      </Channel>
    </Track>
    <Track id="track-2" name="Master" contentType="audio">
      <Channel id="channel-2" role="master" audioChannels="2"/>
    </Track>
  </Structure>
  <Arrangement>
    <Lanes timeUnit="beats">
      <Lanes track="track-1">
        <Clips>
          <Clip time="0" duration="4">
            <Notes>
              <Note time="0" duration="1" channel="1" key="60" vel="0.8"/>
            </Notes>
          </Clip>
        </Clips>
      </Lanes>
      <Points>
        <Target parameter="volume-1"/>
        <RealPoint time="0" value="0" interpolation="linear"/>
      </Points>
    </Lanes>
  </Arrangement>
</Project>)");
    expect(validProject.valid,
           validProject.summary().toRawUTF8());

    const auto wrongOrder = studio::DawProjectSchemaValidator::validateProjectXml(
        R"(<Project version="1.0"><Transport/><Application name="Fixture" version="1"/></Project>)");
    expect(!wrongOrder.valid
               && wrongOrder.summary().containsIgnoreCase("Application"),
           "Project schema validation rejects elements outside XSD sequence order.");

    const auto missingAttribute =
        studio::DawProjectSchemaValidator::validateProjectXml(
            R"(<Project version="1.0"><Application version="1"/></Project>)");
    expect(!missingAttribute.valid
               && missingAttribute.summary().containsIgnoreCase("name"),
           "Project schema validation rejects missing required attributes.");

    const auto invalidEnum = studio::DawProjectSchemaValidator::validateProjectXml(
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Structure><Track id="track-1"><Channel id="channel-1" role="unsupported"/></Track></Structure></Project>)");
    expect(!invalidEnum.valid
               && invalidEnum.summary().containsIgnoreCase("role"),
           "Project schema validation enforces official enumerations.");

    const auto unresolvedReference =
        studio::DawProjectSchemaValidator::validateProjectXml(
            R"(<Project version="1.0"><Application name="Fixture" version="1"/><Arrangement><Lanes track="missing-track"/></Arrangement></Project>)");
    expect(!unresolvedReference.valid
               && unresolvedReference.summary().containsIgnoreCase(
                   "missing-track"),
           "Project schema validation resolves official IDREF attributes.");

    const auto validMetadata =
        studio::DawProjectSchemaValidator::validateMetadataXml(
            R"(<MetaData><Title>Fixture</Title><Artist>Fixture Artist</Artist><Comment>Schema example</Comment></MetaData>)");
    expect(validMetadata.valid,
           validMetadata.summary().toRawUTF8());
    const auto invalidMetadata =
        studio::DawProjectSchemaValidator::validateMetadataXml(
            R"(<MetaData><Artist>Fixture Artist</Artist><Title>Fixture</Title></MetaData>)");
    expect(!invalidMetadata.valid
               && invalidMetadata.summary().containsIgnoreCase("Title"),
           "Metadata schema validation enforces official element order.");
    const auto concreteTimeline =
        studio::DawProjectSchemaValidator::validateProjectXml(
            R"(<Project version="1.0" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><Application name="Fixture" version="1"/><Scenes><Scene id="scene-1"><Timeline xsi:type="lanes"/></Scene></Scenes></Project>)");
    expect(concreteTimeline.valid,
           concreteTimeline.summary().toRawUTF8());

    expect(studio::DawProjectSchemaValidator::projectSchemaSha256()
                   == "794aada904501da5d2005291500402e1cd6f40fab55e0e2eadafd4d9666786a7"
               && studio::DawProjectSchemaValidator::metadataSchemaSha256()
                      == "093c820c0a500580fcea3eb7054f8371bd55a1fd8f0622622350ee7f4b351224",
           "The embedded validators use the pinned authoritative v1.0.0 schemas.");

    const auto officialExample = juce::File(STUDIO_DUO_SOURCE_DIR)
                                     .getChildFile("third_party")
                                     .getChildFile("dawproject")
                                     .getChildFile("v1.0.0")
                                     .getChildFile("examples")
                                     .getChildFile(
                                         "bitwig-readme-project.xml")
                                     .loadFileAsString();
    const auto officialValidation =
        studio::DawProjectSchemaValidator::validateProjectXml(
            officialExample);
    expect(officialValidation.valid,
           officialValidation.summary().toRawUTF8());
}

void officialCompatibilityFixtureImport()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoOfficialDawProject",
                              {},
                              false);
    expect(root.createDirectory(),
           "Official DAWproject fixture directory can be created.");
    const auto projectXml = juce::File(STUDIO_DUO_SOURCE_DIR)
                                .getChildFile("third_party")
                                .getChildFile("dawproject")
                                .getChildFile("v1.0.0")
                                .getChildFile("examples")
                                .getChildFile(
                                    "bitwig-readme-project.xml")
                                .loadFileAsString();
    const auto audio = createAudioFixture(root);
    const auto archive =
        root.getChildFile("official-example.dawproject");
    juce::ZipFile::Builder builder;
    const auto fixedTime = juce::Time(1980, 0, 1, 0, 0);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            projectXml.toRawUTF8(),
            projectXml.getNumBytesAsUTF8(),
            true),
        9,
        "project.xml",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData><Title>Official example</Title></MetaData>",
            std::strlen(
                "<MetaData><Title>Official example</Title></MetaData>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    builder.addEntry(
        audio.createInputStream(),
        9,
        "audio/Drumfunk3 170bpm.wav",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "official-clap-state",
            std::strlen("official-clap-state"),
            false),
        9,
        "plugins/d19b1f6e-bbb6-42fe-a6c9-54b41d97a05d.clap-preset",
        fixedTime);
    {
        auto output = archive.createOutputStream();
        expect(output != nullptr
                   && builder.writeToStream(*output, nullptr),
               "Official DAWproject compatibility fixture can be written.");
    }
    const auto imported = studio::DawProjectIO::importProject(
        archive,
        root.getChildFile("official-example.studioduo"));
    expect(imported.succeeded(),
           imported.result.getErrorMessage().toRawUTF8());
    if (imported.project.has_value())
    {
        const auto findTrack = [&imported](const juce::String& name)
        {
            const auto match = std::find_if(
                imported.project->tracks.cbegin(),
                imported.project->tracks.cend(),
                [&name](const auto& track)
                {
                    return track.name == name;
                });
            return match == imported.project->tracks.cend()
                ? static_cast<const studio::Track*>(nullptr)
                : &*match;
        };
        const auto* bass = findTrack("Bass");
        const auto* drumloop = findTrack("Drumloop");
        expect(bass != nullptr
                   && std::abs(bass->pan) < 0.0001
                   && std::abs(
                          bass->volumeDecibels
                          - juce::Decibels::gainToDecibels(
                              0.659140f))
                          < 0.001f,
               "Official normalized pan and linear gain convert to Studio Duo domains.");
        expect(bass != nullptr
                   && bass->type == studio::TrackType::instrument
                   && bass->inserts.size() == 1
                   && bass->inserts.front().stateFormat
                          == studio::PluginStateFormat::clapPreset
                   && !bass->inserts.front().missing,
               "Official CLAP instruments and preset state import as active devices.");
        expect(drumloop != nullptr
                   && drumloop->clips.size() == 1,
               "Official nested audio clips import as one Studio Duo audio clip.");
        const auto warpSeconds =
            drumloop != nullptr
                    && drumloop->clips.size() == 1
                    && !drumloop->clips.front().warpMarkers.empty()
                ? drumloop->clips.front()
                      .warpMarkers.back()
                      .timelineOffsetSeconds
                : -1.0;
        const auto warpMessage =
            "Inherited beat warp expected 3-4 seconds, got "
            + juce::String(warpSeconds, 6) + ".";
        expect(drumloop != nullptr
                   && drumloop->clips.size() == 1
                   && drumloop->clips.front().warpMarkers.size() == 2
                   && warpSeconds > 3.0
                   && warpSeconds < 4.0,
               warpMessage.toRawUTF8());
        expect(reportContains(
                   imported.report,
                   "unsupported.clip-loop",
                   "/Project/Arrangement"),
               "Official clip loop bounds are reported rather than silently discarded.");
    }
    root.deleteRecursively();
}

void stableIdMapping()
{
    studio::DawProjectIdMapper first;
    studio::DawProjectIdMapper second;
    const auto firstTrack =
        first.externalId("track", "8f22d785-a4b6-4f87-98bc-8fd282589247");
    const auto secondTrack =
        second.externalId("track", "8f22d785-a4b6-4f87-98bc-8fd282589247");
    const auto channel =
        first.externalId("channel", "8f22d785-a4b6-4f87-98bc-8fd282589247");
    expect(firstTrack == secondTrack
               && firstTrack != channel
               && firstTrack.startsWith("sd-")
               && !firstTrack.containsAnyOf(" :/\\"),
           "DAWproject XML IDs are deterministic, kind-specific, and NCName-safe.");

    juce::String error;
    const auto imported = first.importedId("clip", "external-clip", error);
    expect(error.isEmpty()
               && imported.isNotEmpty()
               && imported
                      == first.importedId("clip", "external-clip", error),
           "Imported DAWproject IDs map to stable internal IDs.");
    expect(first.bindImportedId(
                     "channel",
                     "external-channel",
                     "internal-track",
                     error)
               && first.internalId("channel", "external-channel")
                      == "internal-track",
           "The ID mapper preserves explicit aliases used by routing relationships.");
    expect(!first.bindImportedId(
                "channel",
                "external-channel",
                "different-track",
                error)
               && error.containsIgnoreCase("already"),
           "The ID mapper rejects conflicting relationship aliases.");
}

void completeArchiveRoundTrip()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoDawProjectRoundTrip",
                              {},
                              false);
    expect(root.createDirectory(),
           "DAWproject round-trip directory can be created.");
    const auto sourceAudio = createAudioFixture(root);
    const auto sourceAudioHash = juce::SHA256(sourceAudio).toHexString();

    auto project = studio::Project::createDefault();
    project.name = "DAWproject Fixture";
    project.metadata.artist = "Fixture Artist";
    project.metadata.album = "Fixture Album";
    project.metadata.composer = "Fixture Composer";
    project.metadata.year = "2026";
    project.metadata.genre = "Metal";
    project.metadata.website = "https://example.invalid/studio-duo";
    project.metadata.comment = "Generated compatibility fixture";
    project.tempo = 132.0;
    project.tempoChanges = {
        { 0.0, 132.0, false },
        { 2.0, 148.0, true },
        { 4.0, 160.0, false }
    };
    project.meterChanges = {
        { 0.0, 4, 4 },
        { 4.0, 7, 8 }
    };
    project.sections = {
        { "section-intro", "Intro", 0.0 },
        { "section-riff", "Riff", 4.0 }
    };
    for (auto& track : project.tracks)
        track.armed = false;

    studio::Track folder;
    folder.id = "folder-rhythm";
    folder.name = "Rhythm";
    folder.type = studio::TrackType::folder;
    folder.colour = juce::Colour(0xff34495e);
    project.tracks.insert(project.tracks.begin(), folder);
    auto* audioTrack = project.findTrack(
        project.tracks[1].id);
    auto* midiTrack = project.findTrack(
        project.tracks[2].id);
    expect(audioTrack != nullptr && midiTrack != nullptr,
           "Round-trip source tracks are available.");
    if (audioTrack == nullptr || midiTrack == nullptr)
    {
        root.deleteRecursively();
        return;
    }
    audioTrack->id = "track-audio";
    audioTrack->name = "Guitars";
    audioTrack->folderTrackId = folder.id;
    audioTrack->volumeDecibels = -3.0f;
    audioTrack->pan = -0.2f;
    midiTrack->id = "track-midi";
    midiTrack->name = "Drums";
    midiTrack->type = studio::TrackType::instrument;
    midiTrack->folderTrackId = folder.id;
    project.tracks.back().id = "track-master";
    project.tracks.back().name = "Master";
    for (auto& route : project.routingConnections)
    {
        if (route.sourceTrackId == project.tracks[1].id)
            route.sourceTrackId = audioTrack->id;
        if (route.sourceTrackId == project.tracks[2].id)
            route.sourceTrackId = midiTrack->id;
        route.destination.trackId = project.tracks.back().id;
    }
    project.routingConnections.clear();
    for (const auto* track : { audioTrack, midiTrack })
    {
        studio::RoutingConnection output;
        output.id = "route-" + track->id;
        output.name = "Main output";
        output.kind = studio::RouteKind::mainOutput;
        output.sourceTrackId = track->id;
        output.destination.trackId = project.tracks.back().id;
        project.routingConnections.push_back(std::move(output));
    }

    studio::Track aux;
    aux.id = "track-aux";
    aux.name = "Parallel";
    aux.type = studio::TrackType::aux;
    aux.volumeDecibels = -6.0f;
    project.tracks.insert(project.tracks.end() - 1, aux);
    studio::RoutingConnection auxOutput;
    auxOutput.id = "route-aux-output";
    auxOutput.name = "Aux output";
    auxOutput.kind = studio::RouteKind::mainOutput;
    auxOutput.sourceTrackId = aux.id;
    auxOutput.destination.trackId = project.tracks.back().id;
    project.routingConnections.push_back(auxOutput);
    studio::RoutingConnection send;
    send.id = "route-parallel-send";
    send.name = "Parallel send";
    send.kind = studio::RouteKind::send;
    send.tap = studio::RouteTap::preFader;
    send.sourceTrackId = audioTrack->id;
    send.destination.trackId = aux.id;
    send.gainDecibels = -9.0f;
    send.pan = 0.1f;
    project.routingConnections.push_back(send);

    studio::AudioClip audioClip;
    audioClip.id = "clip-audio";
    audioClip.name = "Guitar take";
    audioClip.sourceFile = sourceAudio;
    audioClip.startSeconds = 0.5;
    audioClip.sourceOffsetSeconds = 0.01;
    audioClip.sourceLengthSeconds = 0.1;
    audioClip.sourceRangeEndSeconds = 0.1;
    audioClip.durationSeconds = 0.08;
    audioClip.fadeInSeconds = 0.005;
    audioClip.fadeOutSeconds = 0.006;
    audioClip.warpMarkers = {
        { 0.0, 0.01 },
        { 0.04, 0.055 },
        { 0.08, 0.09 }
    };
    audioTrack->clips.push_back(audioClip);

    studio::MidiClip midiClip;
    midiClip.id = "clip-midi";
    midiClip.name = "Drum notes";
    midiClip.startBeats = 1.0;
    midiClip.durationBeats = 4.0;
    studio::MidiNote note;
    note.id = "note-kick";
    note.pitch = 36;
    note.channel = 1;
    note.startBeats = 0.0;
    note.durationBeats = 0.5;
    note.velocity = 120;
    note.releaseVelocity = 80;
    note.expressions = {
        { "expression-pressure",
          studio::MidiExpressionType::pressure,
          0.1,
          0.75,
          -1 },
        { "expression-pitch",
          studio::MidiExpressionType::pitchBend,
          0.25,
          0.1,
          -1 },
        { "expression-controller",
          studio::MidiExpressionType::controller,
          0.4,
          0.5,
          74 }
    };
    midiClip.notes.push_back(note);
    midiTrack->midiClips.push_back(midiClip);

    studio::PluginInsert insert;
    insert.id = "device-clap";
    insert.pluginIdentifier = "dev.studioduo.fixture";
    insert.name = "Fixture Device";
    insert.manufacturer = "Studio Duo";
    insert.format = "CLAP";
    insert.version = "1.0";
    midiTrack->inserts.push_back(insert);
    studio::PluginInsert bundledInsert;
    bundledInsert.id = "device-bundled";
    bundledInsert.pluginIdentifier = "studio.device.eq";
    bundledInsert.name = "Parametric EQ";
    bundledInsert.manufacturer = "Studio Duo";
    bundledInsert.format = "Studio Duo";
    bundledInsert.version = "1.0";
    bundledInsert.bundledDevice = true;
    bundledInsert.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    audioTrack->inserts.push_back(bundledInsert);

    studio::AutomationLane volumeAutomation;
    volumeAutomation.id = "automation-volume";
    volumeAutomation.name = "Guitar volume";
    volumeAutomation.target.type =
        studio::AutomationTargetType::trackVolume;
    volumeAutomation.target.trackId = audioTrack->id;
    volumeAutomation.timebase = studio::AutomationTimebase::beats;
    volumeAutomation.points = {
        { "automation-volume-1", 0.0, -3.0 },
        { "automation-volume-2", 4.0, -1.0 }
    };
    project.automationLanes.push_back(volumeAutomation);
    studio::AutomationLane deviceAutomation;
    deviceAutomation.id = "automation-device";
    deviceAutomation.name = "Fixture gain";
    deviceAutomation.target.type =
        studio::AutomationTargetType::pluginParameter;
    deviceAutomation.target.trackId = midiTrack->id;
    deviceAutomation.target.insertId = insert.id;
    deviceAutomation.target.parameterId = "gain";
    deviceAutomation.target.parameterIndex = 7;
    deviceAutomation.points = {
        { "automation-device-1", 0.0, 0.25 },
        { "automation-device-2", 2.0, 0.75 }
    };
    project.automationLanes.push_back(deviceAutomation);

    studio::ProjectScene scene;
    scene.id = "scene-breakdown";
    scene.name = "Breakdown";
    studio::SceneSlot audioSlot;
    audioSlot.id = "scene-slot-audio";
    audioSlot.trackId = audioTrack->id;
    audioSlot.audioClip = audioClip;
    studio::SceneSlot midiSlot;
    midiSlot.id = "scene-slot-midi";
    midiSlot.trackId = midiTrack->id;
    midiSlot.stopTrack = true;
    midiSlot.midiClip = midiClip;
    scene.slots = { audioSlot, midiSlot };
    project.scenes.push_back(scene);

    const auto sourcePackage = root.getChildFile("source.studioduo");
    expect(sourcePackage.createDirectory(),
           "DAWproject source package can be created.");
    const juce::MemoryBlock pluginState(
        "fixture-plugin-state",
        20);
    juce::String stateError;
    const auto stateReference = studio::PluginStateStore::store(
        sourcePackage,
        pluginState,
        stateError);
    expect(stateReference.has_value(), stateError.toRawUTF8());
    if (stateReference.has_value())
    {
        midiTrack->inserts.front().stateFile =
            stateReference->relativePath;
        midiTrack->inserts.front().stateHash = stateReference->hash;
        midiTrack->inserts.front().stateFormat =
            studio::PluginStateFormat::clapPreset;
        audioTrack->inserts.front().stateFile =
            stateReference->relativePath;
        audioTrack->inserts.front().stateHash = stateReference->hash;
    }
    expect(studio::ProjectFile::save(project, sourcePackage).wasOk(),
           "DAWproject source native package can be saved.");

    const auto archive = root.getChildFile("fixture.dawproject");
    const auto exported = studio::DawProjectIO::exportProject(
        project,
        sourcePackage,
        archive);
    expect(exported.succeeded(),
           exported.result.getErrorMessage().toRawUTF8());
    expect(exported.succeeded()
               && !exported.report.hasErrors()
               && archive.existsAsFile(),
           "Complete supported project exports to a DAWproject archive.");
    const auto projectXml = archiveEntryText(archive, "project.xml");
    const auto metadataXml = archiveEntryText(archive, "metadata.xml");
    expect(studio::DawProjectSchemaValidator::validateProjectXml(projectXml)
               .valid
               && studio::DawProjectSchemaValidator::validateMetadataXml(
                      metadataXml)
                      .valid,
           "Exported DAWproject XML validates against both official schemas.");
    expect(projectXml.contains("<TempoAutomation")
               && projectXml.contains("<TimeSignatureAutomation")
               && projectXml.contains("<Scene")
               && projectXml.contains("<Warps")
               && projectXml.contains("<ClapPlugin")
               && projectXml.contains("<BuiltinDevice")
               && projectXml.contains("<Points")
               && projectXml.contains("expression=\"pressure\"")
               && archiveContainsPrefix(archive, "media/")
               && archiveContainsPrefix(archive, "plugin-state/"),
           "Export includes transport maps, scenes, warps, devices, automation, note expressions, media, and plugin state.");

    const auto secondArchive =
        root.getChildFile("fixture-second.dawproject");
    const auto secondExport = studio::DawProjectIO::exportProject(
        project,
        sourcePackage,
        secondArchive);
    expect(secondExport.succeeded()
               && juce::SHA256(archive).toHexString()
                      == juce::SHA256(secondArchive).toHexString(),
           "Repeated DAWproject exports are byte deterministic.");
    expect(juce::SHA256(sourceAudio).toHexString() == sourceAudioHash,
           "DAWproject export never mutates source media.");

    const auto sourceArchiveHash = juce::SHA256(archive).toHexString();
    const auto importedPackage =
        root.getChildFile("imported.studioduo");
    const auto imported = studio::DawProjectIO::importProject(
        archive,
        importedPackage);
    expect(imported.succeeded(),
           imported.result.getErrorMessage().toRawUTF8());
    expect(imported.succeeded()
               && imported.project.has_value()
               && imported.project->name == project.name
               && imported.project->metadata.artist
                      == project.metadata.artist
               && imported.project->scenes.size() == 1
               && imported.project->automationLanes.size() >= 2,
           "DAWproject import creates a complete new native project model.");
    if (imported.project.has_value())
    {
        const auto findTrackByName =
            [&imported](const juce::String& name)
        {
            const auto match = std::find_if(
                imported.project->tracks.cbegin(),
                imported.project->tracks.cend(),
                [&name](const auto& track)
                {
                    return track.name == name;
                });
            return match == imported.project->tracks.cend()
                ? static_cast<const studio::Track*>(nullptr)
                : &*match;
        };
        const auto* importedFolder = findTrackByName("Rhythm");
        const auto* importedAudio = findTrackByName("Guitars");
        const auto* importedMidi = findTrackByName("Drums");
        const auto* importedAux = findTrackByName("Parallel");
        const auto* importedMaster = findTrackByName("Master");
        expect(importedFolder != nullptr
                   && importedAudio != nullptr
                   && importedMidi != nullptr
                   && importedAux != nullptr
                   && importedMaster != nullptr
                   && importedAudio->folderTrackId
                          == importedFolder->id
                   && importedMidi->folderTrackId
                          == importedFolder->id
                   && importedMidi->type
                          == studio::TrackType::instrument,
               "Track roles and hierarchy survive DAWproject import.");
        expect(importedAudio != nullptr
                   && importedAudio->clips.size() == 1
                   && importedAudio->clips.front().warpMarkers.size()
                          >= 3
                   && std::abs(
                          importedAudio->clips.front().fadeInSeconds
                          - audioClip.fadeInSeconds)
                          < 0.0001
                   && importedMidi != nullptr
                   && importedMidi->midiClips.size() == 1
                   && importedMidi->midiClips.front().notes.size() == 1
                   && importedMidi->midiClips.front()
                          .notes.front()
                          .expressions.size()
                          == 3,
               "Audio warps/fades and MIDI notes/expressions survive import.");
        expect(importedMidi != nullptr
                   && importedMidi->inserts.size() == 1
                   && importedMidi->inserts.front().format
                          .containsIgnoreCase("CLAP")
                   && std::count_if(
                          imported.project->routingConnections.cbegin(),
                          imported.project->routingConnections.cend(),
                          [](const auto& route)
                          {
                              return route.kind
                                  == studio::RouteKind::mainOutput;
                          })
                          == 3
                   && std::count_if(
                          imported.project->routingConnections.cbegin(),
                          imported.project->routingConnections.cend(),
                          [](const auto& route)
                          {
                              return route.kind
                                  == studio::RouteKind::send;
                          })
                          == 1
                   && imported.project->tempoChanges.size() == 3
                   && imported.project->meterChanges.size() == 2,
               "Devices, routing, tempo, and meter maps survive DAWproject import.");
        expect(importedAudio != nullptr
                   && importedAudio->inserts.size() == 1
                   && importedAudio->inserts.front().bundledDevice
                   && !importedAudio->inserts.front().missing
                   && importedAudio->inserts.front().pluginIdentifier
                          == "studio.device.eq",
               "Studio Duo bundled devices re-import as active bundled devices.");
        expect(imported.project->scenes.front().slots.size() == 2
                   && imported.project->sections.size() == 2,
               "Scenes and arrangement markers survive DAWproject import.");
    }
    expect(imported.succeeded()
               && imported.project.has_value()
               && std::any_of(
                   imported.project->tracks.cbegin(),
                   imported.project->tracks.cend(),
                   [](const auto& track)
                   {
                       return !track.clips.empty()
                           && track.clips.front().sourceFile.existsAsFile();
                   })
               && std::any_of(
                   imported.project->tracks.cbegin(),
                   imported.project->tracks.cend(),
                   [&importedPackage](const auto& track)
                   {
                       return std::any_of(
                           track.inserts.cbegin(),
                           track.inserts.cend(),
                           [&importedPackage](const auto& importedInsert)
                           {
                               if (importedInsert.stateFile.isEmpty())
                                   return false;
                               juce::MemoryBlock state;
                               juce::String error;
                               return studio::PluginStateStore::load(
                                   importedPackage,
                                   { importedInsert.stateFile,
                                     importedInsert.stateHash },
                                   state,
                                   error)
                                   && state
                                          == juce::MemoryBlock(
                                              "fixture-plugin-state",
                                              20);
                           });
                   }),
           "Imported embedded media and plugin state are materialized in the native package.");
    expect(juce::SHA256(archive).toHexString() == sourceArchiveHash,
           "DAWproject import never mutates the source archive.");

    juce::String reopenError;
    const auto reopened =
        studio::ProjectFile::load(importedPackage, reopenError);
    expect(reopened.has_value(), reopenError.toRawUTF8());
    expect(reopened.has_value()
               && reopened->scenes.size() == 1
               && reopened->compatibilityReports.size() == 1,
           "Imported scenes and compatibility report survive native reopen.");

    const auto secondImportedPackage =
        root.getChildFile("imported-second.studioduo");
    const auto secondImport = studio::DawProjectIO::importProject(
        archive,
        secondImportedPackage);
    expect(imported.project.has_value()
               && secondImport.project.has_value()
               && imported.project->tracks.front().id
                      == secondImport.project->tracks.front().id
               && imported.project->scenes.front().id
                      == secondImport.project->scenes.front().id,
           "Repeated imports preserve deterministic internal relationships and IDs.");
    root.deleteRecursively();
}

void invalidArchiveIsTransactional()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoDawProjectInvalid",
                              {},
                              false);
    expect(root.createDirectory(),
           "Invalid DAWproject test directory can be created.");
    const auto archive = root.getChildFile("invalid.dawproject");
    juce::ZipFile::Builder builder;
    const auto fixedTime = juce::Time(1980, 0, 1, 0, 0);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            R"(<Project version="1.0"><Transport/></Project>)",
            std::strlen(
                R"(<Project version="1.0"><Transport/></Project>)"),
            false),
        9,
        "project.xml",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData/>",
            std::strlen("<MetaData/>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    {
        auto output = archive.createOutputStream();
        expect(output != nullptr
                   && builder.writeToStream(*output, nullptr),
               "Invalid DAWproject fixture can be written.");
    }

    const auto destination = root.getChildFile("existing.studioduo");
    expect(destination.createDirectory()
               && destination.getChildFile("sentinel.txt")
                      .replaceWithText("untouched"),
           "Transactional import destination fixture can be created.");
    const auto sourceHash = juce::SHA256(archive).toHexString();
    const auto imported =
        studio::DawProjectIO::importProject(archive, destination);
    expect(!imported.succeeded()
               && imported.result.getErrorMessage().containsIgnoreCase(
                   "schema")
               && destination.getChildFile("sentinel.txt")
                      .loadFileAsString()
                      == "untouched"
               && !destination.getChildFile("manifest.json").exists()
               && juce::SHA256(archive).toHexString() == sourceHash,
           "Invalid imports fail transactionally without touching source or destination.");

    const auto partialDestination =
        root.getChildFile("partial.studioduo");
    const auto missingMetadata =
        root.getChildFile("missing-metadata.dawproject");
    juce::ZipFile::Builder missingBuilder;
    missingBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            R"(<Project version="1.0"><Application name="Fixture" version="1"/></Project>)",
            std::strlen(
                R"(<Project version="1.0"><Application name="Fixture" version="1"/></Project>)"),
            false),
        9,
        "project.xml",
        fixedTime);
    {
        auto output = missingMetadata.createOutputStream();
        expect(output != nullptr
                   && missingBuilder.writeToStream(*output, nullptr),
               "Missing metadata fixture can be written.");
    }
    const auto missingResult = studio::DawProjectIO::importProject(
        missingMetadata,
        partialDestination);
    expect(!missingResult.succeeded()
               && !partialDestination.exists(),
           "Missing required archive entries do not leave partial destinations.");

    const auto semanticArchive =
        root.getChildFile("semantic-invalid.dawproject");
    juce::ZipFile::Builder semanticBuilder;
    const auto semanticProject =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Transport><Tempo id="tempo" unit="bpm" value="120"/></Transport><Arrangement><TempoAutomation><Target parameter="tempo"/><RealPoint time="not-a-number" value="120"/></TempoAutomation></Arrangement></Project>)";
    semanticBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            semanticProject,
            std::strlen(semanticProject),
            false),
        9,
        "project.xml",
        fixedTime);
    semanticBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData/>",
            std::strlen("<MetaData/>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    {
        auto output = semanticArchive.createOutputStream();
        expect(output != nullptr
                   && semanticBuilder.writeToStream(*output, nullptr),
               "Semantic-invalid fixture can be written.");
    }
    const auto semanticResult = studio::DawProjectIO::importProject(
        semanticArchive,
        root.getChildFile("semantic-invalid.studioduo"));
    expect(!semanticResult.succeeded()
               && semanticResult.report.hasErrors()
               && reportContains(
                   semanticResult.report,
                   "semantic.number",
                   "RealPoint"),
           "String-typed DAWproject numbers receive semantic validation beyond the XSD.");

    const auto tempoArchive =
        root.getChildFile("tempo-invalid.dawproject");
    juce::ZipFile::Builder tempoBuilder;
    const auto tempoProject =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Transport><Tempo id="tempo" unit="bpm" value="120"/></Transport><Arrangement><TempoAutomation><Target parameter="tempo"/><RealPoint time="0" value="600"/></TempoAutomation></Arrangement></Project>)";
    tempoBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            tempoProject,
            std::strlen(tempoProject),
            false),
        9,
        "project.xml",
        fixedTime);
    tempoBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData/>",
            std::strlen("<MetaData/>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    {
        auto output = tempoArchive.createOutputStream();
        expect(output != nullptr
                   && tempoBuilder.writeToStream(*output, nullptr),
               "Out-of-range tempo fixture can be written.");
    }
    const auto tempoResult = studio::DawProjectIO::importProject(
        tempoArchive,
        root.getChildFile("tempo-invalid.studioduo"));
    expect(!tempoResult.succeeded()
               && reportContains(
                   tempoResult.report,
                   "semantic.tempo",
                   "TempoAutomation"),
           "Out-of-range tempo automation is rejected rather than clamped.");

    const auto traversalArchive =
        root.getChildFile("path-traversal.dawproject");
    juce::ZipFile::Builder traversalBuilder;
    const auto validProject =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/></Project>)";
    traversalBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            validProject,
            std::strlen(validProject),
            false),
        9,
        "project.xml",
        fixedTime);
    traversalBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData/>",
            std::strlen("<MetaData/>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    traversalBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "escape",
            std::strlen("escape"),
            false),
        9,
        "../escape.bin",
        fixedTime);
    {
        auto output = traversalArchive.createOutputStream();
        expect(output != nullptr
                   && traversalBuilder.writeToStream(*output, nullptr),
               "Traversal fixture can be written.");
    }
    const auto traversalResult =
        studio::DawProjectIO::importProject(
            traversalArchive,
            root.getChildFile("path-traversal.studioduo"));
    expect(!traversalResult.succeeded()
               && traversalResult.result.getErrorMessage()
                      .containsIgnoreCase("unsafe path"),
           "Unsafe ZIP paths are rejected before extraction.");

    const auto payloadProject =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Structure><Track id="audio-track" name="Audio" contentType="audio"><Channel id="audio-channel" destination="master-channel" role="regular" audioChannels="2"/></Track><Track id="master-track" name="Master" contentType="audio"><Channel id="master-channel" role="master" audioChannels="2"/></Track></Structure><Arrangement><Lanes><Lanes track="audio-track"><Clips timeUnit="seconds"><Clip time="0" duration="1" contentTimeUnit="seconds"><Audio duration="1" channels="2" sampleRate="48000"><File path="media/payload.bin"/></Audio></Clip></Clips></Lanes></Lanes></Arrangement></Project>)";
    const auto writePayloadArchive =
        [&root, fixedTime, payloadProject](
            const juce::String& name)
    {
        const auto outputFile =
            root.getChildFile(name + ".dawproject");
        juce::ZipFile::Builder payloadBuilder;
        payloadBuilder.addEntry(
            std::make_unique<juce::MemoryInputStream>(
                payloadProject,
                std::strlen(payloadProject),
                false),
            0,
            "project.xml",
            fixedTime);
        payloadBuilder.addEntry(
            std::make_unique<juce::MemoryInputStream>(
                "<MetaData/>",
                std::strlen("<MetaData/>"),
                false),
            0,
            "metadata.xml",
            fixedTime);
        payloadBuilder.addEntry(
            std::make_unique<juce::MemoryInputStream>(
                "payload-crc-data",
                std::strlen("payload-crc-data"),
                false),
            0,
            "media/payload.bin",
            fixedTime);
        auto output = outputFile.createOutputStream();
        expect(output != nullptr
                   && payloadBuilder.writeToStream(*output, nullptr),
               "Bounded ZIP fixture can be written.");
        return outputFile;
    };

    const auto crcArchive = writePayloadArchive("crc-invalid");
    juce::MemoryBlock crcBytes;
    expect(crcArchive.loadFileAsData(crcBytes),
           "CRC fixture can be loaded.");
    const auto payloadText = std::string("payload-crc-data");
    auto* crcBegin = static_cast<std::uint8_t*>(crcBytes.getData());
    const auto payloadPosition = std::search(
        crcBegin,
        crcBegin + crcBytes.getSize(),
        payloadText.begin(),
        payloadText.end());
    expect(payloadPosition != crcBegin + crcBytes.getSize(),
           "CRC fixture payload can be located.");
    if (payloadPosition != crcBegin + crcBytes.getSize())
        *payloadPosition ^= 0x01;
    expect(crcArchive.replaceWithData(
               crcBytes.getData(),
               crcBytes.getSize()),
           "CRC fixture can be corrupted.");
    const auto crcResult = studio::DawProjectIO::importProject(
        crcArchive,
        root.getChildFile("crc-invalid.studioduo"));
    expect(!crcResult.succeeded()
               && crcResult.result.getErrorMessage()
                      .containsIgnoreCase("CRC"),
           "Corrupt archive payloads fail CRC validation before publication.");

    const auto expandedArchive =
        writePayloadArchive("expanded-invalid");
    juce::MemoryBlock expandedBytes;
    expect(expandedArchive.loadFileAsData(expandedBytes)
               && patchCentralUncompressedSize(
                   expandedBytes,
                   "media/payload.bin",
                   4)
               && expandedArchive.replaceWithData(
                   expandedBytes.getData(),
                   expandedBytes.getSize()),
           "Forged expansion fixture can be prepared.");
    const auto expandedResult =
        studio::DawProjectIO::importProject(
            expandedArchive,
            root.getChildFile("expanded-invalid.studioduo"));
    expect(!expandedResult.succeeded()
               && expandedResult.result.getErrorMessage()
                      .containsIgnoreCase("declared size"),
           "Entries expanding beyond declared sizes are stopped before publication.");

    const auto invalidAudioArchive =
        writePayloadArchive("invalid-audio");
    const auto invalidAudioResult =
        studio::DawProjectIO::importProject(
            invalidAudioArchive,
            root.getChildFile("invalid-audio.studioduo"));
    expect(!invalidAudioResult.succeeded()
               && reportContains(
                   invalidAudioResult.report,
                   "import.media-invalid",
                   "/Project/Arrangement"),
           "Schema-valid but undecodable audio payloads fail transactionally.");

    const auto existingArchive =
        writePayloadArchive("existing-destination");
    const auto existingDestination =
        root.getChildFile("already-there.studioduo");
    expect(existingDestination.createDirectory()
               && existingDestination.getChildFile("sentinel.txt")
                      .replaceWithText("preserved"),
           "Existing valid destination fixture can be created.");
    const auto existingResult =
        studio::DawProjectIO::importProject(
            existingArchive,
            existingDestination);
    expect(!existingResult.succeeded()
               && existingResult.result.getErrorMessage()
                      .containsIgnoreCase("already exists")
               && existingDestination.getChildFile("sentinel.txt")
                      .loadFileAsString()
                      == "preserved",
           "Import publication never replaces an existing native project directory.");
    root.deleteRecursively();
}

void externalMediaImport()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoDawProjectExternalMedia",
                              {},
                              false);
    expect(root.createDirectory(),
           "External-media test directory can be created.");
    const auto externalAudio = createAudioFixture(root);
    const auto sourceHash =
        juce::SHA256(externalAudio).toHexString();
    const auto projectXml =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Structure><Track id="audio-track" name="Audio" contentType="audio"><Channel id="audio-channel" destination="master-channel" role="regular" audioChannels="2"/></Track><Track id="master-track" name="Master" contentType="audio"><Channel id="master-channel" role="master" audioChannels="2"/></Track></Structure><Arrangement><Lanes><Lanes track="audio-track"><Clips timeUnit="seconds"><Clip time="0" duration="0.1" contentTimeUnit="seconds" playStart="0" playStop="0.1"><Audio duration="0.1" channels="2" sampleRate="48000"><File path="fixture.wav" external="true"/></Audio></Clip></Clips></Lanes></Lanes></Arrangement></Project>)";
    const auto archive =
        root.getChildFile("external-media.dawproject");
    juce::ZipFile::Builder builder;
    const auto fixedTime = juce::Time(1980, 0, 1, 0, 0);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            projectXml,
            std::strlen(projectXml),
            false),
        9,
        "project.xml",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData><Title>External media</Title></MetaData>",
            std::strlen(
                "<MetaData><Title>External media</Title></MetaData>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    {
        auto output = archive.createOutputStream();
        expect(output != nullptr
                   && builder.writeToStream(*output, nullptr),
               "External-media DAWproject can be written.");
    }
    const auto destination =
        root.getChildFile("external-media.studioduo");
    const auto imported =
        studio::DawProjectIO::importProject(archive, destination);
    expect(imported.succeeded()
               && imported.project.has_value()
               && std::any_of(
                   imported.project->tracks.cbegin(),
                   imported.project->tracks.cend(),
                   [](const auto& track)
                   {
                       return !track.clips.empty()
                           && track.clips.front().sourceFile
                                  .existsAsFile();
                   })
               && juce::SHA256(externalAudio).toHexString()
                      == sourceHash,
           "Relative external media is copied into the new native project without mutating the source.");
    externalAudio.deleteFile();
    expect(imported.project.has_value()
               && std::any_of(
                   imported.project->tracks.cbegin(),
                   imported.project->tracks.cend(),
                   [](const auto& track)
                   {
                       return !track.clips.empty()
                           && track.clips.front().sourceFile
                                  .existsAsFile();
                   }),
           "Imported external media remains self-contained after the original is removed.");
    root.deleteRecursively();
}

void unitAndChannelHierarchyImport()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoDawProjectUnits",
                              {},
                              false);
    expect(root.createDirectory(),
           "DAWproject unit test directory can be created.");
    const auto projectXml =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Structure><Track id="group-track" name="Group" contentType="audio tracks"><Channel id="group-channel" destination="master-channel" role="submix" audioChannels="2"/><Track id="child-track" name="Child" contentType="audio automation"><Channel id="child-channel" destination="group-channel" role="regular" audioChannels="2"><Pan id="child-pan" unit="normalized" min="0" max="1" value="0.5"/><Volume id="child-volume" unit="linear" min="0" max="2" value="0.5"/></Channel></Track></Track><Track id="master-track" name="Master" contentType="audio"><Channel id="master-channel" role="master" audioChannels="2"/></Track></Structure><Arrangement><Lanes timeUnit="beats"><Points track="child-track" unit="normalized"><Target parameter="child-pan"/><RealPoint time="0" value="0.75" interpolation="linear"/></Points><Points track="child-track" unit="linear"><Target parameter="child-volume"/><RealPoint time="0" value="0.25" interpolation="linear"/></Points></Lanes></Arrangement></Project>)";
    const auto archive = root.getChildFile("units.dawproject");
    juce::ZipFile::Builder builder;
    const auto fixedTime = juce::Time(1980, 0, 1, 0, 0);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            projectXml,
            std::strlen(projectXml),
            false),
        9,
        "project.xml",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData/>",
            std::strlen("<MetaData/>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    {
        auto output = archive.createOutputStream();
        expect(output != nullptr
                   && builder.writeToStream(*output, nullptr),
               "DAWproject unit fixture can be written.");
    }
    const auto imported = studio::DawProjectIO::importProject(
        archive,
        root.getChildFile("units.studioduo"));
    expect(imported.succeeded(),
           imported.result.getErrorMessage().toRawUTF8());
    if (imported.project.has_value())
    {
        const auto child = std::find_if(
            imported.project->tracks.cbegin(),
            imported.project->tracks.cend(),
            [](const auto& track)
            {
                return track.name == "Child";
            });
        expect(child != imported.project->tracks.cend()
                   && child->folderTrackId.isEmpty()
                   && std::abs(child->pan) < 0.0001f
                   && std::abs(
                          child->volumeDecibels
                          - juce::Decibels::gainToDecibels(0.5f))
                          < 0.001f,
               "Normalized pan, linear gain, and channel-bearing hierarchy import correctly.");
        const auto panLane = std::find_if(
            imported.project->automationLanes.cbegin(),
            imported.project->automationLanes.cend(),
            [](const auto& lane)
            {
                return lane.target.type
                    == studio::AutomationTargetType::trackPan;
            });
        const auto volumeLane = std::find_if(
            imported.project->automationLanes.cbegin(),
            imported.project->automationLanes.cend(),
            [](const auto& lane)
            {
                return lane.target.type
                    == studio::AutomationTargetType::trackVolume;
            });
        expect(panLane != imported.project->automationLanes.cend()
                   && !panLane->points.empty()
                   && std::abs(panLane->points.front().value - 0.5)
                          < 0.0001
                   && volumeLane
                          != imported.project->automationLanes.cend()
                   && !volumeLane->points.empty()
                   && std::abs(
                          volumeLane->points.front().value
                          - juce::Decibels::gainToDecibels(0.25))
                          < 0.001,
               "Automation point units convert through the target parameter domain.");
        expect(reportContains(
                   imported.report,
                   "unsupported.channel-track-hierarchy",
                   "/Project/Structure/Track"),
               "Channel-bearing track groups are flattened with an object-specific report.");
    }
    root.deleteRecursively();
}

void nativePluginStateContainerRoundTrip()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoDawProjectNativeState",
                              {},
                              false);
    expect(root.createDirectory(),
           "Native-state test directory can be created.");
    const auto projectXml =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Structure><Track id="plugin-track" name="Plugin" contentType="audio"><Channel id="plugin-channel" destination="master-channel" role="regular" audioChannels="2"><Devices><Vst3Plugin id="vst3-device" deviceID="01234567-89ab-cdef-0123-456789abcdef" deviceName="Fixture VST3" deviceRole="audioFX" loaded="true"><State path="plugin-state/fixture.vstpreset"/></Vst3Plugin></Devices></Channel></Track><Track id="master-track" name="Master" contentType="audio"><Channel id="master-channel" role="master" audioChannels="2"/></Track></Structure></Project>)";
    const auto stateText = std::string("native-vst3-preset-container");
    const auto sourceArchive =
        root.getChildFile("native-state.dawproject");
    juce::ZipFile::Builder builder;
    const auto fixedTime = juce::Time(1980, 0, 1, 0, 0);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            projectXml,
            std::strlen(projectXml),
            false),
        9,
        "project.xml",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData/>",
            std::strlen("<MetaData/>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            stateText.data(),
            stateText.size(),
            false),
        9,
        "plugin-state/fixture.vstpreset",
        fixedTime);
    {
        auto output = sourceArchive.createOutputStream();
        expect(output != nullptr
                   && builder.writeToStream(*output, nullptr),
               "Native-state DAWproject can be written.");
    }
    const auto nativePackage =
        root.getChildFile("native-state.studioduo");
    const auto imported =
        studio::DawProjectIO::importProject(
            sourceArchive,
            nativePackage);
    expect(imported.succeeded()
               && imported.project.has_value()
               && imported.project->tracks.front().inserts.size() == 1
               && imported.project->tracks.front()
                      .inserts.front()
                      .stateFormat
                      == studio::PluginStateFormat::vst3Preset
               && imported.project->tracks.front()
                      .inserts.front()
                      .missing
               && reportContains(
                   imported.report,
                   "compatibility.device-state-container",
                   "/State"),
           "Native VST3 preset containers are preserved as safe missing placeholders.");
    if (imported.project.has_value())
    {
        const auto exported =
            studio::DawProjectIO::exportProject(
                *imported.project,
                nativePackage,
                root.getChildFile("native-state-roundtrip.dawproject"));
        const auto roundTrippedState =
            archiveEntryDataWithSuffix(
                exported.archive,
                ".vstpreset");
        expect(exported.succeeded()
                   && roundTrippedState
                          == juce::MemoryBlock(
                              stateText.data(),
                              stateText.size()),
               "Native VST3 preset containers re-export byte-for-byte.");
    }
    root.deleteRecursively();
}

void unsupportedDataIsReported()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoDawProjectReport",
                              {},
                              false);
    expect(root.createDirectory(),
           "DAWproject report directory can be created.");
    auto project = studio::Project::createDefault();
    for (auto& track : project.tracks)
        track.armed = false;
    auto& track = project.tracks.front();
    const auto unsupportedTrackId = track.id;
    studio::AudioClip clip;
    clip.id = "unsupported-clip";
    clip.name = "Unsupported clip";
    clip.sourceFile = createAudioFixture(root);
    clip.durationSeconds = 0.05;
    clip.sourceLengthSeconds = 0.1;
    clip.sourceRangeEndSeconds = 0.1;
    clip.reversed = true;
    clip.polarityInverted = true;
    clip.fadeInCurve = 0.5f;
    track.clips.push_back(clip);
    track.inputMonitoring = true;
    track.polarityInverted = true;

    studio::RoutingConnection hardware;
    hardware.id = "unsupported-route";
    hardware.name = "Hardware";
    hardware.kind = studio::RouteKind::hardwareOutput;
    hardware.sourceTrackId = track.id;
    hardware.destination.type =
        studio::RouteEndpointType::hardwareOutput;
    hardware.destination.firstChannel = 4;
    project.routingConnections.push_back(hardware);

    studio::ReampRoute reamp;
    reamp.id = "unsupported-reamp";
    reamp.name = "Reamp";
    reamp.sourceTrackId = project.tracks[0].id;
    reamp.returnTrackId = project.tracks[1].id;
    project.reampRoutes.push_back(reamp);

    studio::Track midiTrack;
    midiTrack.id = "unsupported-midi-track";
    midiTrack.name = "Unsupported MIDI track";
    midiTrack.type = studio::TrackType::midi;
    studio::MidiClip midiClip;
    midiClip.id = "unsupported-midi-clip";
    midiClip.name = "Unsupported MIDI";
    midiClip.editorMode = studio::MidiEditorMode::drums;
    midiClip.drumMapId = project.drumMaps.front().id;
    midiClip.humanizeSeed = 42;
    midiClip.humanizeTimingTicks = 12;
    midiClip.humanizeVelocity = 8;
    studio::MidiNote midiNote;
    midiNote.id = "unsupported-midi-note";
    midiNote.pitch = project.drumMaps.front().entries.front().noteNumber;
    midiNote.durationBeats = 0.25;
    midiNote.probability = 0.5;
    midiNote.drumMapEntryId =
        project.drumMaps.front().entries.front().id;
    midiNote.articulation = "fixture";
    midiClip.notes.push_back(midiNote);
    midiTrack.midiClips.push_back(midiClip);
    project.tracks.push_back(std::move(midiTrack));

    studio::AutomationLane unsupportedAutomation;
    unsupportedAutomation.id = "unsupported-automation";
    unsupportedAutomation.name = "Polarity";
    unsupportedAutomation.target.type =
        studio::AutomationTargetType::trackPolarity;
    unsupportedAutomation.target.trackId = unsupportedTrackId;
    unsupportedAutomation.points.push_back({
        "unsupported-automation-point",
        0.0,
        1.0
    });
    project.automationLanes.push_back(unsupportedAutomation);

    const auto package = root.getChildFile("source.studioduo");
    expect(package.createDirectory(),
           "Unsupported-data source package can be created.");
    studio::PluginInsert vstInsert;
    vstInsert.id = "opaque-vst-state";
    vstInsert.pluginIdentifier = "fixture-vst3";
    vstInsert.name = "Opaque VST3";
    vstInsert.format = "VST3";
    juce::String stateError;
    const auto vstState = studio::PluginStateStore::store(
        package,
        juce::MemoryBlock("opaque-vst-state", 16),
        stateError);
    expect(vstState.has_value(), stateError.toRawUTF8());
    if (vstState.has_value())
    {
        vstInsert.stateFile = vstState->relativePath;
        vstInsert.stateHash = vstState->hash;
    }
    project.findTrack(unsupportedTrackId)
        ->inserts.push_back(std::move(vstInsert));
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "Unsupported-data source project can be saved.");
    const auto result = studio::DawProjectIO::exportProject(
        project,
        package,
        root.getChildFile("reported.dawproject"));
    expect(result.succeeded(),
           result.result.getErrorMessage().toRawUTF8());
    expect(reportContains(
               result.report,
               "unsupported.track-input-monitoring",
               unsupportedTrackId)
               && reportContains(
                   result.report,
                   "unsupported.track-polarity",
                   unsupportedTrackId)
               && reportContains(
                   result.report,
                   "unsupported.clip-reverse",
                   clip.id)
               && reportContains(
                   result.report,
                   "unsupported.clip-polarity",
                   clip.id)
               && reportContains(
                   result.report,
                   "unsupported.clip-fade-curve",
                   clip.id)
               && reportContains(
                   result.report,
                   "unsupported.routing",
                   hardware.id)
               && reportContains(
                   result.report,
                   "unsupported.reamp",
                   reamp.id)
               && reportContains(
                   result.report,
                   "unsupported.midi-editor-metadata",
                   midiClip.id)
               && reportContains(
                   result.report,
                   "unsupported.midi-humanization",
                   midiClip.id)
               && reportContains(
                   result.report,
                   "unsupported.note-probability",
                   midiNote.id)
               && reportContains(
                   result.report,
                   "unsupported.note-drum-metadata",
                   midiNote.id)
               && reportContains(
                   result.report,
                   "unsupported.automation-target",
                   unsupportedAutomation.id)
               && reportContains(
                   result.report,
                   "compatibility.device-state-container",
                   "opaque-vst-state"),
           "Every unsupported destination construct produces an object-specific report entry.");
    const auto reportedProjectXml =
        archiveEntryText(
            root.getChildFile("reported.dawproject"),
            "project.xml");
    expect(reportedProjectXml.contains("<Device")
               && reportedProjectXml.contains(
                   "studio-duo-format:VST3")
               && !reportedProjectXml.contains(
                   "opaque-vst-state.vstpreset"),
           "Opaque Studio Duo VST state is embedded generically instead of being mislabeled as a native VST preset.");

    const auto reportFile = root.getChildFile("compatibility-report.json");
    expect(studio::DawProjectIO::saveCompatibilityReport(
               result.report,
               reportFile)
               .wasOk()
               && reportFile.existsAsFile()
               && reportFile.loadFileAsString().contains(
                   "unsupported.clip-reverse"),
           "Compatibility reports can be saved as structured JSON.");

    const auto importArchive =
        root.getChildFile("unsupported-source.dawproject");
    const auto importProjectXml =
        R"(<Project version="1.0"><Application name="Fixture" version="1"/><Structure><Track id="source-track" name="Source" contentType="video"><Channel id="source-channel" role="regular" audioChannels="2" destination="master-channel"/></Track><Track id="master-track" name="Master" contentType="audio"><Channel id="master-channel" role="master" audioChannels="2"/></Track></Structure><Arrangement><Lanes><Lanes track="source-track"><Video duration="1" channels="2" sampleRate="48000"><File path="media/video.bin"/></Video></Lanes></Lanes></Arrangement></Project>)";
    juce::ZipFile::Builder importBuilder;
    const auto fixedTime = juce::Time(1980, 0, 1, 0, 0);
    importBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            importProjectXml,
            std::strlen(importProjectXml),
            false),
        9,
        "project.xml",
        fixedTime);
    importBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "<MetaData><Title>Unsupported source</Title></MetaData>",
            std::strlen(
                "<MetaData><Title>Unsupported source</Title></MetaData>"),
            false),
        9,
        "metadata.xml",
        fixedTime);
    importBuilder.addEntry(
        std::make_unique<juce::MemoryInputStream>(
            "video",
            std::strlen("video"),
            false),
        9,
        "media/video.bin",
        fixedTime);
    {
        auto output = importArchive.createOutputStream();
        expect(output != nullptr
                   && importBuilder.writeToStream(*output, nullptr),
               "Unsupported source archive can be written.");
    }
    const auto imported = studio::DawProjectIO::importProject(
        importArchive,
        root.getChildFile("unsupported-source.studioduo"));
    expect(imported.succeeded()
               && reportContains(
                   imported.report,
                   "unsupported.video",
                   "/Project/Arrangement/Lanes"),
           "Unsupported source timelines import transactionally with object-specific reports.");
    root.deleteRecursively();
}
}

void dawProjectTests()
{
    nativeSceneAndReportPersistence();
    officialSchemaValidation();
    officialCompatibilityFixtureImport();
    stableIdMapping();
    completeArchiveRoundTrip();
    invalidArchiveIsTransactional();
    externalMediaImport();
    unitAndChannelHierarchyImport();
    nativePluginStateContainerRoundTrip();
    unsupportedDataIsReported();
}
