#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>

namespace
{
bool closeTo(double left, double right, double tolerance = 0.000001)
{
    return std::abs(left - right) <= tolerance;
}

bool matchesSongSections(
    const studio::Project& project,
    std::initializer_list<studio::SongSection> expected)
{
    return std::equal(
        project.sections.cbegin(),
        project.sections.cend(),
        expected.begin(),
        expected.end(),
        [](const auto& actual, const auto& wanted)
        {
            return actual.id == wanted.id
                && actual.name == wanted.name
                && closeTo(actual.timeSeconds, wanted.timeSeconds);
        });
}

void namedSongMarkerCommands()
{
    auto project = studio::Project::createDefault();
    studio::SongSection start;
    start.name = "Start";
    start.timeSeconds = 1.25;
    studio::SongSection end;
    end.name = "End";
    end.timeSeconds = 4.5;
    auto paddedStart = start;
    paddedStart.name = " \tStart \n";

    studio::CommandStack commands;
    juce::String error;
    expect(commands.perform(
               std::make_unique<studio::AddSongSectionCommand>(end),
               project,
               error)
               && commands.perform(
                   std::make_unique<studio::AddSongSectionCommand>(
                       paddedStart),
                   project,
                   error)
               && matchesSongSections(project, { start, end }),
           "Named Start and End markers are trimmed and inserted in timeline order.");
    expect(commands.undo(project)
               && matchesSongSections(project, { end })
               && commands.redo(project, error)
               && matchesSongSections(project, { start, end }),
           "Adding a marker preserves its ID, name, and position across undo and redo.");

    auto serializedProject = project;
    serializedProject.sections = { end, paddedStart };
    const auto loaded = studio::Project::fromVar(
        juce::JSON::parse(
            juce::JSON::toString(serializedProject.toVar())),
        error);
    expect(loaded.has_value()
               && matchesSongSections(*loaded, { start, end }),
           "Marker IDs, trimmed names, and fractional positions survive JSON round trips in sorted order.");

    auto movedRequest = start;
    movedRequest.name = "  Export start  ";
    movedRequest.timeSeconds = 6.75;
    auto movedStart = movedRequest;
    movedStart.name = movedStart.name.trim();
    expect(commands.perform(
               std::make_unique<studio::SetSongSectionCommand>(
                   start,
                   movedRequest),
               project,
               error)
               && matchesSongSections(project, { end, movedStart }),
           "Editing a marker renames and repositions its stable ID while reordering the timeline.");
    expect(commands.undo(project)
               && matchesSongSections(project, { start, end })
               && commands.redo(project, error)
               && matchesSongSections(project, { end, movedStart }),
           "Undo and redo restore both marker states and their sorted positions.");

    auto renamedStart = movedStart;
    renamedStart.name = "New Start";
    expect(commands.perform(
               std::make_unique<studio::SetSongSectionCommand>(
                   movedStart,
                   renamedStart),
               project,
               error)
               && matchesSongSections(project, { end, renamedStart })
               && commands.undo(project)
               && matchesSongSections(project, { end, movedStart })
               && commands.redo(project, error)
               && matchesSongSections(project, { end, renamedStart }),
           "Renaming without moving does not treat the marker's own position as a duplicate.");

    expect(commands.perform(
               std::make_unique<studio::RemoveSongSectionCommand>(
                   end.id),
               project,
               error)
               && matchesSongSections(project, { renamedStart })
               && commands.undo(project)
               && matchesSongSections(project, { end, renamedStart })
               && commands.redo(project, error)
               && matchesSongSections(project, { renamedStart }),
           "Removing the first marker and undoing or redoing preserves the remaining marker.");
    expect(commands.undo(project)
               && commands.perform(
                   std::make_unique<studio::RemoveSongSectionCommand>(
                       renamedStart.id),
                   project,
                   error)
               && matchesSongSections(project, { end })
               && commands.undo(project)
               && matchesSongSections(project, { end, renamedStart })
               && commands.redo(project, error)
               && matchesSongSections(project, { end }),
           "Removing the last marker reinserts its original ID, name, and sorted position on undo.");

    while (commands.undo(project))
    {
    }
    expect(project.sections.empty(),
           "Undoing the complete marker edit history restores an empty marker lane.");
    while (commands.redo(project, error))
    {
    }
    expect(matchesSongSections(project, { end }),
           "Replaying the marker edit history is deterministic.");
}

void invalidSongMarkers()
{
    auto project = studio::Project::createDefault();
    studio::SongSection start;
    start.name = "Start";
    start.timeSeconds = 1.0;
    studio::SongSection end;
    end.name = "End";
    end.timeSeconds = 4.0;
    project.sections = { start, end };

    studio::SongSection middle;
    middle.name = "Cue";
    middle.timeSeconds = 2.0;
    studio::CommandStack commands;
    juce::String error;
    expect(commands.perform(
               std::make_unique<studio::AddSongSectionCommand>(middle),
               project,
               error)
               && commands.undo(project),
           "Invalid marker tests retain an existing redo branch.");
    const auto originalProject = juce::JSON::toString(project.toVar());
    const auto rejectWithoutMutation = [&](
                                           std::unique_ptr<studio::ProjectCommand> command,
                                           const char* message)
    {
        error.clear();
        const auto rejected = !commands.perform(
            std::move(command),
            project,
            error);
        expect(rejected
                   && error.isNotEmpty()
                   && juce::JSON::toString(project.toVar()) == originalProject
                   && !commands.canUndo()
                   && commands.canRedo(),
               message);
    };
    const auto rejectSerializedMarker = [&](
                                            const studio::SongSection& marker,
                                            const char* message)
    {
        auto invalidProject = project;
        invalidProject.sections.push_back(marker);
        error.clear();
        expect(!studio::Project::fromVar(
                    invalidProject.toVar(),
                    error)
                    .has_value()
                   && error.isNotEmpty(),
               message);
    };

    for (const auto invalidTime : {
             -1.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity() })
    {
        auto invalid = middle;
        invalid.timeSeconds = invalidTime;
        rejectWithoutMutation(
            std::make_unique<studio::AddSongSectionCommand>(invalid),
            "Adding a negative or non-finite marker leaves the project and command history unchanged.");
        auto edited = start;
        edited.timeSeconds = invalidTime;
        rejectWithoutMutation(
            std::make_unique<studio::SetSongSectionCommand>(
                start,
                edited),
            "Moving a marker to a negative or non-finite time leaves the project and command history unchanged.");
        error.clear();
        expect(!studio::SongSection::fromVar(invalid.toVar(), error)
                    .has_value()
                   && error.isNotEmpty(),
               "Individual marker deserialization rejects negative and non-finite positions.");
        rejectSerializedMarker(
            invalid,
            "Project loading rejects negative and non-finite marker positions.");
    }

    for (const auto& invalidText : { juce::String(), juce::String(" \t\n") })
    {
        auto invalid = middle;
        invalid.name = invalidText;
        rejectWithoutMutation(
            std::make_unique<studio::AddSongSectionCommand>(invalid),
            "Adding an empty or whitespace-only marker name does not mutate the project.");
        auto edited = start;
        edited.name = invalidText;
        rejectWithoutMutation(
            std::make_unique<studio::SetSongSectionCommand>(
                start,
                edited),
            "Renaming a marker to an empty or whitespace-only name does not mutate the project.");
        rejectSerializedMarker(
            invalid,
            "Project loading rejects empty and whitespace-only marker names.");

        invalid = middle;
        invalid.id = invalidText;
        rejectWithoutMutation(
            std::make_unique<studio::AddSongSectionCommand>(invalid),
            "Adding a marker without a usable stable ID does not mutate the project.");
        edited = start;
        edited.id = invalidText;
        rejectWithoutMutation(
            std::make_unique<studio::SetSongSectionCommand>(
                start,
                edited),
            "Editing cannot clear a marker's stable ID.");
        rejectSerializedMarker(
            invalid,
            "Project loading rejects empty and whitespace-only marker IDs.");
    }

    for (const auto duplicatePosition : {
             end.timeSeconds,
             end.timeSeconds - 0.00005,
             end.timeSeconds + 0.00005 })
    {
        auto duplicate = middle;
        duplicate.timeSeconds = duplicatePosition;
        rejectWithoutMutation(
            std::make_unique<studio::AddSongSectionCommand>(duplicate),
            "Adding a marker within the existing position tolerance is rejected without mutation.");
        auto edited = start;
        edited.timeSeconds = duplicatePosition;
        rejectWithoutMutation(
            std::make_unique<studio::SetSongSectionCommand>(
                start,
                edited),
            "Moving a marker within another marker's position tolerance is rejected without mutation.");
        rejectSerializedMarker(
            duplicate,
            "Project loading enforces the same marker position tolerance as editing.");
    }

    auto duplicateId = middle;
    duplicateId.id = end.id;
    rejectWithoutMutation(
        std::make_unique<studio::AddSongSectionCommand>(duplicateId),
        "Adding a duplicate marker ID is rejected even at a different time.");
    rejectSerializedMarker(
        duplicateId,
        "Project loading rejects duplicate marker IDs at different positions.");
    auto changedId = start;
    changedId.id = end.id;
    rejectWithoutMutation(
        std::make_unique<studio::SetSongSectionCommand>(start, changedId),
        "Editing cannot change a marker ID to another existing marker.");
    changedId.id = juce::Uuid().toString();
    rejectWithoutMutation(
        std::make_unique<studio::SetSongSectionCommand>(start, changedId),
        "Editing cannot change a marker ID to a new ID.");
    rejectWithoutMutation(
        std::make_unique<studio::SetSongSectionCommand>(middle, middle),
        "Editing an absent marker reports failure without changing the project.");
    rejectWithoutMutation(
        std::make_unique<studio::RemoveSongSectionCommand>(middle.id),
        "Removing an absent marker reports failure without changing the project.");
    rejectWithoutMutation(
        std::make_unique<studio::RemoveSongSectionCommand>(juce::String()),
        "Removing an empty marker ID reports failure without changing the project.");
    expect(commands.redo(project, error)
               && matchesSongSections(project, { start, middle, end }),
           "Rejected marker edits preserve the redo branch and its stable marker ID.");

    auto boundaryProject = studio::Project::createDefault();
    auto zero = start;
    zero.timeSeconds = 0.0;
    auto adjacent = end;
    adjacent.timeSeconds = 0.0001;
    studio::CommandStack boundaryCommands;
    expect(boundaryCommands.perform(
               std::make_unique<studio::AddSongSectionCommand>(zero),
               boundaryProject,
               error)
               && boundaryCommands.perform(
                   std::make_unique<studio::AddSongSectionCommand>(adjacent),
                   boundaryProject,
                   error)
               && matchesSongSections(boundaryProject, { zero, adjacent }),
           "Zero is a valid marker time and the existing exclusive duplicate tolerance is preserved.");
}

juce::File createLoopSource()
{
    const auto sourceFile = juce::File::getSpecialLocation(
                                juce::File::tempDirectory)
                                .getNonexistentChildFile(
                                    "StudioDuoTransport",
                                    ".wav",
                                    false);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream =
        sourceFile.createOutputStream();
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(1)
            .withBitsPerSample(24));
    juce::AudioBuffer<float> source(1, 100);
    for (int sample = 0; sample < source.getNumSamples(); ++sample)
        source.setSample(0, sample, 0.2f);
    expect(writer != nullptr
               && writer->writeFromAudioSampleBuffer(
                   source,
                   0,
                   source.getNumSamples()),
           "Transport loop source can be written.");
    if (writer != nullptr)
        writer->flush();
    return sourceFile;
}

struct RoutedMidiFixture
{
    studio::Project project;
    juce::String sourceTrackId;
    juce::String destinationTrackId;
};

RoutedMidiFixture createRoutedMidiFixture(
    double noteStartSeconds,
    double noteEndSeconds,
    std::optional<double> loopEndSeconds = std::nullopt,
    bool includePressureExpressions = false,
    bool includeSustainExpression = false)
{
    RoutedMidiFixture fixture {
        studio::Project::createDefault(),
        {},
        {}
    };
    fixture.project.metronomeEnabled = false;
    if (loopEndSeconds.has_value())
    {
        fixture.project.loopEnabled = true;
        fixture.project.loopStartSeconds = 0.0;
        fixture.project.loopEndSeconds = *loopEndSeconds;
    }

    studio::Track source;
    source.name = "Scheduled MIDI source";
    source.type = studio::TrackType::midi;
    fixture.sourceTrackId = source.id;
    fixture.project.tracks.insert(
        fixture.project.tracks.end() - 1,
        source);

    studio::Track destination;
    destination.name = "Scheduled MIDI destination";
    destination.type = studio::TrackType::instrument;
    fixture.destinationTrackId = destination.id;
    fixture.project.tracks.insert(
        fixture.project.tracks.end() - 1,
        destination);

    studio::RoutingConnection route;
    route.name = "Scheduled MIDI route";
    route.signalType = studio::SignalType::midi;
    route.kind = studio::RouteKind::mainOutput;
    route.sourceTrackId = fixture.sourceTrackId;
    route.destination.type = studio::RouteEndpointType::track;
    route.destination.trackId = fixture.destinationTrackId;
    fixture.project.routingConnections.push_back(route);

    studio::MidiClip clip;
    clip.name = "Scheduled MIDI";
    clip.durationBeats = fixture.project.beatsAt(8.0);
    studio::MidiNote note;
    note.pitch = 60;
    note.channel = 2;
    note.startBeats =
        fixture.project.beatsAt(noteStartSeconds);
    note.durationBeats =
        fixture.project.beatsAt(noteEndSeconds)
        - note.startBeats;
    if (includePressureExpressions)
    {
        studio::MidiExpressionPoint keyPressure;
        keyPressure.type =
            studio::MidiExpressionType::pressure;
        keyPressure.offsetBeats =
            fixture.project.beatsAt(noteStartSeconds + 0.001)
            - note.startBeats;
        keyPressure.value = 0.5;
        studio::MidiExpressionPoint channelPressure;
        channelPressure.type =
            studio::MidiExpressionType::channelPressure;
        channelPressure.offsetBeats =
            fixture.project.beatsAt(noteStartSeconds + 0.002)
            - note.startBeats;
        channelPressure.value = 0.75;
        note.expressions = {
            keyPressure,
            channelPressure
        };
    }
    if (includeSustainExpression)
    {
        studio::MidiExpressionPoint sustain;
        sustain.type = studio::MidiExpressionType::controller;
        sustain.controller = 64;
        sustain.offsetBeats =
            fixture.project.beatsAt(noteStartSeconds + 0.001)
            - note.startBeats;
        sustain.value = 1.0;
        note.expressions.push_back(sustain);
    }
    clip.notes.push_back(note);
    fixture.project.findTrack(fixture.sourceTrackId)
        ->midiClips.push_back(clip);
    if (includePressureExpressions)
    {
        studio::AutomationLane channelPressure;
        channelPressure.name = "Track channel pressure";
        channelPressure.target.type =
            studio::AutomationTargetType::midiChannelPressure;
        channelPressure.target.trackId = fixture.sourceTrackId;
        channelPressure.target.midiChannel = 3;
        channelPressure.timebase =
            studio::AutomationTimebase::seconds;
        channelPressure.interpolation =
            studio::AutomationInterpolation::step;
        channelPressure.points.push_back({
            juce::Uuid().toString(),
            noteStartSeconds + 0.003,
            0.25
        });
        fixture.project.automationLanes.push_back(
            std::move(channelPressure));
    }
    return fixture;
}

juce::MidiMessage traceMessage(
    const studio::StudioAudioEngine::MidiEventForTesting& event)
{
    return juce::MidiMessage(
        event.data.data(),
        static_cast<int>(event.size),
        0.0);
}

int traceCount(
    const std::vector<studio::StudioAudioEngine::MidiEventForTesting>& events,
    std::uint64_t runtimeKey,
    const std::function<bool(const juce::MidiMessage&)>& predicate)
{
    return static_cast<int>(std::count_if(
        events.cbegin(),
        events.cend(),
        [runtimeKey, &predicate](const auto& event)
        {
            return event.runtimeKey == runtimeKey
                && predicate(traceMessage(event));
        }));
}

bool hasTermination(
    const std::vector<studio::StudioAudioEngine::MidiEventForTesting>& events,
    std::uint64_t runtimeKey)
{
    return traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isNoteOff();
               })
            > 0
        && traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isAllNotesOff();
               })
            > 0
        && traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isSustainPedalOff();
               })
            > 0
        && traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isAllSoundOff();
               })
            > 0;
}

bool hasSafeChannelReset(
    const std::vector<studio::StudioAudioEngine::MidiEventForTesting>& events,
    std::uint64_t runtimeKey)
{
    return traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isSustainPedalOff();
               })
            > 0
        && traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isAllNotesOff();
               })
            > 0
        && traceCount(
               events,
               runtimeKey,
               [](const auto& message)
               {
                   return message.isAllSoundOff();
               })
            > 0;
}

void midiTransportDiscontinuities()
{
    {
        auto fixture = createRoutedMidiFixture(0.0, 4.0);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Snapshot handoff fixture publishes.");
        engine.seekSeconds(0.0);
        engine.play();
        engine.processActiveBlockForTesting(64);

        auto editedProject = fixture.project;
        editedProject.name = "Snapshot-only playback edit";
        editedProject.routingConnections.front().enabled = false;
        expect(engine.updateProject(editedProject).wasOk(),
               "A playback edit reuses the active runtime.");
        engine.clearMidiEventsForTesting();
        engine.pause();
        engine.processActiveBlockForTesting(64);
        const auto paused = engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        expect(hasTermination(paused, sourceKey)
                   && hasTermination(paused, destinationKey),
               "A snapshot-only routing edit preserves active scheduled notes at their original destination for later pause termination.");
    }

    {
        auto fixture = createRoutedMidiFixture(
            0.0,
            0.002,
            std::nullopt,
            false,
            true);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Sustain pause fixture publishes.");
        engine.seekSeconds(0.0);
        engine.play();
        engine.processActiveBlockForTesting(256);
        engine.clearMidiEventsForTesting();
        engine.pause();
        engine.processActiveBlockForTesting(64);
        const auto paused = engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        expect(hasSafeChannelReset(paused, sourceKey)
                   && hasSafeChannelReset(paused, destinationKey),
               "Pause releases a sustained voice after its scheduled note-off at both MIDI destinations.");
    }

    {
        auto fixture = createRoutedMidiFixture(
            0.0,
            0.002,
            std::nullopt,
            false,
            true);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Sustain seek fixture publishes.");
        engine.seekSeconds(0.0);
        engine.play();
        engine.processActiveBlockForTesting(256);
        engine.clearMidiEventsForTesting();
        engine.seekSeconds(1.0);
        engine.processActiveBlockForTesting(64);
        const auto sought = engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        expect(hasSafeChannelReset(sought, sourceKey)
                   && hasSafeChannelReset(sought, destinationKey),
               "Seeking releases a sustained voice after its scheduled note-off at both MIDI destinations.");
        engine.processActiveBlockForTesting(64);
        const auto soughtAgain =
            engine.takeMidiEventsForTesting();
        expect(!hasSafeChannelReset(soughtAgain, sourceKey)
                   && !hasSafeChannelReset(
                       soughtAgain,
                       destinationKey),
               "Seeking clears sustained scheduled state without duplicate resets.");
    }

    {
        auto fixture = createRoutedMidiFixture(
            0.0,
            4.0,
            std::nullopt,
            true);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Pressure playback fixture publishes.");
        engine.seekSeconds(0.0);
        engine.clearMidiEventsForTesting();
        engine.play();
        engine.processActiveBlockForTesting(256);
        const auto played = engine.takeMidiEventsForTesting();
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        expect(traceCount(
                   played,
                   destinationKey,
                   [](const auto& message)
                   {
                       return message.isAftertouch();
                   })
                       == 1
                   && traceCount(
                          played,
                          destinationKey,
                          [](const auto& message)
                          {
                              return message.isChannelPressure();
                          })
                          == 2
                   && traceCount(
                          played,
                          destinationKey,
                          [](const auto& message)
                          {
                              return message.isChannelPressure()
                                  && message.getChannel() == 3
                                  && message
                                         .getChannelPressureValue()
                                      == 32;
                          })
                          == 1,
               "Playback emits polyphonic key pressure, per-note channel pressure, and track-level channel-scoped pressure as distinct MIDI messages.");

        engine.clearMidiEventsForTesting();
        engine.pause();
        engine.processActiveBlockForTesting(64);
        const auto paused = engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        expect(hasTermination(paused, sourceKey)
                   && hasTermination(paused, destinationKey),
               "Pause terminates active scheduled notes at both the source and routed destination.");
        engine.processActiveBlockForTesting(64);
        const auto pausedAgain =
            engine.takeMidiEventsForTesting();
        expect(!hasTermination(pausedAgain, sourceKey)
                   && !hasTermination(pausedAgain, destinationKey),
               "Pause clears scheduled note state without duplicate termination events.");
    }

    {
        auto fixture = createRoutedMidiFixture(0.0, 4.0);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Seek termination fixture publishes.");
        engine.seekSeconds(0.0);
        engine.play();
        engine.processActiveBlockForTesting(64);
        engine.clearMidiEventsForTesting();
        engine.seekSeconds(1.0);
        engine.processActiveBlockForTesting(64);
        const auto sought = engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        expect(hasTermination(sought, sourceKey)
                   && hasTermination(sought, destinationKey),
               "Seeking mid-note terminates the old scheduled voice through its original route.");
        engine.processActiveBlockForTesting(64);
        const auto soughtAgain =
            engine.takeMidiEventsForTesting();
        expect(!hasTermination(soughtAgain, sourceKey)
                   && !hasTermination(soughtAgain, destinationKey),
               "Seeking leaves no stale scheduled-note state to terminate twice.");
    }

    {
        constexpr auto loopEndSeconds = 100.0 / 48000.0;
        auto fixture = createRoutedMidiFixture(
            0.0,
            1.0,
            loopEndSeconds);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Loop termination fixture publishes.");
        engine.seekSeconds(0.0);
        engine.clearMidiEventsForTesting();
        engine.play();
        engine.processActiveBlockForTesting(150);
        const auto looped = engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        const auto sourceNoteOns = traceCount(
            looped,
            sourceKey,
            [](const auto& message)
            {
                return message.isNoteOn();
            });
        const auto destinationNoteOns = traceCount(
            looped,
            destinationKey,
            [](const auto& message)
            {
                return message.isNoteOn();
            });
        expect(sourceNoteOns == 2
                   && destinationNoteOns == 2
                   && hasTermination(looped, sourceKey)
                   && hasTermination(looped, destinationKey),
               "A mid-note loop wrap terminates the old voice before scheduling the wrapped note on every route.");
        engine.clearMidiEventsForTesting();
        engine.pause();
        engine.processActiveBlockForTesting(64);
        const auto loopPaused =
            engine.takeMidiEventsForTesting();
        expect(hasTermination(loopPaused, sourceKey)
                   && hasTermination(loopPaused, destinationKey),
               "The wrapped replacement note remains tracked exactly once for later pause termination.");
    }

    {
        auto fixture = createRoutedMidiFixture(7.99, 8.0);
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(fixture.project).wasOk(),
               "Automatic-end termination fixture publishes.");
        engine.seekSeconds(7.99);
        engine.clearMidiEventsForTesting();
        engine.play();
        engine.processActiveBlockForTesting(480);
        const auto endingBlock =
            engine.takeMidiEventsForTesting();
        const auto sourceKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.sourceTrackId);
        const auto destinationKey =
            studio::StudioAudioEngine::runtimeKeyForTesting(
                fixture.destinationTrackId);
        expect(!engine.isPlaying()
                   && traceCount(
                          endingBlock,
                          destinationKey,
                          [](const auto& message)
                          {
                              return message.isNoteOn();
                          })
                          == 1
                   && !hasTermination(
                       endingBlock,
                       destinationKey),
               "The automatic-end fixture leaves a boundary note active until transport termination runs.");
        engine.processActiveBlockForTesting(64);
        const auto ended = engine.takeMidiEventsForTesting();
        expect(hasTermination(ended, sourceKey)
                   && hasTermination(ended, destinationKey),
               "Automatic transport end terminates boundary notes through every destination.");
        engine.processActiveBlockForTesting(64);
        const auto endedAgain =
            engine.takeMidiEventsForTesting();
        expect(!hasTermination(endedAgain, sourceKey)
                   && !hasTermination(endedAgain, destinationKey),
               "Automatic transport end clears active state without repeated note termination.");
    }
}
}

void transportTests()
{
    namedSongMarkerCommands();
    invalidSongMarkers();

    auto project = studio::Project::createDefault();
    project.name = "Transport verification";
    project.tempo = 110.0;
    project.timeSignatureNumerator = 5;
    project.timeSignatureDenominator = 4;
    project.tempoChanges = {
        { 0.0, 110.0, true },
        { 3.0, 170.0, false }
    };
    project.meterChanges = {
        { 0.0, 5, 4 },
        { 6.0, 7, 8 }
    };
    project.metronomeEnabled = true;
    project.metronomeSubdivision = 3;
    project.metronomeOutputChannel = 2;
    project.metronomeLevel = 0.5f;
    project.metronomeAccentLevel = 0.9f;
    project.punchEnabled = true;
    project.punchInSeconds = 8.0;
    project.punchOutSeconds = 10.0;
    project.countInBars = 2;
    project.preRollSeconds = 0.5;
    project.postRollSeconds = 1.0;
    project.loopEnabled = true;
    project.loopStartSeconds = 2.0;
    project.loopEndSeconds = 12.0;

    juce::String error;
    expect(project.validateTransport(error), error.toRawUTF8());
    expect(closeTo(project.timelineEndSeconds(), 12.0),
           "Timeline extent includes transport ranges beyond existing clips.");

    const auto package = juce::File::getSpecialLocation(
                             juce::File::tempDirectory)
                             .getNonexistentChildFile(
                                 "StudioDuoTransport",
                                 ".studioduo",
                                 false);
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "Transport project can be saved.");
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value(), error.toRawUTF8());
    expect(loaded.has_value()
               && loaded->tempoChanges == project.tempoChanges
               && loaded->meterChanges == project.meterChanges
               && loaded->metronomeSubdivision
                      == project.metronomeSubdivision
               && loaded->metronomeOutputChannel
                      == project.metronomeOutputChannel
               && closeTo(loaded->metronomeLevel,
                          project.metronomeLevel)
               && closeTo(loaded->metronomeAccentLevel,
                          project.metronomeAccentLevel)
               && loaded->punchEnabled
               && closeTo(loaded->punchInSeconds,
                          project.punchInSeconds)
               && closeTo(loaded->punchOutSeconds,
                          project.punchOutSeconds)
               && loaded->countInBars == project.countInBars
               && closeTo(loaded->preRollSeconds,
                          project.preRollSeconds)
               && closeTo(loaded->postRollSeconds,
                          project.postRollSeconds)
               && loaded->loopEnabled
               && closeTo(loaded->loopStartSeconds,
                          project.loopStartSeconds)
               && closeTo(loaded->loopEndSeconds,
                          project.loopEndSeconds),
           "Tempo, meter, metronome, punch, roll, and loop data survive save and reopen.");

    auto duplicateTempo = project;
    duplicateTempo.tempoChanges.push_back({ 3.0, 180.0, false });
    error.clear();
    expect(!duplicateTempo.validateTransport(error)
               && error.containsIgnoreCase("unique"),
           "Transport validation rejects duplicate tempo-map positions.");

    auto invalidValue = project.toVar();
    invalidValue.getDynamicObject()->setProperty(
        "punchOutSeconds",
        project.punchInSeconds);
    error.clear();
    expect(!studio::Project::fromVar(invalidValue, error).has_value()
               && error.containsIgnoreCase("punch"),
           "Project loading rejects invalid punch ranges instead of normalizing them.");

    auto invalidState =
        studio::ProjectTransportState::fromProject(project);
    invalidState.metronomeSubdivision = 9;
    studio::CommandStack commands;
    error.clear();
    expect(!commands.perform(
               std::make_unique<studio::SetProjectTransportCommand>(
                   studio::ProjectTransportState::fromProject(project),
                   invalidState),
               project,
               error)
               && error.containsIgnoreCase("metronome"),
           "Undoable transport edits reject invalid metronome settings.");

    auto punchProject = studio::Project::createDefault();
    punchProject.tempo = 120.0;
    punchProject.punchEnabled = true;
    punchProject.punchInSeconds = 8.0;
    punchProject.punchOutSeconds = 10.0;
    punchProject.countInBars = 1;
    punchProject.preRollSeconds = 0.5;
    punchProject.postRollSeconds = 1.0;
    const auto punchPlan = punchProject.recordingPlan(9.0);
    expect(closeTo(punchPlan.transportStartSeconds, 5.5)
               && closeTo(punchPlan.captureStartSeconds, 8.0)
               && closeTo(punchPlan.captureEndSeconds, 10.0)
               && closeTo(punchPlan.transportEndSeconds, 11.0),
           "Count-in, pre-roll, punch, and post-roll produce exact transport boundaries.");
    punchProject.loopEnabled = true;
    punchProject.loopStartSeconds = 4.0;
    punchProject.loopEndSeconds = 12.0;
    expect(!punchProject.recordingPlan(9.0).loopEnabled,
           "Punch recording takes priority over loop recording when both are enabled.");

    const auto enteringPunch =
        studio::recordingCaptureRange(90, 20, 100, 200);
    const auto leavingPunch =
        studio::recordingCaptureRange(190, 20, 100, 200);
    const auto afterPunch =
        studio::recordingCaptureRange(210, 20, 100, 200);
    expect(enteringPunch.sourceOffset == 10
               && enteringPunch.samples == 10
               && leavingPunch.sourceOffset == 0
               && leavingPunch.samples == 10
               && afterPunch.samples == 0,
           "Punch capture clips callback blocks at sample-accurate in/out boundaries.");

    studio::RecordingPlan invalidPlan;
    invalidPlan.transportStartSeconds = 2.0;
    invalidPlan.captureStartSeconds = 1.0;
    studio::StudioAudioEngine invalidPlanEngine;
    const auto invalidPlanResult =
        invalidPlanEngine.startRecording({}, invalidPlan);
    expect(invalidPlanResult.failed()
               && invalidPlanResult.getErrorMessage()
                      .containsIgnoreCase("transport range"),
           "Recording plans are validated before devices or files are touched.");

    auto clickProject = studio::Project::createDefault();
    clickProject.metronomeSubdivision = 3;
    clickProject.metronomeOutputChannel = 2;
    clickProject.metronomeLevel = 0.25f;
    clickProject.metronomeAccentLevel = 1.0f;
    clickProject.meterChanges = {
        { 0.25, 3, 4 }
    };
    studio::StudioAudioEngine clickEngine;
    expect(clickEngine.updateProject(clickProject).wasOk(),
           "Metronome project publishes for real-time playback.");
    clickEngine.seekSeconds(0.0);
    clickEngine.play();
    const auto clickBlock =
        clickEngine.renderActiveBlockForTesting(14000, 4);
    const auto firstAccent = clickBlock.getMagnitude(2, 0, 800);
    const auto subdivisionClick =
        clickBlock.getMagnitude(2, 8000, 800);
    const auto meterAccent =
        clickBlock.getMagnitude(2, 12000, 800);
    expect(clickBlock.getMagnitude(0, 0, 14000) < 0.000001f
               && clickBlock.getMagnitude(1, 0, 14000) < 0.000001f
               && firstAccent > 0.1f
               && subdivisionClick > 0.01f
               && firstAccent > subdivisionClick * 2.0f
               && meterAccent > subdivisionClick * 2.0f
               && clickBlock.getMagnitude(3, 0, 14000) > 0.1f,
           "Metronome routing, triplet subdivisions, and meter-change accents render on the selected output pair.");

    juce::AudioBuffer<float> exportedClick;
    expect(clickEngine.renderToBuffer(
               clickProject,
               exportedClick,
               48000.0)
               .wasOk()
               && exportedClick.getMagnitude(
                      0,
                      0,
                      exportedClick.getNumSamples())
                      < 0.000001f
               && exportedClick.getMagnitude(
                      1,
                      0,
                      exportedClick.getNumSamples())
                      < 0.000001f,
           "Metronome routing stays out of final buffer renders.");

    auto sectionProject = studio::Project::createDefault();
    studio::SongSection silentSection { "silent-section", "Muted click", 0.0 };
    silentSection.clickSettings = studio::SectionClickSettings {};
    silentSection.clickSettings->enabled = false;
    studio::SongSection genericMarker { "neutral-marker", "Export marker", 0.5 };
    studio::SongSection activeSection { "active-section", "Three eighths", 1.0 };
    sectionProject.sections = { silentSection, genericMarker, activeSection };
    studio::SectionTransportSettings sectionSettings;
    sectionSettings.tempoBpm = 180.0;
    sectionSettings.timeSignature = studio::SectionTimeSignature { 3, 8 };
    sectionSettings.clickSettings = studio::SectionClickSettings {};
    sectionSettings.clickSettings->subdivision = 2;
    sectionSettings.clickSettings->level = 0.1f;
    sectionSettings.clickSettings->accentBeats = { 1, 3 };
    studio::SetSectionTransportCommand sectionCommand(activeSection.id, sectionSettings);
    juce::String sectionError;
    expect(sectionCommand.perform(sectionProject, sectionError),
           ("Section transport fixture configures 180 BPM, 3/8 and custom accents: "
            + sectionError).toRawUTF8());
    studio::StudioAudioEngine sectionEngine;
    expect(sectionEngine.updateProject(sectionProject).wasOk(),
           "Section-aware metronome publishes a playback snapshot.");
    sectionEngine.seekSeconds(0.0);
    sectionEngine.play();
    const auto sectionClick = sectionEngine.renderActiveBlockForTesting(73000);
    const auto sectionAccent = sectionClick.getMagnitude(0, 48000, 800);
    const auto sectionSubdivision = sectionClick.getMagnitude(0, 52000, 800);
    const auto secondBeat = sectionClick.getMagnitude(0, 56000, 800);
    const auto thirdAccent = sectionClick.getMagnitude(0, 64000, 800);
    expect(sectionClick.getMagnitude(0, 0, 48000) < 0.000001f,
           "A muted section stays silent across an unconfigured marker.");
    expect(sectionAccent > 0.1f && thirdAccent > 0.1f
               && sectionSubdivision > 0.005f && secondBeat > 0.005f
               && sectionAccent > sectionSubdivision * 5.0f
               && thirdAccent > secondBeat * 5.0f,
           "Section tempo, 3/8 meter, subdivisions and selected accented beats drive the audible click.");

    sectionProject.loopEnabled = true;
    sectionProject.loopStartSeconds = 0.95;
    sectionProject.loopEndSeconds = 1.1;
    expect(sectionEngine.updateProject(sectionProject).wasOk(),
           "A configurable loop can cross a section click change.");
    sectionEngine.seekSeconds(1.08);
    sectionEngine.play();
    const auto loopedClick = sectionEngine.renderActiveBlockForTesting(5000);
    expect(loopedClick.getMagnitude(0, 1100, 2000) < 0.000001f
               && loopedClick.getMagnitude(0, 3360, 800) > 0.1f,
           "Loop wrap re-evaluates section click state and restores accents at the section boundary.");

    sectionProject.metronomeEnabled = false;
    expect(sectionEngine.updateProject(sectionProject).wasOk(),
           "The global click master gate updates with section overrides.");
    sectionEngine.seekSeconds(1.0);
    sectionEngine.play();
    expect(sectionEngine.renderActiveBlockForTesting(800).getMagnitude(0, 800) < 0.000001f,
           "Section click enablement cannot bypass the global CLICK-off control.");
    sectionProject.metronomeEnabled = true;
    juce::AudioBuffer<float> sectionExport;
    expect(sectionEngine.renderToBuffer(sectionProject, sectionExport, 48000.0).wasOk()
               && sectionExport.getMagnitude(0, sectionExport.getNumSamples()) < 0.000001f,
           "Per-section click patterns never leak into audio exports.");

    const auto loopSource = createLoopSource();
    auto loopProject = studio::Project::createDefault();
    loopProject.metronomeEnabled = false;
    loopProject.loopEnabled = true;
    loopProject.loopStartSeconds = 0.0;
    loopProject.loopEndSeconds = 100.0 / 48000.0;
    studio::AudioClip loopClip;
    loopClip.sourceFile = loopSource;
    loopClip.durationSeconds = loopProject.loopEndSeconds;
    loopClip.sourceLengthSeconds = loopClip.durationSeconds;
    loopClip.sourceRangeEndSeconds = loopClip.durationSeconds;
    loopProject.tracks.front().clips.push_back(loopClip);

    studio::StudioAudioEngine loopEngine;
    juce::AudioBuffer<float> offlineLoop;
    expect(loopEngine.renderToBuffer(
               loopProject,
               offlineLoop,
               48000.0)
               .wasOk()
               && offlineLoop.getSample(0, 25) > 0.19f
               && offlineLoop.getSample(0, 125) > 0.19f
               && offlineLoop.getSample(0, 225) > 0.19f,
           "Plugin-free buffer rendering follows the same loop boundaries as real-time playback.");

    expect(loopEngine.updateProject(loopProject).wasOk(),
           "Loop project publishes for real-time playback.");
    loopEngine.seekSeconds(0.0);
    loopEngine.play();
    const auto realtimeLoop =
        loopEngine.renderActiveBlockForTesting(250);
    expect(realtimeLoop.getSample(0, 25) > 0.19f
               && realtimeLoop.getSample(0, 125) > 0.19f
               && realtimeLoop.getSample(0, 225) > 0.19f,
           "Real-time playback wraps audio at mid-block loop boundaries.");

    auto extendedLoop = studio::Project::createDefault();
    extendedLoop.metronomeEnabled = false;
    extendedLoop.loopEnabled = true;
    extendedLoop.loopStartSeconds = 7.99;
    extendedLoop.loopEndSeconds = 8.01;
    studio::StudioAudioEngine extendedLoopEngine;
    expect(extendedLoopEngine.updateProject(extendedLoop).wasOk(),
           "Extended loop project publishes.");
    extendedLoopEngine.seekSeconds(7.999);
    extendedLoopEngine.play();
    extendedLoopEngine.processActiveBlockForTesting(200);
    expect(extendedLoopEngine.isPlaying()
               && extendedLoopEngine.positionSeconds() > 8.0,
           "Loop playback can cross the current content end before wrapping.");

    midiTransportDiscontinuities();

    loopSource.deleteFile();
    package.deleteRecursively();
}
