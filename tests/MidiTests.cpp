#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "midi/MidiCaptureBuffer.h"
#include "midi/MidiEditing.h"
#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
studio::Track makeMidiTrack(studio::Project& project,
                            const juce::String& name = "Drums")
{
    studio::Track track;
    track.name = name;
    track.type = studio::TrackType::midi;
    project.tracks.insert(project.tracks.end() - 1, track);
    return track;
}

studio::MidiClip makeMidiClip(const studio::Project& project)
{
    studio::MidiClip clip;
    clip.name = "Metal MIDI";
    clip.editorMode = studio::MidiEditorMode::drums;
    clip.drumMapId = project.drumMaps.front().id;
    clip.durationBeats = 4.0;
    studio::MidiNote note;
    note.pitch = 38;
    note.startBeats = 0.5;
    note.durationBeats = 0.5;
    note.velocity = 112;
    note.probability = 0.75;
    const auto* mapEntry = project.drumMaps.front().entryForPitch(
        note.pitch);
    note.drumMapEntryId = mapEntry->id;
    note.articulation = mapEntry->articulation;
    note.chokeGroup = mapEntry->chokeGroup;
    note.cymbalState = mapEntry->cymbalState;
    studio::MidiExpressionPoint pressure;
    pressure.type = studio::MidiExpressionType::pressure;
    pressure.offsetBeats = 0.125;
    pressure.value = 0.65;
    studio::MidiExpressionPoint pitch;
    pitch.type = studio::MidiExpressionType::pitchBend;
    pitch.offsetBeats = 0.25;
    pitch.value = -0.2;
    note.expressions = { pressure, pitch };
    clip.notes.push_back(note);
    return clip;
}

void midiPersistence()
{
    auto project = studio::Project::createDefault();
    auto track = makeMidiTrack(project);
    auto clip = makeMidiClip(project);
    const auto trackId = track.id;
    const auto clipId = clip.id;
    project.findTrack(trackId)->midiClips.push_back(clip);

    juce::String error;
    const auto decoded = studio::Project::fromVar(
        project.toVar(),
        error);
    expect(decoded.has_value(), error.toRawUTF8());
    const auto* decodedClip = decoded.has_value()
        ? decoded->findMidiClip(clipId)
        : nullptr;
    expect(decodedClip != nullptr
               && decodedClip->notes.size() == 1
               && decodedClip->notes.front().id == clip.notes.front().id
               && decodedClip->notes.front().expressions.size() == 2
               && decodedClip->drumMapId == clip.drumMapId,
           "MIDI clips, stable note IDs, expressions, and drum-map references survive model serialization.");
    expect(decoded.has_value()
               && !decoded->drumMaps.empty()
               && !decoded->midiPatterns.empty()
               && !decoded->midiRoutingTemplates.empty(),
           "Drum maps, pattern aliases, and routing templates survive model serialization.");

    const auto package = juce::File::getCurrentWorkingDirectory()
                             .getNonexistentChildFile(
                                 "StudioDuoMidiPersistence",
                                 ".studioduo",
                                 false);
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "A project containing MIDI composition data saves.");
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value(), error.toRawUTF8());
    const auto* loadedClip = loaded.has_value()
        ? loaded->findMidiClip(clipId)
        : nullptr;
    expect(loadedClip != nullptr
               && loadedClip->notes.front().expressions.front().id
                      == clip.notes.front().expressions.front().id
               && loaded->findDrumMap(clip.drumMapId) != nullptr,
           "Native project generations restore exact MIDI notes, expression IDs, and maps.");
    package.deleteRecursively();

    auto legacy = project.toVar();
    auto* object = legacy.getDynamicObject();
    object->setProperty("formatVersion", 4);
    object->removeProperty("drumMaps");
    object->removeProperty("midiPatterns");
    object->removeProperty("midiRoutingTemplates");
    for (auto& trackValue : *object->getProperty("tracks").getArray())
        trackValue.getDynamicObject()->removeProperty("midiClips");
    error.clear();
    const auto migrated = studio::Project::fromVar(legacy, error);
    const auto migratedAgain = studio::Project::fromVar(legacy, error);
    expect(migrated.has_value(), error.toRawUTF8());
    expect(migrated.has_value()
               && migrated->drumMaps.size() == 1
               && migrated->midiPatterns.size() == 3
               && migrated->midiRoutingTemplates.size() == 1
               && std::all_of(
                   migrated->tracks.cbegin(),
                   migrated->tracks.cend(),
                   [](const auto& migratedTrack)
                   {
                       return migratedTrack.midiClips.empty();
                   })
               && migratedAgain.has_value()
               && migratedAgain->drumMaps.front().id
                      == migrated->drumMaps.front().id
               && migratedAgain->midiPatterns.front().id
                      == migrated->midiPatterns.front().id,
           "Version 4 projects migrate with empty MIDI clips and deterministic default editor resources.");
}

void midiEditingCommands()
{
    auto project = studio::Project::createDefault();
    auto track = makeMidiTrack(project, "MIDI editing");
    const auto trackId = track.id;
    auto clip = makeMidiClip(project);
    const auto clipId = clip.id;
    const auto noteId = clip.notes.front().id;
    studio::CommandStack commands;
    juce::String error;
    expect(commands.perform(
               std::make_unique<studio::AddMidiClipCommand>(
                   trackId,
                   clip),
               project,
               error),
           error.toRawUTF8());
    expect(project.findMidiClip(clipId) != nullptr,
           "AddMidiClipCommand creates an ordinary editable clip.");

    auto before = *project.findMidiClip(clipId);
    auto after = before;
    expect(studio::moveMidiNotes(
               after,
               { noteId },
               0.25,
               2),
           "UI-independent note movement applies.");
    expect(studio::resizeMidiNotes(
               after,
               { noteId },
               0.25,
               1.0 / 128.0),
           "UI-independent note resizing applies.");
    expect(studio::setMidiLaneValue(
               after,
               { noteId },
               studio::MidiEditorLane::velocity,
               0.5,
               0.0,
               studio::MidiExpressionType::pressure),
           "UI-independent velocity lane editing applies.");
    expect(studio::setMidiLaneValue(
               after,
               { noteId },
               studio::MidiEditorLane::timing,
               0.75,
               0.0,
               studio::MidiExpressionType::pressure)
               && studio::setMidiLaneValue(
                   after,
                   { noteId },
                   studio::MidiEditorLane::probability,
                   0.4,
                   0.0,
                   studio::MidiExpressionType::pressure)
               && studio::setMidiLaneValue(
                   after,
                   { noteId },
                   studio::MidiEditorLane::expression,
                   0.8,
                   1.0,
                   studio::MidiExpressionType::timbre),
           "Timing, probability, and per-note expression lane edits apply without UI state.");
    expect(commands.perform(
               std::make_unique<studio::SetMidiClipStateCommand>(
                   trackId,
                   before,
                   after,
                   "Edit MIDI notes"),
               project,
               error),
           error.toRawUTF8());
    const auto* edited = project.findMidiClip(clipId);
    expect(edited != nullptr
               && edited->notes.front().pitch == 40
               && std::abs(edited->notes.front().startBeats - 0.75)
                      < 0.000001
               && std::abs(edited->notes.front().durationBeats - 0.75)
                      < 0.000001
               && std::abs(
                      edited->notes.front().timingOffsetBeats
                      - 0.0625)
                      < 0.000001
               && std::abs(edited->notes.front().probability - 0.4)
                      < 0.000001
               && edited->notes.front().velocity == 64
               && std::any_of(
                   edited->notes.front().expressions.cbegin(),
                   edited->notes.front().expressions.cend(),
                   [](const auto& point)
                   {
                       return point.type
                               == studio::MidiExpressionType::timbre
                           && std::abs(point.value - 0.8) < 0.000001;
                   }),
           "Undoable MIDI clip state commands persist move, resize, and lane edits.");
    expect(commands.undo(project)
               && project.findMidiClip(clipId)->notes.front().pitch == 38,
           "Undo restores the complete prior MIDI clip state.");
    expect(commands.redo(project, error)
               && project.findMidiClip(clipId)->notes.front().pitch == 40,
           "Redo restores the edited MIDI clip state.");
    auto noteDeleted = *project.findMidiClip(clipId);
    expect(studio::deleteMidiNotes(noteDeleted, { noteId })
               && noteDeleted.notes.empty(),
           "UI-independent note deletion removes selected stable IDs only.");
    auto duplicateTrack =
        std::make_unique<studio::DuplicateTrackCommand>(trackId);
    auto* duplicateTrackPointer = duplicateTrack.get();
    expect(commands.perform(
               std::move(duplicateTrack),
               project,
               error),
           error.toRawUTF8());
    const auto* duplicatedTrack = project.findTrack(
        duplicateTrackPointer->duplicatedTrackId());
    expect(duplicatedTrack != nullptr
               && duplicatedTrack->midiClips.size() == 1
               && duplicatedTrack->midiClips.front().id != clipId
               && duplicatedTrack->midiClips.front().notes.front().id
                      != noteId
               && duplicatedTrack->midiClips.front()
                      .notes.front()
                      .expressions.front()
                      .id
                      != clip.notes.front().expressions.front().id,
           "Duplicating a MIDI track regenerates clip, note, and expression IDs.");
    expect(commands.undo(project)
               && project.findTrack(
                      duplicateTrackPointer->duplicatedTrackId())
                      == nullptr,
           "Undo removes the duplicated MIDI track family.");
    expect(commands.perform(
               std::make_unique<studio::DeleteMidiClipCommand>(clipId),
               project,
               error)
               && project.findMidiClip(clipId) == nullptr,
           "DeleteMidiClipCommand removes a MIDI clip.");
    expect(commands.undo(project)
               && project.findMidiClip(clipId) != nullptr,
           "Undo restores a deleted MIDI clip with stable IDs.");
}

void deterministicHumanization()
{
    auto project = studio::Project::createDefault();
    auto clip = makeMidiClip(project);
    clip.notes.clear();
    for (int index = 0; index < 8; ++index)
    {
        studio::MidiNote note;
        note.id = "humanize-note-" + juce::String(index);
        note.startBeats = 1.0 + static_cast<double>(index) * 0.25;
        note.velocity = 90 + index;
        clip.notes.push_back(note);
    }
    studio::MidiHumanizeSettings settings;
    settings.seed = 0x123456789ull;
    settings.maximumTimingTicks = 18;
    settings.maximumVelocityChange = 11;
    const auto first = studio::humanizeMidiClip(clip, settings);
    const auto second = studio::humanizeMidiClip(clip, settings);
    const auto different = studio::humanizeMidiClip(
        clip,
        { settings.seed + 1, 18, 11 });
    expect(
        juce::JSON::toString(first.toVar(), false)
            == juce::JSON::toString(second.toVar(), false),
        "Seeded humanization produces byte-identical project JSON for the same input.");
    expect(
        juce::JSON::toString(first.toVar(), false)
            != juce::JSON::toString(different.toVar(), false),
        "Changing the humanization seed changes deterministic results.");
    expect(first.humanizeSeed == settings.seed
               && first.humanizeTimingTicks == 18
               && first.humanizeVelocity == 11
               && std::all_of(
                   first.notes.cbegin(),
                   first.notes.cend(),
                   [](const auto& note)
                   {
                       return note.velocity >= 1
                           && note.velocity <= 127
                           && note.actualStartBeats() >= 0.0;
                   }),
           "Humanization stores its seed and exact bounded timing/velocity results.");
    const std::array expectedTimingTicks { -1, 7, -12, -17, -4, -4, -4, -15 };
    const std::array expectedVelocities { 92, 99, 84, 96, 99, 86, 105, 89 };
    auto referenceMatches = first.notes.size() == expectedTimingTicks.size();
    for (std::size_t index = 0;
         referenceMatches && index < first.notes.size();
         ++index)
    {
        referenceMatches =
            static_cast<int>(std::llround(
                first.notes[index].timingOffsetBeats * 960.0))
                == expectedTimingTicks[index]
            && first.notes[index].velocity == expectedVelocities[index];
    }
    expect(referenceMatches,
           "Humanization matches the fixed cross-platform integer reference vector.");
    juce::String error;
    const auto reopened = studio::MidiClip::fromVar(
        first.toVar(),
        error);
    expect(reopened.has_value(), error.toRawUTF8());
    expect(reopened.has_value()
               && juce::JSON::toString(reopened->toVar(), false)
                      == juce::JSON::toString(first.toVar(), false),
           "Exact humanized results survive save and reopen.");
    auto maximumSeed = first;
    maximumSeed.humanizeSeed =
        std::numeric_limits<std::uint64_t>::max();
    const auto maximumSeedRoundTrip = studio::MidiClip::fromVar(
        maximumSeed.toVar(),
        error);
    expect(maximumSeedRoundTrip.has_value()
               && maximumSeedRoundTrip->humanizeSeed
                      == maximumSeed.humanizeSeed,
           "Humanization seeds preserve the full unsigned 64-bit range.");
}

void drumMapsPatternsAndTools()
{
    const auto map = studio::createDefaultMetalDrumMap();
    juce::String error;
    const auto imported = studio::DrumMap::fromVar(map.toVar(), error);
    expect(imported.has_value(), error.toRawUTF8());
    const auto* openHat = map.entryForPitch(46);
    expect(openHat != nullptr
               && openHat->chokeGroup == "hihat"
               && openHat->cymbalState == studio::CymbalState::open
               && openHat->footControlCC == 4,
           "Metal drum maps retain named articulations, choke groups, cymbal states, and foot control.");
    const auto* snare = map.entryForPitch(38);
    expect(snare != nullptr && !snare->roundRobinNotes.empty(),
           "Metal drum maps expose round-robin note hints.");

    const auto patterns = studio::createDefaultMetalPatterns(map);
    const auto expanded = studio::expandMidiPattern(
        patterns.front(),
        &map,
        2.0,
        2);
    expect(expanded.size() == patterns.front().events.size() * 2
               && expanded.front().startBeats >= 2.0
               && expanded.front().id
                      != patterns.front().events.front().id,
           "Pattern aliases expand into independent ordinary MIDI notes.");

    studio::MidiEntryRequest request;
    request.startBeats = 1.0;
    request.lengthBeats = 1.0;
    request.stepBeats = 0.25;
    request.pitch = 38;
    const auto flam = studio::generateMidiEntryTool(
        studio::MidiEntryTool::flam,
        request,
        &map);
    const auto roll = studio::generateMidiEntryTool(
        studio::MidiEntryTool::roll,
        request,
        &map);
    const auto gravity = studio::generateMidiEntryTool(
        studio::MidiEntryTool::gravityBlast,
        request,
        &map);
    const auto blast = studio::generateMidiEntryTool(
        studio::MidiEntryTool::blastBeat,
        request,
        &map);
    const auto doubleKick = studio::generateMidiEntryTool(
        studio::MidiEntryTool::doubleKick,
        request,
        &map);
    expect(flam.size() == 8
               && roll.size() == 4
               && gravity.size() == 10
               && blast.size() == 10
               && doubleKick.size() == 4,
           "Flam, roll, gravity blast, blast beat, and double-kick tools produce fixed note counts.");
    expect(doubleKick[0].pitch != doubleKick[1].pitch
               && doubleKick[0].drumMapEntryId.isNotEmpty()
               && blast.front().articulation.isNotEmpty(),
           "Metal entry tools use mapped kit pieces while leaving ordinary note pitches editable.");

    auto invalid = map.toVar();
    auto* entries = invalid.getDynamicObject()
                        ->getProperty("entries")
                        .getArray();
    entries->getReference(1)
        .getDynamicObject()
        ->setProperty(
            "noteNumber",
            entries->getReference(0)
                .getDynamicObject()
                ->getProperty("noteNumber"));
    error.clear();
    expect(!studio::DrumMap::fromVar(invalid, error).has_value()
               && error.containsIgnoreCase("unique"),
           "Invalid imported drum-map pitches are rejected explicitly.");

    auto project = studio::Project::createDefault();
    auto beforeResources =
        studio::ProjectMidiResources::fromProject(project);
    auto afterResources = beforeResources;
    afterResources.drumMaps.front().entries.front().name =
        "Edited kick";
    studio::CommandStack commands;
    error.clear();
    expect(commands.perform(
               std::make_unique<
                   studio::SetProjectMidiResourcesCommand>(
                   beforeResources,
                   afterResources,
                   "Edit drum map"),
               project,
               error)
               && project.drumMaps.front().entries.front().name
                      == "Edited kick",
           "Editable drum-map changes use the project command stack.");
    expect(commands.undo(project)
               && project.drumMaps.front().entries.front().name
                      != "Edited kick",
           "Drum-map edits are undoable.");
}

void routingTemplates()
{
    auto project = studio::Project::createDefault();
    auto track = makeMidiTrack(project, "Drum source");
    const auto trackId = track.id;
    studio::MidiClip clip;
    clip.durationBeats = 4.0;
    for (const auto pitch : { 36, 38, 45, 49 })
        clip.notes.push_back(
            studio::createMidiNote(
                clip,
                static_cast<double>(clip.notes.size()) * 0.25,
                pitch,
                0.125,
                100,
                &project.drumMaps.front()));
    project.findTrack(trackId)->midiClips.push_back(clip);
    const auto originalTrackCount = project.tracks.size();
    const auto originalRouteCount = project.routingConnections.size();

    studio::CommandStack commands;
    juce::String error;
    expect(commands.perform(
               std::make_unique<
                   studio::ApplyMidiRoutingTemplateCommand>(
                   trackId,
                   project.midiRoutingTemplates.front()),
               project,
               error),
           error.toRawUTF8());
    const auto* routedSource = project.findTrack(trackId);
    const auto filteredRoutes = static_cast<int>(std::count_if(
        project.routingConnections.cbegin(),
        project.routingConnections.cend(),
        [trackId](const auto& route)
        {
            return route.sourceTrackId == trackId
                && route.signalType == studio::SignalType::midi
                && route.midiChannel >= 1
                && route.midiChannel <= 4;
        }));
    expect(project.tracks.size() == originalTrackCount + 4
               && project.routingConnections.size()
                      == originalRouteCount + 4
               && filteredRoutes == 4
               && routedSource != nullptr
               && routedSource->midiClips.front().notes[0].channel == 1
               && routedSource->midiClips.front().notes[1].channel == 2
               && routedSource->midiClips.front().notes[2].channel == 3
               && routedSource->midiClips.front().notes[3].channel == 4,
           "Multi-output routing templates create destinations, routes, and per-piece channels.");
    expect(commands.undo(project)
               && project.tracks.size() == originalTrackCount
               && project.routingConnections.size() == originalRouteCount
               && project.findTrack(trackId)
                      ->midiClips.front().notes[1].channel == 1,
           "Applying a MIDI routing template is fully undoable.");
    expect(commands.redo(project, error)
               && project.tracks.size() == originalTrackCount + 4,
           "MIDI routing templates redo with stable generated track and route IDs.");
}

void midiRecordingAndRetrospectiveCapture()
{
    studio::StudioAudioEngine engine;
    studio::RecordingPlan plan;
    plan.transportStartSeconds = 0.0;
    plan.captureStartSeconds = 0.0;
    expect(engine.startRecording({}, plan, true).wasOk(),
           "MIDI-only recording starts without allocating an audio-file recorder in tests.");
    juce::MidiBuffer firstBlock;
    firstBlock.addEvent(
        juce::MidiMessage::noteOn(
            2,
            60,
            static_cast<juce::uint8>(100)),
        4);
    firstBlock.addEvent(
        juce::MidiMessage::aftertouchChange(2, 60, 80),
        12);
    juce::ignoreUnused(
        engine.renderActiveBlockWithMidiForTesting(firstBlock, 32));
    juce::MidiBuffer secondBlock;
    secondBlock.addEvent(
        juce::MidiMessage::pitchWheel(2, 10000),
        4);
    secondBlock.addEvent(
        juce::MidiMessage::noteOff(2, 60),
        20);
    juce::ignoreUnused(
        engine.renderActiveBlockWithMidiForTesting(secondBlock, 32));
    auto recording = engine.stopMidiRecording();
    engine.stopRecording();
    expect(recording.result.wasOk()
               && recording.captured.events.size() == 4
               && recording.durationSeconds > 0.0,
           "The real-time-safe ring captures note and expression events during recording.");

    auto project = studio::Project::createDefault();
    juce::String error;
    const auto converted = studio::convertCapturedMidiToClip(
        project,
        recording.captured,
        recording.captureStartStreamSample,
        recording.timelineStartSeconds,
        recording.durationSeconds,
        recording.sampleRate,
        nullptr,
        "Recorded MIDI",
        error);
    expect(converted.has_value(), error.toRawUTF8());
    expect(converted.has_value()
               && converted->clip.notes.size() == 1
               && converted->clip.notes.front().channel == 2
               && converted->clip.notes.front().expressions.size() == 2
               && converted->clip.notes.front().durationBeats > 0.0,
           "Recorded raw events become an ordinary editable note with per-note expressions.");

    studio::StudioAudioEngine retrospectiveEngine;
    juce::MidiBuffer recent;
    recent.addEvent(
        juce::MidiMessage::noteOn(
            1,
            64,
            static_cast<juce::uint8>(96)),
        3);
    recent.addEvent(juce::MidiMessage::noteOff(1, 64), 27);
    juce::ignoreUnused(
        retrospectiveEngine.renderActiveBlockWithMidiForTesting(
            recent,
            64));
    const auto retrospective =
        retrospectiveEngine.captureRetrospectiveMidi(10.0);
    expect(retrospective.result.wasOk()
               && retrospective.captured.events.size() == 2
               && std::abs(
                      retrospective.timelineStartSeconds
                      - 3.0 / retrospective.sampleRate)
                      < 0.0000001,
           "Retrospective capture retrieves recent stopped-transport MIDI without affecting live routing.");

    studio::MidiCaptureBuffer ring;
    juce::MidiBuffer direct;
    direct.addEvent(
        juce::MidiMessage::controllerEvent(1, 74, 90),
        7);
    ring.push(direct, 100, 200);
    const auto window = ring.read(0, ring.writeOrdinal());
    expect(window.events.size() == 1
               && window.events.front().streamSample == 107
               && window.events.front().timelineSample == 207,
           "The capture ring preserves deterministic stream and timeline sample positions.");
}
}

void midiTests()
{
    midiPersistence();
    midiEditingCommands();
    deterministicHumanization();
    drumMapsPatternsAndTools();
    routingTemplates();
    midiRecordingAndRetrospectiveCapture();
}
