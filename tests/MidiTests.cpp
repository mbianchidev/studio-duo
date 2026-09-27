#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "midi/MidiCaptureBuffer.h"
#include "midi/MidiEditing.h"
#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"
#include "ui/MidiEditorComponent.h"
#include "ui/DrumPerformanceComponent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <thread>

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
    studio::MidiExpressionPoint channelPressure;
    channelPressure.type =
        studio::MidiExpressionType::channelPressure;
    channelPressure.offsetBeats = 0.375;
    channelPressure.value = 0.45;
    note.expressions = { pressure, pitch, channelPressure };
    clip.notes.push_back(note);
    return clip;
}

const studio::MidiNote* findNote(const studio::MidiClip& clip,
                                 const juce::String& noteId)
{
    const auto note = std::find_if(
        clip.notes.cbegin(),
        clip.notes.cend(),
        [&noteId](const auto& candidate)
        {
            return candidate.id == noteId;
        });
    return note == clip.notes.cend() ? nullptr : &*note;
}

studio::CapturedMidiEvent capturedEvent(
    std::uint64_t ordinal,
    std::int64_t streamSample,
    const juce::MidiMessage& message)
{
    studio::CapturedMidiEvent event;
    event.ordinal = ordinal;
    event.streamSample = streamSample;
    event.timelineSample = streamSample;
    event.size = static_cast<std::uint8_t>(message.getRawDataSize());
    std::copy_n(
        message.getRawData(),
        message.getRawDataSize(),
        event.data.begin());
    return event;
}

void midiPersistence()
{
    auto project = studio::Project::createDefault();
    auto track = makeMidiTrack(project);
    auto clip = makeMidiClip(project);
    clip.drumPadBindings = studio::defaultDrumPadBindings(&project.drumMaps.front());
    clip.drumPadBindings[8] = { 52, 'B' };
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
               && decodedClip->notes.front().expressions.size() == 3
               && decodedClip->notes.front().expressions[0].type
                      == studio::MidiExpressionType::pressure
               && decodedClip->notes.front().expressions[2].type
                      == studio::MidiExpressionType::channelPressure
               && decodedClip->drumMapId == clip.drumMapId,
           "MIDI clips, polyphonic pressure, channel pressure, stable IDs, and drum-map references survive model serialization.");
    expect(decoded.has_value()
               && !decoded->drumMaps.empty()
               && !decoded->midiPatterns.empty()
               && !decoded->midiRoutingTemplates.empty(),
           "Drum maps, pattern aliases, and routing templates survive model serialization.");
    const auto legacyPressure =
        studio::midiExpressionTypeFromString("pressure");
    const auto distinctChannelPressure =
        studio::midiExpressionTypeFromString(
            "channelPressure");
    expect(legacyPressure
                   == studio::MidiExpressionType::pressure
               && distinctChannelPressure
                   == studio::MidiExpressionType::
                       channelPressure,
           "Existing pressure data remains polyphonic key pressure while channel pressure has a distinct persisted value.");

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
               && loadedClip->drumPadBindings == clip.drumPadBindings
               && loaded->findDrumMap(clip.drumMapId) != nullptr,
           "Native project generations restore exact MIDI notes, expression IDs, maps, and custom drum-pad assignments.");
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

void sustainedCaptureFinalization()
{
    auto project = studio::Project::createDefault();
    project.tempo = 60.0;
    project.tempoChanges = {
        { 0.0, 60.0, false },
        { 0.5, 120.0, false }
    };

    constexpr auto sampleRate = 1000.0;
    constexpr auto captureStartSample = std::int64_t { 100 };
    constexpr auto timelineStartSeconds = 0.25;
    constexpr auto captureDurationSeconds = 1.0;
    studio::CapturedMidiWindow captured;
    captured.events = {
        capturedEvent(
            0,
            captureStartSample,
            juce::MidiMessage::noteOn(
                1,
                60,
                static_cast<juce::uint8>(100))),
        capturedEvent(
            1,
            captureStartSample + 750,
            juce::MidiMessage::aftertouchChange(1, 60, 96)),
        capturedEvent(
            2,
            captureStartSample + 450,
            juce::MidiMessage::pitchWheel(1, 10000)),
        capturedEvent(
            3,
            captureStartSample + 1000,
            juce::MidiMessage::noteOn(
                1,
                62,
                static_cast<juce::uint8>(90)))
    };
    captured.lastOrdinal = captured.events.size();

    juce::String error;
    const auto converted = studio::convertCapturedMidiToClip(
        project,
        captured,
        captureStartSample,
        timelineStartSeconds,
        captureDurationSeconds,
        sampleRate,
        nullptr,
        "Held capture",
        error);
    expect(converted.has_value(), error.toRawUTF8());
    const auto* note =
        converted.has_value() && converted->clip.notes.size() == 1
        ? &converted->clip.notes.front()
        : nullptr;
    const auto expectedDuration =
        project.beatsAt(timelineStartSeconds + captureDurationSeconds)
        - project.beatsAt(timelineStartSeconds);
    const auto earlierOffset =
        project.beatsAt(timelineStartSeconds + 0.45)
        - project.beatsAt(timelineStartSeconds);
    const auto laterOffset =
        project.beatsAt(timelineStartSeconds + 0.75)
        - project.beatsAt(timelineStartSeconds);
    expect(note != nullptr
               && converted->ignoredEvents == 1
               && std::abs(
                      converted->clip.durationBeats
                      - expectedDuration)
                      < 0.0000001
               && std::abs(note->durationBeats - expectedDuration)
                      < 0.0000001
               && note->expressions.size() == 2
               && std::abs(
                      note->expressions[0].offsetBeats
                      - earlierOffset)
                      < 0.0000001
               && std::abs(
                      note->expressions[1].offsetBeats
                      - laterOffset)
                      < 0.0000001,
           "Held notes close at the tempo-aware capture end after raw expression offsets are sorted and clamped.");
}

void drumPadBindingsPersistence()
{
    auto clip = studio::MidiClip {};
    auto encoded = clip.toVar();
    juce::Array<juce::var> bindings;
    const juce::String keys = "QWERASDFZXCV";
    for (int index = 0; index < keys.length(); ++index)
    {
        auto binding = std::make_unique<juce::DynamicObject>();
        binding->setProperty("noteNumber", 36 + index);
        binding->setProperty("keyCode", static_cast<int>(keys[index]));
        bindings.add(juce::var(binding.release()));
    }
    encoded.getDynamicObject()->setProperty("drumPadBindings", bindings);

    juce::String error;
    const auto restored = studio::MidiClip::fromVar(encoded, error);
    expect(restored.has_value()
               && juce::JSON::toString(
                      restored->toVar().getProperty("drumPadBindings", {}),
                      false)
                   == juce::JSON::toString(juce::var(bindings), false),
           "Custom drum-pad sounds and keyboard assignments survive MIDI clip serialization.");

    auto legacy = clip.toVar();
    legacy.getDynamicObject()->removeProperty("drumPadBindings");
    expect(studio::MidiClip::fromVar(legacy, error).has_value(),
           "Existing MIDI clips without pad settings remain loadable.");
    auto invalid = encoded.clone();
    auto* invalidPads = invalid.getDynamicObject()
                            ->getProperty("drumPadBindings").getArray();
    (*invalidPads)[1].getDynamicObject()->setProperty("keyCode", 'Q');
    expect(!studio::MidiClip::fromVar(invalid, error).has_value()
               && error.isNotEmpty(),
           "Duplicate drum keyboard bindings fail validation instead of shadowing a pad.");
    invalid = encoded.clone();
    invalid.getDynamicObject()->getProperty("drumPadBindings")
        .getArray()->getReference(0).getDynamicObject()
        ->setProperty("noteNumber", static_cast<juce::int64>(0x100000024LL));
    expect(!studio::MidiClip::fromVar(invalid, error).has_value(),
           "Oversized drum note values cannot wrap into valid MIDI pitches.");
}

void drumPerformanceKeyboard()
{
    auto project = studio::Project::createDefault();
    auto clip = makeMidiClip(project);
    const auto* map = project.findDrumMap(clip.drumMapId);
    studio::DrumPerformanceComponent pads;
    std::vector<juce::MidiMessage> messages;
    std::vector<juce::String> destinations;
    pads.onMidiMessage = [&](const auto& trackId, const auto& message)
    {
        destinations.push_back(trackId);
        messages.push_back(message);
        return juce::Result::ok();
    };
    pads.onBindingsEdited = [&](const auto& bindings)
    {
        clip.drumPadBindings = bindings;
        return true;
    };
    pads.setContext("synthetic-drums", &clip, map);
    pads.setSize(760, 294);
    expect(!pads.keyPressed(juce::KeyPress('Z')) && messages.empty(),
           "Drum keys do not intercept normal editing until keyboard mode is enabled.");
    pads.setKeyboardEnabled(true);
    expect(pads.keyPressed(juce::KeyPress('Z'))
               && pads.keyPressed(juce::KeyPress('Z'))
               && pads.keyPressed(juce::KeyPress('D')),
           "Keyboard performance accepts simultaneous kick/snare keys and consumes key repeat.");
    expect(messages.size() == 2
               && messages[0].isNoteOn()
               && messages[0].getNoteNumber() == 36
               && messages[1].getNoteNumber() == 38,
           "Held-key repeats do not retrigger notes and chords retain both drum hits.");
    expect(!pads.keyPressed(juce::KeyPress(
               'S', juce::ModifierKeys::commandModifier, 0)),
           "Save and other Command/Ctrl shortcuts are not stolen by drum keys.");
    pads.keyStateChanged(false);
    expect(messages.size() == 4
               && messages[2].isNoteOff()
               && messages[3].isNoteOff(),
           "Key releases terminate each held note.");

    expect(pads.assignSound(8, 52).wasOk()
               && pads.assignKey(8, 'B').wasOk()
               && clip.drumPadBindings.size() == studio::drumPadCount,
           "Selecting a pad sound and binding a key persist through the edit callback.");
    expect(pads.assignKey(8, 'D').failed()
               && pads.assignKey(8, juce::KeyPress::spaceKey).failed()
               && pads.assignSound(8, 128).failed(),
           "Conflicting keys, transport keys, and invalid notes are rejected.");
    messages.clear();
    expect(pads.keyPressed(juce::KeyPress('Z'))
               && messages.empty()
               && pads.keyPressed(juce::KeyPress('B'))
               && messages.size() == 1
               && messages.front().getNoteNumber() == 52,
           "Rebound pads play only from their new key; unmapped letters cannot trigger arrangement shortcuts.");
    pads.setContext("synthetic-other-drums", &clip, map);
    expect(messages.size() == 2
               && messages.back().isNoteOff()
               && destinations.back() == "synthetic-drums"
               && !pads.isKeyboardEnabled(),
           "Changing tracks releases notes to their original track and disables keyboard capture.");
    pads.setKeyboardEnabled(true);
    pads.keyPressed(juce::KeyPress('B'));
    pads.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    expect(!pads.isKeyboardEnabled() && messages.back().isNoteOff(),
           "Escape releases held notes and exits keyboard mode.");

    pads.setKeyboardEnabled(true);
    messages.clear();
    pads.keyPressed(juce::KeyPress('S'));
    expect(messages.size() == 2
               && messages.front().isControllerOfType(4)
               && messages.front().getControllerValue() == 0
               && messages.back().getNoteNumber() == 46,
           "The open hi-hat sends its mapped foot control before the hit.");
    pads.releaseAllNotes();

    for (const auto width : { 560, 760, 1120 })
    {
        pads.setSize(width, 294);
        auto visiblePads = 0;
        for (auto* child : pads.getChildren())
        {
            if (!child->getComponentID().startsWith("drum-pad-"))
                continue;
            ++visiblePads;
            expect(pads.getLocalBounds().contains(child->getBounds())
                       && child->getWidth() >= 64
                       && child->getHeight() >= 40,
                   "All drum pads remain visible and usable at supported lower-editor widths.");
        }
        expect(visiblePads == static_cast<int>(studio::drumPadCount),
               "The drum performance view exposes twelve accessible pad buttons.");
    }
}

void targetedSoftwareMidi()
{
    auto project = studio::Project::createDefault();
    const auto drums = makeMidiTrack(project, "Synthetic drums");
    const auto keys = makeMidiTrack(project, "Synthetic keys");
    project.findTrack(drums.id)->armed = true;
    project.findTrack(keys.id)->armed = true;
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project).wasOk(),
           "A two-track live MIDI graph can be prepared.");
    for (auto attempt = 0;
         attempt < 200 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    engine.setMidiAuditionEnabled(true);
    studio::RecordingPlan plan;
    expect(engine.startRecording({}, plan, true).wasOk(),
           "Software drums reuse the ordinary MIDI recording session.");
    expect(engine.enqueueMidiInput(
               drums.id,
               juce::MidiMessage::noteOn(1, 36, juce::uint8(110))).wasOk(),
           "A software drum hit is admitted to its selected track.");
    juce::MidiBuffer hardware;
    hardware.addEvent(
        juce::MidiMessage::noteOn(2, 60, juce::uint8(90)), 8);
    engine.clearMidiEventsForTesting();
    juce::ignoreUnused(engine.renderActiveBlockWithMidiForTesting(hardware, 512));
    const auto routed = engine.takeMidiEventsForTesting();
    const auto receives = [&](const auto& trackId, int pitch)
    {
        return std::any_of(routed.cbegin(), routed.cend(), [&](const auto& event)
        {
            const juce::MidiMessage message(event.data.data(), event.size, 0.0);
            return event.runtimeKey
                    == studio::StudioAudioEngine::runtimeKeyForTesting(trackId)
                && message.isNoteOn() && message.getNoteNumber() == pitch;
        });
    };
    expect(receives(drums.id, 36) && !receives(keys.id, 36)
               && receives(drums.id, 60) && receives(keys.id, 60),
           "Software pad MIDI is track-specific while hardware input keeps its existing armed-track fan-out.");
    expect(engine.enqueueMidiInput(
               drums.id, juce::MidiMessage::noteOff(1, 36)).wasOk(),
           "Software drum releases use the same queue as note-ons.");
    hardware.clear();
    hardware.addEvent(juce::MidiMessage::noteOff(2, 60), 32);
    juce::ignoreUnused(engine.renderActiveBlockWithMidiForTesting(hardware, 512));
    auto recording = engine.stopMidiRecording();
    engine.stopRecording();
    juce::String error;
    const auto convert = [&](const juce::String& trackId)
    {
        return studio::convertCapturedMidiToClip(
            project, recording.captured, recording.captureStartStreamSample,
            recording.timelineStartSeconds, recording.durationSeconds,
            recording.sampleRate, nullptr, "Synthetic performance", error, trackId);
    };
    const auto drumTake = convert(drums.id);
    const auto keyTake = convert(keys.id);
    expect(drumTake.has_value() && drumTake->clip.notes.size() == 2
               && keyTake.has_value() && keyTake->clip.notes.size() == 1
               && keyTake->clip.notes.front().pitch == 60,
           "Recorded software drums do not leak into another armed instrument's MIDI clip.");
    const auto retrospective = engine.captureRetrospectiveMidi(30.0, keys.id);
    expect(retrospective.result.wasOk()
               && retrospective.captured.events.size() == 2,
           "Retrospective capture filters software MIDI before determining the selected track's range.");
    expect(engine.enqueueMidiInput(
               "missing-track", juce::MidiMessage::noteOff(1, 36)).failed(),
           "Software input to a missing track reports an error.");
}

void alternateDrumEditor()
{
    auto project = studio::Project::createDefault();
    const auto track = makeMidiTrack(project);
    studio::MidiClip clip;
    project.findTrack(track.id)->midiClips.push_back(clip);
    studio::MidiEditorComponent editor;
    editor.onClipEdited = [&](const auto&, const auto&, const auto& after, const auto&)
    {
        *project.findMidiClip(clip.id) = after;
        editor.setProject(&project);
    };
    editor.setProject(&project);
    editor.setSelection(track.id, clip.id);
    editor.setSize(760, 330);
    editor.showDrumPads();
    expect(editor.isShowingDrumPads()
               && project.findMidiClip(clip.id)->editorMode == studio::MidiEditorMode::drums
               && project.findMidiClip(clip.id)->notes.empty(),
           "Opening drum pads assigns the drum map without replacing or inserting MIDI notes.");
    expect(editor.drumPerformance().assignKey(8, 'B').wasOk()
               && project.findMidiClip(clip.id)->drumPadBindings[8].keyCode == 'B',
           "Pad assignments use the editor's ordinary undoable clip-edit callback.");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(project.findMidiClip(clip.id)->notes.empty(),
           "Pad-view keyboard input cannot insert notes into the hidden piano roll.");
    editor.showDrumPads(false);
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(project.findMidiClip(clip.id)->notes.size() == 1,
           "The existing MIDI grid's Enter-to-create workflow remains available after leaving pads.");
    auto& recorded = project.findMidiClip(clip.id)->notes.front();
    recorded.pitch = 47;
    studio::applyDrumMapMetadata(recorded, &project.drumMaps.front(), 1);
    for (auto* child : editor.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*>(child);
            button != nullptr && button->getButtonText() == "DRUMS")
        {
            button->onClick();
            break;
        }
    editor.showDrumPads();
    expect(project.findMidiClip(clip.id)->notes.front().pitch == 47
               && project.findMidiClip(clip.id)->notes.front().roundRobinHint == 1,
           "Switching a recorded round-robin sound through piano roll and pads does not change its playback pitch.");
    editor.showDrumPads(false);
    for (auto* child : editor.getChildren())
        if (auto* box = dynamic_cast<juce::ComboBox*>(child);
            box != nullptr && box->getText() == "Velocity")
            box->setSelectedId(5, juce::sendNotificationSync);
    auto expressionControlVisible = false;
    for (auto* child : editor.getChildren())
        if (auto* box = dynamic_cast<juce::ComboBox*>(child);
            box != nullptr && box->getText() == "Poly pressure")
            expressionControlVisible = box->isVisible() && box->getWidth() > 40;
    expect(expressionControlVisible,
           "The compact editor toolbar gives expression controls space when their lane is selected.");
}

void drumCaptureFidelity()
{
    auto project = studio::Project::createDefault();
    project.tempo = 60.0;
    project.tempoChanges = { { 0.0, 60.0, false }, { 0.5, 120.0, false } };
    studio::CapturedMidiWindow capture;
    capture.events = {
        capturedEvent(0, 300, juce::MidiMessage::noteOn(1, 38, juce::uint8(110))),
        capturedEvent(1, 400, juce::MidiMessage::noteOff(1, 38)),
        capturedEvent(2, 50, juce::MidiMessage::controllerEvent(1, 4, 27)),
        capturedEvent(3, 50, juce::MidiMessage::noteOn(1, 46, juce::uint8(92))),
        capturedEvent(4, 150, juce::MidiMessage::noteOff(1, 46)),
        capturedEvent(5, 450, juce::MidiMessage::noteOn(1, 47, juce::uint8(102))),
        capturedEvent(6, 650, juce::MidiMessage::noteOff(1, 47))
    };
    juce::String error;
    const auto converted = studio::convertCapturedMidiToClip(
        project, capture, 0, 0.0, 1.0, 1000.0,
        &project.drumMaps.front(), "Synthetic kit performance", error);
    expect(converted.has_value(), error.toRawUTF8());
    if (!converted.has_value() || converted->clip.notes.size() != 3)
        return;
    const auto& notes = converted->clip.notes;
    expect(notes[0].pitch == 46 && notes[0].footControlValue == 27
               && notes[1].pitch == 38 && notes[2].pitch == 47
               && notes[2].roundRobinHint == 1
               && converted->ignoredEvents == 0,
           "Captured drums preserve exact played sounds, explicit round robins, and pre-hit foot control without ignored-event warnings.");
    expect(std::abs(notes[2].durationBeats - 0.35) < 0.0000001
               && std::abs(notes[2].startBeats - 0.45) < 0.0000001,
           "A drum hit spanning a tempo change keeps its actual performance timing.");
}

void softwareMidiCancellationAndOverflow()
{
    auto project = studio::Project::createDefault();
    const auto drums = makeMidiTrack(project);
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project).wasOk(), "The software-input cancellation fixture loads.");
    for (auto attempt = 0;
         attempt < 200 && engine.pluginRuntimeTransitionPending(); ++attempt)
        juce::Thread::sleep(10);
    engine.setMidiAuditionEnabled(true);
    const auto hit = juce::MidiMessage::noteOn(1, 36, juce::uint8(100));
    studio::RecordingPlan plan;
    expect(engine.startRecording({}, plan, true).wasOk(), "A cancellable drum take starts.");
    expect(engine.enqueueMidiInput(drums.id, hit).wasOk(), "A pending drum hit queues.");
    juce::ignoreUnused(engine.stopMidiRecording());
    engine.stopRecording();
    engine.clearMidiEventsForTesting();
    engine.processActiveBlockForTesting(512);
    const auto stoppedEvents = engine.takeMidiEventsForTesting();
    expect(std::none_of(stoppedEvents.cbegin(), stoppedEvents.cend(), [](const auto& event)
    {
        return juce::MidiMessage(event.data.data(), event.size, 0.0).isNoteOn();
    }), "Stopping a take cancels queued hits instead of sounding them after recording ended.");

    auto overflowReported = false;
    for (int index = 0; index < 1024; ++index)
    {
        if (engine.enqueueMidiInput(drums.id, hit).failed())
        {
            overflowReported = true;
            break;
        }
    }
    expect(overflowReported, "Software MIDI has a bounded queue and reports overflow.");
    engine.clearMidiEventsForTesting();
    engine.processActiveBlockForTesting(512);
    const auto overflowEvents = engine.takeMidiEventsForTesting();
    expect(std::none_of(overflowEvents.cbegin(), overflowEvents.cend(), [](const auto& event)
    {
        return juce::MidiMessage(event.data.data(), event.size, 0.0).isNoteOn();
    }), "An overflow discards the entire stale generation rather than losing only note-offs.");
    expect(engine.enqueueMidiInput(drums.id, hit).wasOk(),
           "Software MIDI recovers after the audio consumer drains an overflow.");
    engine.processActiveBlockForTesting(512);
    const auto recoveredEvents = engine.takeMidiEventsForTesting();
    expect(std::any_of(recoveredEvents.cbegin(), recoveredEvents.cend(), [](const auto& event)
    {
        return juce::MidiMessage(event.data.data(), event.size, 0.0).isNoteOn();
    }), "An unarmed track can audition new software input after recovery.");
}

void softwareMidiTransportBoundaries()
{
    for (const auto looping : { false, true })
    {
        auto project = studio::Project::createDefault();
        const auto drums = makeMidiTrack(project);
        project.loopEnabled = looping;
        project.loopStartSeconds = 0.0;
        project.loopEndSeconds = 0.1;
        project.preRollSeconds = looping ? 0.0 : 0.1;
        studio::StudioAudioEngine engine;
        expect(engine.updateProject(project).wasOk(), "The drum transport fixture loads.");
        for (auto attempt = 0;
             attempt < 200 && engine.pluginRuntimeTransitionPending(); ++attempt)
            juce::Thread::sleep(10);
        engine.setMidiAuditionEnabled(true);
        const auto plan = project.recordingPlan(looping ? 0.0 : 0.1);
        expect(engine.startRecording({}, plan, true).wasOk(),
               "Drum performance starts with the existing pre-roll or loop recording plan.");
        expect(engine.enqueueMidiInput(
                   drums.id, juce::MidiMessage::noteOn(1, 36, juce::uint8(100))).wasOk(),
               "A hit before the first transport boundary queues.");
        engine.processActiveBlockForTesting(512);
        expect(engine.enqueueMidiInput(drums.id, juce::MidiMessage::noteOff(1, 36)).wasOk(),
               "The first transport-boundary hit releases.");
        engine.processActiveBlockForTesting(512);
        engine.processActiveBlockForTesting(3776);
        expect(engine.enqueueMidiInput(
                   drums.id, juce::MidiMessage::noteOn(1, 38, juce::uint8(115))).wasOk(),
               "A hit after the pre-roll or loop boundary queues.");
        engine.processActiveBlockForTesting(512);
        expect(engine.enqueueMidiInput(drums.id, juce::MidiMessage::noteOff(1, 38)).wasOk(),
               "The second transport-boundary hit releases.");
        engine.processActiveBlockForTesting(512);
        const auto recording = engine.stopMidiRecording();
        engine.stopRecording();
        expect(recording.result.wasOk(), "The drum transport take finalizes.");
        const auto passes = studio::recordingPasses(recording.durationSeconds, plan);
        expect(passes.size() == (looping ? 2u : 1u),
               "Drum input uses ordinary pass boundaries without duplicating pre-roll.");
        for (std::size_t index = 0; index < passes.size(); ++index)
        {
            const auto& pass = passes[index];
            juce::String error;
            const auto converted = studio::convertCapturedMidiToClip(
                project, recording.captured,
                recording.captureStartStreamSample + static_cast<std::int64_t>(
                    std::llround(pass.sourceOffsetSeconds * recording.sampleRate)),
                pass.timelineStartSeconds, pass.durationSeconds, recording.sampleRate,
                &project.drumMaps.front(), "Synthetic drum pass", error, drums.id);
            expect(converted.has_value() && converted->clip.notes.size() == 1
                       && converted->clip.notes.front().pitch == (looping && index == 0 ? 36 : 38),
                   "Pre-roll hits are excluded and each loop pass retains only its own played drum note.");
        }
    }
}

void midiCapturePublicationAndStopBoundary()
{
    studio::MidiCaptureBuffer ring;
    juce::MidiBuffer message;
    for (std::uint64_t ordinal = 0;
         ordinal < studio::MidiCaptureBuffer::capacity;
         ++ordinal)
    {
        message.clear();
        message.addEvent(
            juce::MidiMessage::noteOn(
                1,
                static_cast<int>(ordinal % 128),
                static_cast<juce::uint8>(100)),
            0);
        ring.push(
            message,
            static_cast<std::int64_t>(ordinal),
            static_cast<std::int64_t>(ordinal));
    }

    std::atomic<bool> publicationEntered { false };
    std::atomic<bool> publicationRelease { false };
    ring.setPublicationPauseForTesting(
        &publicationEntered,
        &publicationRelease);
    std::thread wrappingWriter([&]
    {
        juce::MidiBuffer wrapped;
        wrapped.addEvent(
            juce::MidiMessage::noteOn(
                1,
                73,
                static_cast<juce::uint8>(111)),
            0);
        ring.push(
            wrapped,
            static_cast<std::int64_t>(
                studio::MidiCaptureBuffer::capacity),
            900000);
    });
    for (int attempt = 0;
         attempt < 5000
             && !publicationEntered.load(std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    const auto oldSlotDuringWrap = ring.read(0, 1);
    expect(publicationEntered.load(std::memory_order_acquire)
               && ring.writeOrdinal()
                      == studio::MidiCaptureBuffer::capacity
               && oldSlotDuringWrap.events.empty()
               && oldSlotDuringWrap.overwrittenEvents == 1,
           "A reserved wrapping slot is not published early and cannot expose torn prior-generation data.");
    publicationRelease.store(true, std::memory_order_release);
    wrappingWriter.join();
    ring.setPublicationPauseForTesting(nullptr, nullptr);
    const auto wrapped = ring.read(
        studio::MidiCaptureBuffer::capacity,
        studio::MidiCaptureBuffer::capacity + 1);
    expect(wrapped.events.size() == 1
               && wrapped.events.front().ordinal
                      == studio::MidiCaptureBuffer::capacity
               && wrapped.events.front().streamSample
                      == static_cast<std::int64_t>(
                          studio::MidiCaptureBuffer::capacity)
               && wrapped.events.front().timelineSample == 900000
               && wrapped.events.front().data[1] == 73,
           "A wrapping capture slot becomes visible atomically after publication.");

    studio::MidiCaptureBuffer concurrentRing;
    std::atomic<bool> writerDone { false };
    std::atomic<bool> observedConsistentData { true };
    std::thread concurrentWriter([&]
    {
        juce::MidiBuffer eventBuffer;
        for (std::uint64_t ordinal = 0;
             ordinal < studio::MidiCaptureBuffer::capacity + 4096;
             ++ordinal)
        {
            eventBuffer.clear();
            eventBuffer.addEvent(
                juce::MidiMessage::noteOn(
                    1,
                    static_cast<int>(ordinal % 128),
                    static_cast<juce::uint8>(99)),
                0);
            concurrentRing.push(
                eventBuffer,
                static_cast<std::int64_t>(ordinal),
                static_cast<std::int64_t>(ordinal + 500000));
        }
        writerDone.store(true, std::memory_order_release);
    });
    while (!writerDone.load(std::memory_order_acquire))
    {
        const auto end = concurrentRing.writeOrdinal();
        const auto begin = end > studio::MidiCaptureBuffer::capacity
            ? end - studio::MidiCaptureBuffer::capacity
            : 0;
        const auto window = concurrentRing.read(begin, end);
        for (const auto& event : window.events)
        {
            if (event.streamSample
                    != static_cast<std::int64_t>(event.ordinal)
                || event.timelineSample
                    != static_cast<std::int64_t>(
                        event.ordinal + 500000)
                || event.size != 3
                || event.data[1]
                    != static_cast<std::uint8_t>(
                        event.ordinal % 128))
            {
                observedConsistentData.store(
                    false,
                    std::memory_order_release);
                break;
            }
        }
    }
    concurrentWriter.join();
    expect(observedConsistentData.load(std::memory_order_acquire),
           "Concurrent capture reads never observe torn or ABA slot contents across wrap.");

    studio::StudioAudioEngine engine;
    studio::RecordingPlan plan;
    plan.transportStartSeconds = 0.0;
    plan.captureStartSeconds = 0.0;
    expect(engine.startRecording({}, plan, true).wasOk(),
           "The final-block capture fixture starts MIDI recording.");
    std::atomic<bool> finalBlockEntered { false };
    std::atomic<bool> finalBlockRelease { false };
    engine.setMidiCapturePublicationPauseForTesting(
        &finalBlockEntered,
        &finalBlockRelease);
    juce::MidiBuffer finalBlock;
    finalBlock.addEvent(
        juce::MidiMessage::noteOn(
            3,
            67,
            static_cast<juce::uint8>(105)),
        7);
    std::thread audioThread([&]
    {
        juce::ignoreUnused(
            engine.renderActiveBlockWithMidiForTesting(
                finalBlock,
                64));
    });
    for (int attempt = 0;
         attempt < 5000
             && !finalBlockEntered.load(std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    std::optional<studio::StudioAudioEngine::MidiRecordingResult>
        stoppedRecording;
    std::atomic<bool> stopStarted { false };
    std::atomic<bool> stopFinished { false };
    std::thread stopThread([&]
    {
        stopStarted.store(true, std::memory_order_release);
        stoppedRecording = engine.stopMidiRecording();
        stopFinished.store(true, std::memory_order_release);
    });
    for (int attempt = 0;
         attempt < 5000
             && !stopStarted.load(std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    juce::Thread::sleep(10);
    expect(finalBlockEntered.load(std::memory_order_acquire)
               && !stopFinished.load(std::memory_order_acquire),
           "Stopping MIDI capture waits for a block admitted before the stop boundary.");
    finalBlockRelease.store(true, std::memory_order_release);
    audioThread.join();
    stopThread.join();
    engine.setMidiCapturePublicationPauseForTesting(nullptr, nullptr);
    engine.stopRecording();
    expect(stoppedRecording.has_value()
               && stoppedRecording->result.wasOk()
               && stoppedRecording->captured.events.size() == 1
               && stoppedRecording->captured.events.front().streamSample
                      == 7
               && std::abs(
                      stoppedRecording->durationSeconds
                      - 64.0 / stoppedRecording->sampleRate)
                      < 0.0000001,
           "A pre-stop final block is wholly published and included with its exact capture boundary.");
}

void pianoRollNoteCreation()
{
    auto project = studio::Project::createDefault();
    const auto track = makeMidiTrack(project, "Piano roll");
    studio::MidiClip clip;
    clip.name = "Ordinary piano roll";
    clip.editorMode = studio::MidiEditorMode::pianoRoll;
    clip.durationBeats = 4.0;
    const auto clipId = clip.id;
    project.findTrack(track.id)->midiClips.push_back(clip);

    expect(project.drumMaps.front().entryForPitch(60) != nullptr,
           "The piano-roll regression fixture includes a default-map entry at the editor cursor pitch.");
    std::optional<studio::MidiClip> edited;
    studio::MidiEditorComponent editor;
    editor.onClipEdited =
        [&edited](const juce::String&,
                  const studio::MidiClip&,
                  const studio::MidiClip& after,
                  const juce::String&)
    {
        edited = after;
    };
    editor.setProject(&project);
    editor.setSelection(track.id, clipId);
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)),
           "Return creates a piano-roll note.");
    const auto* note = edited.has_value() && edited->notes.size() == 1
        ? &edited->notes.front()
        : nullptr;
    expect(note != nullptr
               && note->drumMapEntryId.isEmpty()
               && note->articulation.isEmpty()
               && note->chokeGroup.isEmpty()
               && note->cymbalState == studio::CymbalState::none
               && note->footControlValue == -1
               && note->roundRobinHint == -1,
           "Piano-roll note creation does not borrow metadata from the project's default drum map.");
}

void midiClipDrumMapReferenceValidation()
{
    {
        auto project = studio::Project::createDefault();
        const auto track = makeMidiTrack(project, "Missing drum map");
        studio::MidiClip clip;
        clip.name = "Ordinary MIDI";
        const auto clipId = clip.id;
        project.findTrack(track.id)->midiClips.push_back(clip);

        auto after = clip;
        after.notes.push_back(studio::createMidiNote(
            after,
            0.0,
            60,
            0.25,
            100,
            &project.drumMaps.front()));
        expect(after.notes.front().drumMapEntryId.isNotEmpty(),
               "The missing-map regression fixture contains a note-level drum-map reference.");

        studio::CommandStack commands;
        juce::String error;
        expect(!commands.perform(
                   std::make_unique<studio::SetMidiClipStateCommand>(
                       track.id,
                       clip,
                       after,
                       "Create MIDI note"),
                   project,
                   error)
                   && error.containsIgnoreCase("has no drum map")
                   && error.containsIgnoreCase("select a drum map"),
               "SetMidiClipStateCommand rejects a note-level drum-map reference when the clip has no map and explains how to fix it.");
        expect(project.findMidiClip(clipId) != nullptr
                   && project.findMidiClip(clipId)->notes.empty(),
               "A rejected missing-map edit leaves the original MIDI clip unchanged.");

        const auto package =
            juce::File::getCurrentWorkingDirectory()
                .getNonexistentChildFile(
                    "StudioDuoMissingMidiMapReference",
                    ".studioduo",
                    false);
        const auto saveResult = studio::ProjectFile::save(project, package);
        expect(saveResult.wasOk(),
               "Rejecting a missing note-level map owner keeps the project saveable.");
        if (package.exists())
            package.deleteRecursively();
    }

    {
        auto project = studio::Project::createDefault();
        const auto track = makeMidiTrack(project, "Invalid drum entry");
        auto clip = makeMidiClip(project);
        const auto clipId = clip.id;
        project.findTrack(track.id)->midiClips.push_back(clip);

        auto after = clip;
        after.notes.front().drumMapEntryId = "missing-drum-entry";
        studio::CommandStack commands;
        juce::String error;
        expect(!commands.perform(
                   std::make_unique<studio::SetMidiClipStateCommand>(
                       track.id,
                       clip,
                       after,
                       "Remap MIDI note"),
                   project,
                   error)
                   && error.containsIgnoreCase(
                       "unavailable drum-map entry")
                   && error.contains("missing-drum-entry")
                   && error.containsIgnoreCase("remap"),
               "SetMidiClipStateCommand rejects an unavailable note-level drum-map entry and explains how to fix it.");
        expect(project.findMidiClip(clipId) != nullptr
                   && project.findMidiClip(clipId)
                          ->notes.front()
                          .drumMapEntryId
                       == clip.notes.front().drumMapEntryId,
               "A rejected invalid-entry edit leaves the original MIDI clip unchanged.");

        const auto package =
            juce::File::getCurrentWorkingDirectory()
                .getNonexistentChildFile(
                    "StudioDuoInvalidMidiMapReference",
                    ".studioduo",
                    false);
        const auto saveResult = studio::ProjectFile::save(project, package);
        expect(saveResult.wasOk(),
               "Rejecting an invalid note-level map entry keeps the project saveable.");
        if (package.exists())
            package.deleteRecursively();
    }
}

void midiNoteMoveMetadata()
{
    const auto map = studio::createDefaultMetalDrumMap();
    studio::MidiClip clip;
    clip.name = "Metadata moves";
    clip.editorMode = studio::MidiEditorMode::drums;
    clip.drumMapId = map.id;
    clip.durationBeats = 4.0;

    auto openHat = studio::createMidiNote(
        clip,
        0.5,
        46,
        0.25,
        100,
        &map);
    const auto openHatId = openHat.id;
    clip.notes.push_back(openHat);
    auto snare = studio::createMidiNote(
        clip,
        1.0,
        38,
        0.25,
        100,
        &map);
    const auto snareId = snare.id;
    clip.notes.push_back(snare);

    expect(openHat.articulation.isNotEmpty()
               && openHat.chokeGroup.isNotEmpty()
               && openHat.cymbalState == studio::CymbalState::open
               && openHat.footControlValue == 0
               && snare.roundRobinHint == 1,
           "The timing-move fixture covers articulation, choke, foot-control, cymbal, and round-robin metadata.");
    expect(studio::moveMidiNotes(
               clip,
               { openHatId, snareId },
               0.25,
               0),
           "Timing-only movement applies to mapped drum notes.");
    const auto* movedOpenHat = findNote(clip, openHatId);
    const auto* movedSnare = findNote(clip, snareId);
    expect(movedOpenHat != nullptr
               && movedOpenHat->drumMapEntryId
                      == openHat.drumMapEntryId
               && movedOpenHat->articulation == openHat.articulation
               && movedOpenHat->chokeGroup == openHat.chokeGroup
               && movedOpenHat->cymbalState == openHat.cymbalState
               && movedOpenHat->footControlValue
                      == openHat.footControlValue
               && movedOpenHat->roundRobinHint
                      == openHat.roundRobinHint
               && movedSnare != nullptr
               && movedSnare->drumMapEntryId == snare.drumMapEntryId
               && movedSnare->articulation == snare.articulation
               && movedSnare->chokeGroup == snare.chokeGroup
               && movedSnare->cymbalState == snare.cymbalState
               && movedSnare->footControlValue
                      == snare.footControlValue
               && movedSnare->roundRobinHint
                      == snare.roundRobinHint,
           "Timing-only moves preserve all drum playback metadata.");

    studio::MidiClip remapped;
    remapped.name = "Pitch remap";
    remapped.editorMode = studio::MidiEditorMode::drums;
    remapped.drumMapId = map.id;
    remapped.durationBeats = 4.0;
    auto remappedNote = studio::createMidiNote(
        remapped,
        0.5,
        46,
        0.25,
        100,
        &map);
    const auto remappedNoteId = remappedNote.id;
    remapped.notes.push_back(remappedNote);
    expect(studio::moveMidiNotes(
               remapped,
               { remappedNoteId },
               0.0,
               -4,
               &map),
           "Pitch movement applies against the clip's drum map.");
    const auto* closedHat = map.entryForPitch(42);
    const auto* movedClosedHat = findNote(remapped, remappedNoteId);
    expect(closedHat != nullptr
               && movedClosedHat != nullptr
               && movedClosedHat->pitch == closedHat->noteNumber
               && movedClosedHat->drumMapEntryId == closedHat->id
               && movedClosedHat->articulation
                      == closedHat->articulation
               && movedClosedHat->chokeGroup == closedHat->chokeGroup
               && movedClosedHat->cymbalState
                      == closedHat->cymbalState
               && movedClosedHat->footControlValue == 127
               && movedClosedHat->roundRobinHint == -1,
           "Pitch changes remap all drum metadata to the destination entry.");

    studio::MidiClip retainedRoundRobin;
    retainedRoundRobin.name = "Retained round robin";
    retainedRoundRobin.editorMode = studio::MidiEditorMode::drums;
    retainedRoundRobin.drumMapId = map.id;
    retainedRoundRobin.durationBeats = 4.0;
    auto retainedNote = studio::createMidiNote(
        retainedRoundRobin,
        0.5,
        38,
        0.25,
        100,
        &map);
    studio::applyDrumMapMetadata(retainedNote, &map, 1);
    const auto retainedNoteId = retainedNote.id;
    retainedRoundRobin.notes.push_back(retainedNote);
    expect(studio::moveMidiNotes(
               retainedRoundRobin,
               { retainedNoteId },
               0.0,
               -4,
               &map),
           "Round-robin drum notes can move to another mapped row.");
    const auto* kick = map.entryForPitch(36);
    const auto* movedKick = findNote(
        retainedRoundRobin,
        retainedNoteId);
    expect(kick != nullptr
               && movedKick != nullptr
               && movedKick->drumMapEntryId == kick->id
               && movedKick->roundRobinHint == 1
               && movedKick->pitch == kick->roundRobinNotes.front(),
           "Pitch remapping retains a round-robin variant that is valid for the destination entry.");

    studio::MidiClip resetRoundRobin;
    resetRoundRobin.name = "Reset round robin";
    resetRoundRobin.editorMode = studio::MidiEditorMode::drums;
    resetRoundRobin.drumMapId = map.id;
    resetRoundRobin.durationBeats = 4.0;
    auto resetNote = studio::createMidiNote(
        resetRoundRobin,
        0.5,
        38,
        0.25,
        100,
        &map);
    studio::applyDrumMapMetadata(resetNote, &map, 1);
    resetNote.roundRobinHint = 7;
    const auto resetNoteId = resetNote.id;
    resetRoundRobin.notes.push_back(resetNote);
    expect(studio::moveMidiNotes(
               resetRoundRobin,
               { resetNoteId },
               0.0,
               -4,
               &map),
           "A note with a stale round-robin hint can still be safely remapped.");
    const auto* resetKick = findNote(resetRoundRobin, resetNoteId);
    expect(resetKick != nullptr
               && resetKick->drumMapEntryId == kick->id
               && resetKick->roundRobinHint == 0
               && resetKick->pitch == kick->noteNumber,
           "Pitch remapping resets a round-robin variant that is invalid for the destination entry.");
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
                   studio::MidiExpressionType::timbre)
               && studio::setMidiLaneValue(
                   after,
                   { noteId },
                   studio::MidiEditorLane::expression,
                   0.55,
                   1.125,
                   studio::MidiExpressionType::channelPressure),
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
                   })
               && std::any_of(
                   edited->notes.front().expressions.cbegin(),
                   edited->notes.front().expressions.cend(),
                   [](const auto& point)
                   {
                       return point.type
                               == studio::MidiExpressionType::
                                   channelPressure
                           && point.controller == -1
                           && std::abs(point.value - 0.55)
                                  < 0.000001;
                   }),
           "Undoable MIDI clip state commands persist move, resize, and distinct pressure lane edits.");
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
        juce::MidiMessage::channelPressureChange(2, 72),
        2);
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
               && recording.captured.events.size() == 5
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
               && converted->clip.notes.front().expressions.size() == 3
               && std::count_if(
                      converted->clip.notes.front().expressions.cbegin(),
                      converted->clip.notes.front().expressions.cend(),
                      [](const auto& expression)
                      {
                          return expression.type
                              == studio::MidiExpressionType::pressure;
                      })
                      == 1
               && std::count_if(
                      converted->clip.notes.front().expressions.cbegin(),
                      converted->clip.notes.front().expressions.cend(),
                      [](const auto& expression)
                      {
                          return expression.type
                              == studio::MidiExpressionType::
                                  channelPressure;
                      })
                      == 1
               && converted->clip.notes.front().durationBeats > 0.0,
           "Recorded key pressure and channel pressure remain distinct editable expressions.");

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

    studio::StudioAudioEngine heldRetrospectiveEngine;
    juce::MidiBuffer heldRecent;
    heldRecent.addEvent(
        juce::MidiMessage::noteOn(
            1,
            65,
            static_cast<juce::uint8>(90)),
        4);
    juce::ignoreUnused(
        heldRetrospectiveEngine.renderActiveBlockWithMidiForTesting(
            heldRecent,
            32));
    heldRetrospectiveEngine.processActiveBlockForTesting(24000);
    const auto heldRetrospective =
        heldRetrospectiveEngine.captureRetrospectiveMidi(10.0);
    const auto expectedHeldDuration =
        (24032.0 - 4.0) / heldRetrospective.sampleRate;
    error.clear();
    const auto heldConverted =
        heldRetrospective.result.wasOk()
        ? studio::convertCapturedMidiToClip(
              project,
              heldRetrospective.captured,
              heldRetrospective.captureStartStreamSample,
              heldRetrospective.timelineStartSeconds,
              heldRetrospective.durationSeconds,
              heldRetrospective.sampleRate,
              nullptr,
              "Held retrospective MIDI",
              error)
        : std::optional<studio::MidiCaptureConversion> {};
    expect(heldRetrospective.result.wasOk()
               && std::abs(
                      heldRetrospective.durationSeconds
                      - expectedHeldDuration)
                      < 0.0000001
               && heldConverted.has_value()
               && heldConverted->clip.notes.size() == 1
               && std::abs(
                      heldConverted->clip.notes.front().durationBeats
                      - heldConverted->clip.durationBeats)
                      < 0.0000001,
           "Retrospective held notes extend to the exact capture end instead of a placeholder duration.");

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
    sustainedCaptureFinalization();
    drumPadBindingsPersistence();
    drumPerformanceKeyboard();
    targetedSoftwareMidi();
    alternateDrumEditor();
    drumCaptureFidelity();
    softwareMidiCancellationAndOverflow();
    softwareMidiTransportBoundaries();
    midiCapturePublicationAndStopBoundary();
    pianoRollNoteCreation();
    midiClipDrumMapReferenceValidation();
    midiNoteMoveMetadata();
    midiEditingCommands();
    deterministicHumanization();
    drumMapsPatternsAndTools();
    routingTemplates();
    midiRecordingAndRetrospectiveCapture();
}
