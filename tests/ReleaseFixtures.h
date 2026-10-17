#pragma once

#include "TestHarness.h"
#include "model/ProjectModel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

namespace release_fixture
{
class Directory
{
public:
    Directory()
        : file(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("StudioDuoReleaseTests-" + juce::Uuid().toString()))
    {
        expect(file.createDirectory().wasOk(),
               "The isolated synthetic release-fixture directory can be created.");
    }

    ~Directory()
    {
        expect(file.deleteRecursively(),
               "The isolated release-fixture directory can be removed.");
    }

    const juce::File file;
};

inline juce::Result writeMedia(const juce::File& file, float amplitude = 0.125f)
{
    if (const auto result = file.getParentDirectory().createDirectory(); result.failed())
        return result;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(2)
            .withBitsPerSample(24));
    if (writer == nullptr)
        return juce::Result::fail("Could not create the synthetic release-fixture WAV.");
    juce::AudioBuffer<float> audio(2, 6000);
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto value = sample % 4 < 2 ? amplitude : -amplitude;
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, -value);
    }
    if (!writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples())
        || !writer->flush())
        return juce::Result::fail("Could not complete the synthetic release-fixture WAV.");
    return juce::Result::ok();
}

inline juce::String snapshot(const studio::Project& project)
{
    return juce::JSON::toString(project.toVar(), false);
}

inline studio::RoutingConnection mainRoute(
    const juce::String& id,
    const juce::String& source,
    const juce::String& destination)
{
    studio::RoutingConnection route;
    route.id = id;
    route.name = "Main output";
    route.sourceTrackId = source;
    route.kind = studio::RouteKind::mainOutput;
    route.destination.type = studio::RouteEndpointType::track;
    route.destination.trackId = destination;
    return route;
}

inline studio::Project exchangeProject(const juce::File& media)
{
    studio::Project project;
    project.id = "release-exchange-project";
    project.name = juce::String::fromUTF8("Synthetic \xc3\xa9" "change");
    project.metronomeEnabled = false;
    project.tempo = 137.0;
    project.tempoChanges = { { 0.0, 137.0, true }, { 0.1, 173.0, false } };
    project.meterChanges = { { 0.0, 7, 8 }, { 0.1, 4, 4 } };
    project.markers = { { "exchange-marker", "Edit boundary", 0.03125 } };
    project.sections = { { "exchange-section", "Synthetic song", 0.0 } };
    project.sections.front().endTimeSeconds = 0.125;
    project.sections.front().clickSettings = studio::SectionClickSettings {};
    project.loopStartSeconds = 0.03125;
    project.loopEndSeconds = 0.09375;
    project.metadata.album = "Synthetic release";

    studio::AudioClip clip;
    clip.id = "exchange-clip";
    clip.name = "Edited synthetic audio";
    clip.sourceFile = media;
    juce::FileInputStream mediaStream(media);
    if (mediaStream.openedOk())
        clip.sourceHash = juce::SHA256(mediaStream).toHexString();
    clip.startSeconds = 0.03125;
    clip.sourceOffsetSeconds = 0.015625;
    clip.sourceLengthSeconds = 0.125;
    clip.sourceRangeEndSeconds = 0.125;
    clip.durationSeconds = 0.0625;
    clip.fadeInSeconds = 0.00390625;
    clip.fadeOutSeconds = 0.0078125;
    clip.fadeInCurve = -0.5f;
    clip.fadeOutCurve = 0.5f;
    clip.gainDecibels = -3.0f;
    clip.reversed = true;
    clip.transientSourceSeconds = { 0.03125, 0.0625 };

    studio::Track audio;
    audio.id = "exchange-audio";
    audio.name = "Synthetic audio";
    audio.clips = { clip };
    audio.pan = -0.25f;
    audio.volumeDecibels = -6.0f;
    audio.outputTrackId = "exchange-bus";

    studio::PluginInsert rack;
    rack.id = "exchange-rack";
    rack.name = "Serial rack";
    rack.kind = studio::PluginInsertKind::serialRack;
    studio::PluginInsert plugin;
    plugin.id = "exchange-plugin";
    plugin.name = "Missing synthetic processor";
    plugin.pluginIdentifier = "synthetic.release.processor";
    plugin.format = "VST3";
    plugin.containerId = rack.id;
    plugin.missing = true;
    plugin.latencySamples = 64;
    audio.inserts = { rack, plugin };

    studio::Track phase = audio;
    phase.id = "exchange-phase";
    phase.name = "Phase partner";
    phase.inserts.clear();
    phase.clips.front().id = "exchange-phase-clip";
    phase.clips.front().polarityInverted = true;

    studio::Track take;
    take.id = "exchange-take";
    take.name = "Synthetic take";
    take.parentTrackId = audio.id;
    take.versionNumber = 1;
    take.clips = { clip };
    take.clips.front().id = "exchange-take-clip";
    audio.activeTakeTrackId = take.id;
    audio.compRegions = { { "exchange-comp", take.id, 0.03125, 0.0625 } };

    studio::Track instrument;
    instrument.id = "exchange-instrument";
    instrument.name = "Synthetic instrument";
    instrument.type = studio::TrackType::instrument;
    instrument.outputTrackId = "exchange-bus";
    studio::MidiClip midi;
    midi.id = "exchange-midi-clip";
    midi.durationBeats = 1.0;
    midi.humanizeSeed = 18446744073709549567ULL;
    studio::MidiNote note;
    note.id = "exchange-note";
    note.pitch = 36;
    note.timingOffsetBeats = 0.00390625;
    note.probability = 0.75;
    studio::MidiExpressionPoint expression;
    expression.id = "exchange-expression";
    expression.type = studio::MidiExpressionType::channelPressure;
    expression.offsetBeats = 0.0625;
    expression.value = 0.5;
    note.expressions = { expression };
    midi.notes = { note };
    instrument.midiClips = { midi };

    studio::Track bus;
    bus.id = "exchange-bus";
    bus.name = "Synthetic bus";
    bus.type = studio::TrackType::bus;
    bus.outputTrackId = "exchange-master";
    studio::Track master;
    master.id = "exchange-master";
    master.name = "Master";
    master.type = studio::TrackType::master;
    project.tracks = { audio, take, phase, instrument, bus, master };
    project.routingConnections = {
        mainRoute("exchange-route-audio", audio.id, bus.id),
        mainRoute("exchange-route-phase", phase.id, bus.id),
        mainRoute("exchange-route-instrument", instrument.id, bus.id),
        mainRoute("exchange-route-bus", bus.id, master.id)
    };
    studio::RoutingConnection send;
    send.id = "exchange-send";
    send.name = "Synthetic send";
    send.kind = studio::RouteKind::send;
    send.sourceTrackId = audio.id;
    send.destination.type = studio::RouteEndpointType::track;
    send.destination.trackId = bus.id;
    send.gainDecibels = -12.0f;
    project.routingConnections.push_back(send);

    studio::AutomationLane lane;
    lane.id = "exchange-automation";
    lane.name = "Beat-based mix automation";
    lane.target.trackId = audio.id;
    lane.timebase = studio::AutomationTimebase::beats;
    lane.points = { { "exchange-point-a", 0.0, 0.5 },
                    { "exchange-point-b", 0.25, 0.75 } };
    project.automationLanes = { lane };
    studio::EditGroup group;
    group.id = "exchange-edit-group";
    group.name = "Phase-linked synthetic tracks";
    group.trackIds = { audio.id, phase.id };
    group.timingReferenceTrackId = audio.id;
    group.protectedAnchorsSeconds = { 0.0625 };
    project.editGroups = { group };
    studio::ReampRoute reamp;
    reamp.id = "exchange-reamp";
    reamp.name = "Synthetic calibrated hardware route";
    reamp.sourceTrackId = audio.id;
    reamp.returnTrackId = phase.id;
    reamp.latencySamples = 257;
    reamp.alignmentOffsetSamples = -7;
    reamp.enabled = false;
    project.reampRoutes = { reamp };

    studio::ProjectScene scene;
    scene.id = "exchange-scene";
    scene.name = "Synthetic scene";
    studio::SceneSlot slot;
    slot.id = "exchange-slot";
    slot.trackId = phase.id;
    slot.audioClip = clip;
    slot.audioClip->id = "exchange-scene-clip";
    scene.slots = { slot };
    project.scenes = { scene };
    studio::CompatibilityReport report;
    report.id = "exchange-compatibility";
    report.format = "DAWproject";
    report.operation = "import";
    report.createdAt = "2026-01-01T00:00:00Z";
    report.issues = { { studio::CompatibilitySeverity::warning,
                        "synthetic.unsupported",
                        "/tracks/exchange-audio",
                        "Synthetic compatibility warning." } };
    project.compatibilityReports = { report };
    project.mastering.title = "Synthetic multi-song release";
    for (int index = 0; index < 2; ++index)
    {
        studio::MasteringTrack song;
        song.id = "exchange-song-" + juce::String(index);
        song.title = "Synthetic song " + juce::String(index + 1);
        studio::MasteringSourceMix mix;
        mix.id = "exchange-mix-" + juce::String(index);
        mix.file = media;
        mix.sourceHash = clip.sourceHash;
        mix.durationSeconds = 0.125;
        song.sources = { mix };
        song.selectedSourceId = mix.id;
        song.gapBeforeSeconds = index == 0 ? 0.0 : 0.0625;
        song.fadeInSeconds = 0.00390625;
        song.fadeOutSeconds = 0.00390625;
        project.mastering.tracks.push_back(std::move(song));
    }
    return project;
}
}
