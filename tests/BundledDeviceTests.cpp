#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "devices/AmpDeviceProcessor.h"
#include "devices/DeviceRegistry.h"
#include "devices/DrumDeviceProcessor.h"
#include "midi/MidiEditing.h"
#include "model/ProjectCommands.h"
#include "model/ProjectModel.h"
#include "model/ProjectTemplates.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>

namespace
{
bool setParameter(juce::AudioProcessor& processor,
                  const juce::String& id,
                  float normalized)
{
    for (auto* parameter : processor.getParameters())
    {
        const auto* identified =
            dynamic_cast<juce::AudioProcessorParameterWithID*>(
                parameter);
        if (identified != nullptr && identified->paramID == id)
        {
            parameter->setValueNotifyingHost(normalized);
            return true;
        }
    }
    return false;
}

float magnitude(const juce::AudioBuffer<float>& audio,
                int channel,
                int start = 0,
                int samples = -1)
{
    if (channel < 0 || channel >= audio.getNumChannels())
        return 0.0f;
    const auto count = samples < 0
        ? audio.getNumSamples() - start
        : std::min(samples, audio.getNumSamples() - start);
    return count > 0 ? audio.getMagnitude(channel, start, count) : 0.0f;
}

int outputChannel(const juce::AudioProcessor& processor,
                  int bus,
                  int channel)
{
    return processor.getChannelIndexInProcessBlockBuffer(
        false,
        bus,
        channel);
}

bool waitForRuntime(studio::StudioAudioEngine& engine)
{
    for (int attempt = 0;
         attempt < 200 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    return !engine.pluginRuntimeTransitionPending();
}

bool waitForFlag(const std::atomic<bool>& flag)
{
    for (int attempt = 0;
         attempt < 2000 && !flag.load(std::memory_order_acquire);
         ++attempt)
        juce::Thread::sleep(1);
    return flag.load(std::memory_order_acquire);
}

studio::StudioAudioEngine::PluginRuntimeRequest requestFor(
    const studio::Track& track,
    const studio::PluginInsert& insert,
    juce::MemoryBlock state = {})
{
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = track.id;
    request.insertId = insert.id;
    request.name = insert.name;
    request.deviceIdentifier = insert.pluginIdentifier;
    request.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    request.state = std::move(state);
    return request;
}

studio::Track& addInstrumentTrack(studio::Project& project,
                                  const juce::String& name)
{
    studio::Track track;
    track.name = name;
    track.type = studio::TrackType::instrument;
    project.tracks.insert(project.tracks.end() - 1, track);
    return *project.findTrack(track.id);
}

studio::PluginInsert addBundledInsert(
    studio::Track& track,
    const juce::String& identifier,
    const juce::String& name)
{
    studio::PluginInsert insert;
    insert.pluginIdentifier = identifier;
    insert.name = name;
    insert.manufacturer = "Studio Duo";
    insert.format = "Studio Duo";
    insert.bundledDevice = true;
    insert.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    track.inserts.push_back(insert);
    return insert;
}

juce::AudioBuffer<float> sineInput(int samples,
                                   double frequency = 110.0)
{
    juce::AudioBuffer<float> input(2, samples);
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto value = static_cast<float>(
            std::sin(
                juce::MathConstants<double>::twoPi
                * frequency
                * static_cast<double>(sample)
                / 48000.0)
            * 0.18);
        input.setSample(0, sample, value);
        input.setSample(1, sample, value);
    }
    return input;
}

bool writeCabinetIr(const juce::File& file, int samples)
{
    if (file.existsAsFile())
        file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream =
        file.createOutputStream();
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(1)
            .withBitsPerSample(24));
    if (writer == nullptr)
        return false;
    juce::AudioBuffer<float> impulse(1, samples);
    impulse.clear();
    impulse.setSample(0, 0, 0.8f);
    for (int sample = 1; sample < samples; ++sample)
    {
        impulse.setSample(
            0,
            sample,
            static_cast<float>(
                std::sin(
                    juce::MathConstants<double>::twoPi
                    * 2200.0
                    * static_cast<double>(sample)
                    / 48000.0)
                * std::exp(-static_cast<double>(sample) / 72.0)
                * 0.16));
    }
    return writer->writeFromAudioSampleBuffer(
        impulse,
        0,
        impulse.getNumSamples());
}

void registryMetadata()
{
    const auto* drums = studio::DeviceRegistry::descriptor(
        "studio.device.drum-composer");
    const auto* guitar = studio::DeviceRegistry::descriptor(
        "studio.device.guitar-amp");
    const auto* bass = studio::DeviceRegistry::descriptor(
        "studio.device.bass-amp");
    expect(drums != nullptr
               && drums->instrument
               && drums->inputChannels == 0
               && drums->outputChannels == 10
               && drums->outputBuses
                      == std::vector<juce::String> {
                          "Main", "Kick", "Snare", "Toms", "Cymbals"
                      },
           "The bundled registry advertises the drum instrument and its five stereo buses.");
    expect(guitar != nullptr
               && bass != nullptr
               && !guitar->instrument
               && !bass->instrument
               && guitar->category == "Amp"
               && bass->category == "Amp",
           "The bundled registry advertises both cabinet amp effects.");
}

void drumProcessor()
{
    auto drum = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    expect(drum != nullptr
               && drum->acceptsMidi()
               && drum->getBusCount(false)
                      == studio::DrumDeviceProcessor::outputBusCount,
           "The bundled drum device is a MIDI instrument with multi-output buses.");
    drum->prepareToPlay(48000.0, 1024);

    juce::AudioBuffer<float> audio(
        drum->getTotalNumOutputChannels(),
        1024);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120), 0);
    drum->processBlock(audio, midi);
    expect(magnitude(
               audio,
               outputChannel(
                   *drum,
                   studio::DrumDeviceProcessor::mainOutput,
                   0))
               > 0.01f,
           "A normal kick MIDI note renders drum audio.");
    expect(magnitude(
               audio,
               outputChannel(
                   *drum,
                   studio::DrumDeviceProcessor::kickOutput,
                   0))
               > 0.01f
               && magnitude(
                      audio,
                      outputChannel(
                          *drum,
                          studio::DrumDeviceProcessor::snareOutput,
                          0))
                      < 0.00001f,
           "Kick audio appears on the deterministic kick stem only.");

    const auto renderVelocity = [&drum](int velocity)
    {
        drum->reset();
        juce::AudioBuffer<float> rendered(
            drum->getTotalNumOutputChannels(),
            512);
        rendered.clear();
        juce::MidiBuffer events;
        events.addEvent(
            juce::MidiMessage::noteOn(
                1,
                38,
                static_cast<juce::uint8>(velocity)),
            0);
        drum->processBlock(rendered, events);
        return magnitude(
            rendered,
            outputChannel(
                *drum,
                studio::DrumDeviceProcessor::snareOutput,
                0));
    };
    expect(renderVelocity(120) > renderVelocity(36) * 2.0f,
           "Drum synthesis honors MIDI velocity.");

    drum->reset();
    juce::AudioBuffer<float> deterministicA(
        drum->getTotalNumOutputChannels(),
        1024);
    deterministicA.clear();
    juce::MidiBuffer sequence;
    sequence.addEvent(
        juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110),
        0);
    sequence.addEvent(
        juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110),
        512);
    drum->processBlock(deterministicA, sequence);
    drum->reset();
    juce::AudioBuffer<float> deterministicB(
        drum->getTotalNumOutputChannels(),
        1024);
    deterministicB.clear();
    drum->processBlock(deterministicB, sequence);
    auto maximumDifference = 0.0f;
    for (int channel = 0;
         channel < deterministicA.getNumChannels();
         ++channel)
        for (int sample = 0;
             sample < deterministicA.getNumSamples();
             ++sample)
            maximumDifference = std::max(
                maximumDifference,
                std::abs(
                    deterministicA.getSample(channel, sample)
                    - deterministicB.getSample(channel, sample)));
    expect(maximumDifference < 0.000001f,
           "Drum round robin and noise reset to a deterministic sequence.");
    const auto kickChannel = outputChannel(
        *drum,
        studio::DrumDeviceProcessor::kickOutput,
        0);
    auto roundRobinDifference = 0.0f;
    for (int sample = 4; sample < 120; ++sample)
    {
        roundRobinDifference += std::abs(
            deterministicA.getSample(kickChannel, sample)
            - deterministicA.getSample(
                kickChannel,
                512 + sample));
    }
    expect(roundRobinDifference > 0.01f,
           "Repeated base notes use deterministic round-robin variations.");

    auto crash = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    auto chokedCrash = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    crash->prepareToPlay(48000.0, 512);
    chokedCrash->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> crashBlock(
        crash->getTotalNumOutputChannels(),
        512);
    juce::AudioBuffer<float> chokeBlock(
        chokedCrash->getTotalNumOutputChannels(),
        512);
    crashBlock.clear();
    chokeBlock.clear();
    juce::MidiBuffer crashHit;
    crashHit.addEvent(
        juce::MidiMessage::noteOn(1, 49, (juce::uint8) 118),
        0);
    crash->processBlock(crashBlock, crashHit);
    chokedCrash->processBlock(chokeBlock, crashHit);
    crashBlock.clear();
    chokeBlock.clear();
    juce::MidiBuffer noEvents;
    juce::MidiBuffer choke;
    choke.addEvent(
        juce::MidiMessage::noteOn(1, 57, (juce::uint8) 100),
        0);
    crash->processBlock(crashBlock, noEvents);
    chokedCrash->processBlock(chokeBlock, choke);
    const auto cymbalChannel = outputChannel(
        *crash,
        studio::DrumDeviceProcessor::cymbalsOutput,
        0);
    expect(magnitude(chokeBlock, cymbalChannel, 128, 384)
               < magnitude(crashBlock, cymbalChannel, 128, 384) * 0.2f,
           "Mapped cymbal choke notes terminate the matching cymbal state.");

    const auto map = studio::createDefaultMetalDrumMap();
    studio::MidiClip mappedClip;
    mappedClip.editorMode = studio::MidiEditorMode::drums;
    mappedClip.drumMapId = map.id;
    const auto openHat = studio::createMidiNote(
        mappedClip,
        0.0,
        46,
        0.25,
        110,
        &map);
    const auto closedHat = studio::createMidiNote(
        mappedClip,
        0.0,
        42,
        0.25,
        110,
        &map);
    const auto pedalHat = studio::createMidiNote(
        mappedClip,
        0.0,
        44,
        0.25,
        110,
        &map);
    expect(openHat.footControlValue == 0
               && closedHat.footControlValue == 127
               && pedalHat.footControlValue > 0
               && pedalHat.footControlValue < 127,
           "Hi-hat metadata follows the MIDI CC4 convention: 0 is open and 127 is closed.");

    const auto renderHatTail = [](int footValue)
    {
        auto instrument = studio::DeviceRegistry::create(
            "studio.device.drum-composer");
        instrument->prepareToPlay(48000.0, 1024);
        juce::AudioBuffer<float> first(
            instrument->getTotalNumOutputChannels(),
            1024);
        first.clear();
        juce::MidiBuffer hit;
        hit.addEvent(
            juce::MidiMessage::controllerEvent(1, 4, footValue),
            0);
        hit.addEvent(
            juce::MidiMessage::noteOn(1, 46, (juce::uint8) 110),
            0);
        instrument->processBlock(first, hit);
        juce::AudioBuffer<float> tail(
            instrument->getTotalNumOutputChannels(),
            1024);
        juce::MidiBuffer empty;
        for (int block = 0; block < 32; ++block)
        {
            tail.clear();
            instrument->processBlock(tail, empty);
        }
        return magnitude(
            tail,
            outputChannel(
                *instrument,
                studio::DrumDeviceProcessor::cymbalsOutput,
                0));
    };
    expect(renderHatTail(0) > renderHatTail(127) * 2.0f,
           "CC4 value 0 produces a longer open hi-hat decay than closed value 127.");

    const auto renderHatAfterControl = [](int footValue)
    {
        auto instrument = studio::DeviceRegistry::create(
            "studio.device.drum-composer");
        instrument->prepareToPlay(48000.0, 1024);
        juce::AudioBuffer<float> renderedHat(
            instrument->getTotalNumOutputChannels(),
            1024);
        renderedHat.clear();
        juce::MidiBuffer hit;
        hit.addEvent(
            juce::MidiMessage::controllerEvent(1, 4, 0),
            0);
        hit.addEvent(
            juce::MidiMessage::noteOn(1, 46, (juce::uint8) 110),
            0);
        instrument->processBlock(renderedHat, hit);
        renderedHat.clear();
        juce::MidiBuffer control;
        control.addEvent(
            juce::MidiMessage::controllerEvent(1, 4, footValue),
            0);
        instrument->processBlock(renderedHat, control);
        return magnitude(
            renderedHat,
            outputChannel(
                *instrument,
                studio::DrumDeviceProcessor::cymbalsOutput,
                0),
            512,
            512);
    };
    expect(renderHatAfterControl(127)
               < renderHatAfterControl(0) * 0.1f,
           "Closing CC4 to 127 rapidly chokes an already-open hi-hat.");

    juce::MemoryBlock state;
    drum->getStateInformation(state);
    auto restored = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    auto* validated =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            restored.get());
    expect(validated != nullptr
               && validated->restoreValidatedState(
                      state.getData(),
                      static_cast<int>(state.getSize()))
                      .wasOk(),
           "Drum device parameters restore through validated state.");
}

void drumTailAndVoiceReuse()
{
    constexpr auto sampleRate = 8000.0;
    constexpr auto blockSamples = 256;
    studio::DrumDeviceProcessor drum;
    drum.prepareToPlay(sampleRate, blockSamples);
    expect(setParameter(drum, "room", 1.0f),
           "The drum tail fixture enables the maximum room amount.");

    const auto declaredTailSamples = static_cast<int>(
        std::ceil(drum.getTailLengthSeconds() * sampleRate));
    auto renderedSamples = 0;
    while (renderedSamples < declaredTailSamples)
    {
        const auto samples = std::min(
            blockSamples,
            declaredTailSamples - renderedSamples);
        juce::AudioBuffer<float> audio(
            drum.getTotalNumOutputChannels(),
            samples);
        audio.clear();
        juce::MidiBuffer midi;
        if (renderedSamples == 0)
        {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1,
                    51,
                    static_cast<juce::uint8>(127)),
                0);
        }
        drum.processBlock(audio, midi);
        renderedSamples += samples;
    }

    juce::AudioBuffer<float> afterTail(
        drum.getTotalNumOutputChannels(),
        blockSamples);
    afterTail.clear();
    juce::MidiBuffer empty;
    drum.processBlock(afterTail, empty);
    const auto mainChannel = outputChannel(
        drum,
        studio::DrumDeviceProcessor::mainOutput,
        0);
    const auto cymbalChannel = outputChannel(
        drum,
        studio::DrumDeviceProcessor::cymbalsOutput,
        0);
    expect(std::max(
               magnitude(afterTail, mainChannel),
               magnitude(afterTail, cymbalChannel))
               < 0.0001f,
           "The longest cymbal and room output are near silence at the declared drum tail.");

    drum.reset();
    constexpr auto repeatedHitSeconds = 10;
    const auto totalSamples =
        static_cast<int>(sampleRate) * repeatedHitSeconds;
    const auto hitIntervalSamples =
        static_cast<int>(sampleRate) / 32;
    constexpr std::array repeatedPattern { 36, 38, 41, 49 };
    auto nextHitSample = 0;
    auto nextPatternNote = std::size_t { 0 };
    for (int blockStart = 0;
         blockStart < totalSamples;
         blockStart += blockSamples)
    {
        const auto samples = std::min(
            blockSamples,
            totalSamples - blockStart);
        juce::AudioBuffer<float> audio(
            drum.getTotalNumOutputChannels(),
            samples);
        audio.clear();
        juce::MidiBuffer midi;
        while (nextHitSample < blockStart + samples)
        {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1,
                    repeatedPattern[nextPatternNote],
                    static_cast<juce::uint8>(110)),
                nextHitSample - blockStart);
            nextPatternNote =
                (nextPatternNote + 1) % repeatedPattern.size();
            nextHitSample += hitIntervalSamples;
        }
        drum.processBlock(audio, midi);
    }
    expect(drum.activeVoiceCountForTesting() < 40,
           "A dense repeated kit pattern with eight cymbal hits per second reuses expired voices without exhausting the fixed pool.");
}

void liveDrumPadAudio()
{
    auto project = studio::ProjectTemplates::createBlankSong();
    project.metronomeEnabled = false;
    auto track = studio::ProjectTemplates::createDrumPerformanceTrack(project, 0.0);
    track.armed = false;
    studio::AddTrackCommand addTrack(track);
    juce::String error;
    expect(addTrack.perform(project, error), error.toRawUTF8());
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project, { requestFor(track, track.inserts.front()) }).wasOk()
               && waitForRuntime(engine),
           "The ready-to-play drum preset creates an actual audio runtime.");
    engine.setMidiAuditionEnabled(true);
    const auto silence = engine.renderActiveBlockForTesting(512);
    expect(magnitude(silence, 0) < 0.000001f,
           "The drum preset is silent until a pad is played.");
    expect(engine.enqueueMidiInput(
               track.id, juce::MidiMessage::noteOn(1, 36, juce::uint8(112))).wasOk(),
           "A visual drum-pad hit reaches the bundled instrument.");
    juce::ignoreUnused(engine.renderActiveBlockForTesting(512));
    const auto hit = engine.renderActiveBlockForTesting(2048);
    expect(magnitude(hit, 0) > 0.001f && magnitude(hit, 1) > 0.001f,
           "Drum pads produce stereo audio through the ordinary mixer while stopped and unarmed.");
    expect(engine.enqueueMidiInput(track.id, juce::MidiMessage::noteOff(1, 36)).wasOk(),
           "The live drum-pad note can be released.");
    juce::ignoreUnused(engine.renderActiveBlockForTesting(512));
    const auto tail = engine.renderActiveBlockForTesting(512);
    expect(magnitude(tail, 0) > 0.0001f,
           "One-shot drum tails continue between input events instead of being truncated.");
}

void drumEngineRoutingAndRender()
{
    auto project = studio::Project::createDefault();
    auto& instrument = addInstrumentTrack(project, "Bundled drums");
    instrument.armed = true;
    const auto instrumentId = instrument.id;
    const auto insert = addBundledInsert(
        instrument,
        "studio.device.drum-composer",
        "Metal Drum Composer");

    auto stateProcessor = studio::DeviceRegistry::create(
        insert.pluginIdentifier);
    setParameter(*stateProcessor, "main", 0.0f);
    setParameter(*stateProcessor, "room", 0.0f);
    juce::MemoryBlock quietMainState;
    stateProcessor->getStateInformation(quietMainState);

    studio::Track kickAux;
    kickAux.name = "Kick stem";
    kickAux.type = studio::TrackType::aux;
    project.tracks.insert(project.tracks.end() - 1, kickAux);
    studio::RoutingConnection kickRoute;
    kickRoute.name = "Kick";
    kickRoute.kind = studio::RouteKind::send;
    kickRoute.tap = studio::RouteTap::preFader;
    kickRoute.sourceTrackId = instrumentId;
    kickRoute.sourceInsertId = insert.id;
    kickRoute.sourceBusIndex =
        studio::DrumDeviceProcessor::kickOutput;
    kickRoute.destination.type =
        studio::RouteEndpointType::track;
    kickRoute.destination.trackId = kickAux.id;
    project.routingConnections.push_back(kickRoute);
    juce::String validationError;
    expect(project.validateRoutingGraph(validationError),
           validationError.toRawUTF8());
    const auto restoredProject = studio::Project::fromVar(
        project.toVar(),
        validationError);
    expect(restoredProject.has_value()
               && std::any_of(
                   restoredProject->routingConnections.cbegin(),
                   restoredProject->routingConnections.cend(),
                   [&kickRoute](const auto& route)
                   {
                       return route.id == kickRoute.id
                           && route.sourceInsertId
                                  == kickRoute.sourceInsertId
                           && route.sourceBusIndex
                                  == kickRoute.sourceBusIndex;
                   }),
           "Bundled processor-output routing survives project serialization.");
    auto removalProject = project;
    studio::CommandStack commands;
    juce::String commandError;
    expect(commands.perform(
               std::make_unique<studio::RemovePluginInsertCommand>(
                   instrumentId,
                   insert.id),
               removalProject,
               commandError)
               && std::none_of(
                   removalProject.routingConnections.cbegin(),
                   removalProject.routingConnections.cend(),
                   [&kickRoute](const auto& route)
                   {
                       return route.id == kickRoute.id;
                   }),
           "Removing a multi-output insert removes its dependent output routes.");
    expect(commands.undo(removalProject)
               && std::any_of(
                   removalProject.routingConnections.cbegin(),
                   removalProject.routingConnections.cend(),
                   [&kickRoute](const auto& route)
                   {
                       return route.id == kickRoute.id;
                   }),
           "Undo restores a removed multi-output insert and its routes.");

    studio::StudioAudioEngine routedEngine;
    const auto* routedTrack = project.findTrack(instrumentId);
    auto routedRequest = requestFor(
        *routedTrack,
        insert,
        quietMainState);
    expect(routedEngine.updateProject(
               project,
               { routedRequest })
               .wasOk()
               && waitForRuntime(routedEngine),
           "A routed bundled drum runtime becomes ready.");
    juce::MidiBuffer kick;
    kick.addEvent(
        juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120),
        0);
    const auto routed = routedEngine.renderActiveBlockWithMidiForTesting(
        kick,
        1024);

    auto dryProject = project;
    dryProject.routingConnections.erase(
        std::remove_if(
            dryProject.routingConnections.begin(),
            dryProject.routingConnections.end(),
            [&kickRoute](const auto& route)
            {
                return route.id == kickRoute.id;
            }),
        dryProject.routingConnections.end());
    studio::StudioAudioEngine mainOnlyEngine;
    expect(mainOnlyEngine.updateProject(
               dryProject,
               { routedRequest })
               .wasOk()
               && waitForRuntime(mainOnlyEngine),
           "A main-only bundled drum runtime becomes ready.");
    const auto mainOnly =
        mainOnlyEngine.renderActiveBlockWithMidiForTesting(
            kick,
            1024);
    expect(magnitude(routed, 0)
               > magnitude(mainOnly, 0) * 20.0f,
           "A processor-output route carries the kick stem through the normal audio graph.");

    auto renderProject = studio::Project::createDefault();
    auto& renderTrack = addInstrumentTrack(
        renderProject,
        "Rendered drums");
    const auto renderInsert = addBundledInsert(
        renderTrack,
        "studio.device.drum-composer",
        "Metal Drum Composer");
    studio::MidiClip clip;
    clip.name = "Drum render";
    clip.editorMode = studio::MidiEditorMode::drums;
    clip.drumMapId = renderProject.drumMaps.front().id;
    clip.durationBeats = 1.0;
    studio::MidiNote note;
    note.pitch = 38;
    note.velocity = 116;
    note.durationBeats = 0.25;
    note.roundRobinHint = 1;
    const auto* entry =
        renderProject.drumMaps.front().entryForPitch(note.pitch);
    note.drumMapEntryId = entry->id;
    note.articulation = entry->articulation;
    renderTrack.midiClips.push_back(clip);
    renderTrack.midiClips.back().notes.push_back(note);
    studio::AutomationLane drumAutomation;
    drumAutomation.name = "Drum main";
    drumAutomation.target.type =
        studio::AutomationTargetType::deviceParameter;
    drumAutomation.target.trackId = renderTrack.id;
    drumAutomation.target.insertId = renderInsert.id;
    drumAutomation.target.parameterId = "main";
    drumAutomation.target.parameterIndex = 0;
    drumAutomation.interpolation =
        studio::AutomationInterpolation::step;
    drumAutomation.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.82
    });
    renderProject.automationLanes.push_back(drumAutomation);

    studio::StudioAudioEngine renderEngine;
    juce::AudioBuffer<float> rendered;
    expect(renderEngine.renderToBuffer(
               renderProject,
               rendered,
               48000.0,
               { requestFor(renderTrack, renderInsert) })
               .wasOk(),
           "Offline rendering instantiates the bundled MIDI instrument.");
    expect(magnitude(rendered, 0, 0, 4096) > 0.01f,
           "Offline rendering schedules persisted drum notes into audio.");
}

void ampCabinetPublicationRace()
{
    const auto cabinetIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoPublicationRaceCabinet",
                ".wav",
                false);
    expect(writeCabinetIr(cabinetIr, 512),
           "The cabinet publication race fixture writes.");

    studio::AmpDeviceProcessor processing(
        studio::AmpDeviceType::guitar);
    studio::AmpDeviceProcessor reference(
        studio::AmpDeviceType::guitar);
    processing.prepareToPlay(48000.0, 2048);
    reference.prepareToPlay(48000.0, 2048);

    auto publicationsSucceeded = true;
    auto readersUsedPublishedKernel = true;
    auto currentIsCustom = false;
    for (int iteration = 0; iteration < 12; ++iteration)
    {
        const auto publishCustom = !currentIsCustom;
        currentIsCustom = publishCustom;
        const auto referenceResult = publishCustom
            ? reference.loadCabinetFile(cabinetIr)
            : reference.useEmbeddedDefaultCabinet();
        if (referenceResult.failed())
        {
            publicationsSucceeded = false;
            break;
        }

        processing.reset();
        reference.reset();
        auto actual = sineInput(
            2048,
            90.0 + static_cast<double>(iteration));
        auto expected = actual;
        studio::AmpDeviceProcessor::CabinetReaderBarrierForTesting
            barrier;
        processing.setCabinetReaderBarrierForTesting(&barrier);
        juce::MidiBuffer actualMidi;
        std::thread audioThread(
            [&processing, &actual, &actualMidi]
            {
                processing.processBlock(actual, actualMidi);
            });

        const auto readerPaused = waitForFlag(barrier.slotLoaded);
        const auto publicationResult = publishCustom
            ? processing.loadCabinetFile(cabinetIr)
            : processing.useEmbeddedDefaultCabinet();
        barrier.resume.store(true, std::memory_order_release);
        audioThread.join();
        processing.setCabinetReaderBarrierForTesting(nullptr);
        if (!readerPaused || publicationResult.failed())
        {
            publicationsSucceeded = false;
            break;
        }

        juce::MidiBuffer expectedMidi;
        reference.processBlock(expected, expectedMidi);
        auto difference = 0.0f;
        for (int channel = 0; channel < actual.getNumChannels(); ++channel)
        {
            for (int sample = 0; sample < actual.getNumSamples(); ++sample)
            {
                difference = std::max(
                    difference,
                    std::abs(
                        actual.getSample(channel, sample)
                        - expected.getSample(channel, sample)));
            }
        }
        readersUsedPublishedKernel =
            readersUsedPublishedKernel
            && difference < 0.00001f;
    }

    expect(publicationsSucceeded,
           "Repeated cabinet publication succeeds while processing is paused in reader acquisition.");
    expect(readersUsedPublishedKernel,
           "Cabinet readers retry acquisition when publication changes the active slot.");
    cabinetIr.deleteFile();
}

void ampProcessorAndCabinetState()
{
    auto guitar = studio::DeviceRegistry::create(
        "studio.device.guitar-amp");
    auto bass = studio::DeviceRegistry::create(
        "studio.device.bass-amp");
    guitar->prepareToPlay(48000.0, 4096);
    bass->prepareToPlay(48000.0, 4096);
    auto guitarAudio = sineInput(4096, 110.0);
    auto bassAudio = guitarAudio;
    juce::MidiBuffer midi;
    guitar->processBlock(guitarAudio, midi);
    bass->processBlock(bassAudio, midi);
    expect(guitar->getLatencySamples() == 128
               && bass->getLatencySamples() == 128
               && guitar->getTailLengthSeconds() > 0.0
               && bass->getTailLengthSeconds() > 0.0,
           "Cabinet convolution reports fixed latency and a finite tail.");
    expect(magnitude(guitarAudio, 0, 128, 3968) > 0.001f
               && magnitude(guitarAudio, 0, 128, 3968) < 2.0f
               && magnitude(bassAudio, 0, 128, 3968) > 0.001f
               && magnitude(bassAudio, 0, 128, 3968) < 2.0f,
           "Guitar and bass amps perform real nonlinear cabinet processing.");
    auto ampDifference = 0.0f;
    for (int sample = 128; sample < 4096; ++sample)
        ampDifference += std::abs(
            guitarAudio.getSample(0, sample)
            - bassAudio.getSample(0, sample));
    expect(ampDifference > 0.1f,
           "The guitar and bass amp signal paths have distinct voicing.");
    expect(std::all_of(
               guitar->getParameters().begin(),
               guitar->getParameters().end(),
               [](const auto* parameter)
               {
                   return parameter->isAutomatable();
               }),
           "Amp gain, tone, cabinet mix, and output controls are automatable.");

    const auto validIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoCabinet",
                ".wav",
                false);
    const auto longIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoLongCabinet",
                ".wav",
                false);
    const auto invalidIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoInvalidCabinet",
                ".txt",
                false);
    expect(writeCabinetIr(validIr, 512),
           "The synthetic cabinet fixture writes.");
    expect(writeCabinetIr(longIr, 1200),
           "The oversized synthetic cabinet fixture writes.");
    invalidIr.replaceWithText("not audio");

    auto* guitarAmp =
        dynamic_cast<studio::AmpDeviceProcessor*>(guitar.get());
    expect(guitarAmp != nullptr,
           "The registry creates the reusable amp processor.");
    const auto invalidResult =
        guitarAmp->loadCabinetFile(invalidIr);
    expect(invalidResult.failed()
               && invalidResult.getErrorMessage()
                      .containsIgnoreCase("valid audio"),
           "Invalid cabinet files return an explicit error.");
    const auto longResult =
        guitarAmp->loadCabinetFile(longIr);
    expect(longResult.failed()
               && longResult.getErrorMessage()
                      .containsIgnoreCase("too long"),
           "Cabinet IRs outside the bounded real-time configuration are rejected.");
    expect(guitarAmp->loadCabinetFile(validIr).wasOk()
               && guitarAmp->cabinetDescription().contains(
                   validIr.getFileName()),
           "A valid synthetic cabinet IR loads outside the audio callback.");
    setParameter(*guitar, "gain", 0.31f);
    setParameter(*guitar, "mid", 0.72f);
    juce::MemoryBlock state;
    auto* validated =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            guitar.get());
    expect(validated != nullptr
               && validated->saveValidatedState(state).wasOk(),
           "Amp parameters and cabinet data serialize together.");
    validIr.deleteFile();

    auto restored = studio::DeviceRegistry::create(
        "studio.device.guitar-amp");
    restored->prepareToPlay(48000.0, 4096);
    auto* restoredAmp =
        dynamic_cast<studio::AmpDeviceProcessor*>(
            restored.get());
    auto* restoredValidated =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            restored.get());
    expect(restoredValidated != nullptr
               && restoredValidated->restoreValidatedState(
                      state.getData(),
                      static_cast<int>(state.getSize()))
                      .wasOk()
               && restoredAmp->cabinetDescription().contains(
                   validIr.getFileName()),
           "Cabinet audio restores from opaque state after the source file is removed.");

    guitar->reset();
    restored->reset();
    auto originalOutput = sineInput(4096, 146.83);
    auto restoredOutput = originalOutput;
    guitar->processBlock(originalOutput, midi);
    restored->processBlock(restoredOutput, midi);
    auto stateDifference = 0.0f;
    for (int sample = 0; sample < 4096; ++sample)
        stateDifference = std::max(
            stateDifference,
            std::abs(
                originalOutput.getSample(0, sample)
                - restoredOutput.getSample(0, sample)));
    expect(stateDifference < 0.00001f,
           "Restored cabinet and amp state reproduce deterministic audio.");

    expect(restoredValidated->restoreValidatedState(
               state.getData(),
               static_cast<int>(state.getSize() / 2))
               .failed(),
           "Truncated cabinet state is rejected instead of silently using a fallback.");
    restored->setStateInformation(
        state.getData(),
        static_cast<int>(state.getSize() / 2));
    expect(restoredAmp->cabinetDescription()
               .containsIgnoreCase("error"),
           "Standalone state restoration exposes invalid cabinet state in the editor.");
    expect(restoredAmp->useEmbeddedDefaultCabinet().wasOk()
               && restoredAmp->cabinetDescription()
                      .containsIgnoreCase("embedded"),
           "A documented embedded cabinet is always available.");

    longIr.deleteFile();
    invalidIr.deleteFile();
}

void ampRuntimeStateAndAutomation()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.inputMonitoring = true;
    const auto insert = addBundledInsert(
        track,
        "studio.device.guitar-amp",
        "Guitar Amp");
    auto request = requestFor(track, insert);
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project, { request }).wasOk()
               && waitForRuntime(engine),
           "The guitar amp activates in the live insert graph.");
    const auto statuses = engine.pluginRuntimeStatuses();
    expect(statuses.size() == 1
               && statuses.front().state
                      == studio::StudioAudioEngine::
                          PluginRuntimeStatus::State::ready
               && statuses.front().latencySamples == 128
               && statuses.front().tailSeconds > 0.0
               && statuses.front().parameters.size() == 8,
           "Amp runtime metadata exposes latency, tail, and parameters.");
    juce::String error;
    expect(engine.setPluginParameter(
               insert.id,
               0,
               0.82f,
               error),
           error.toRawUTF8());
    const auto captures = engine.capturePluginStates(1000);
    expect(captures.size() == 1
               && captures.front().result.wasOk()
               && !captures.front().state.isEmpty(),
           "Live amp automation changes are captured in persistent state.");

    auto invalidRequest = request;
    const char invalidState[] { 'b', 'a', 'd' };
    invalidRequest.state.append(invalidState, sizeof(invalidState));
    studio::StudioAudioEngine invalidEngine;
    expect(invalidEngine.updateProject(
               project,
               { invalidRequest })
               .wasOk()
               && waitForRuntime(invalidEngine),
           "Invalid bundled state remains a recoverable runtime transition.");
    const auto invalidStatuses =
        invalidEngine.pluginRuntimeStatuses();
    expect(invalidStatuses.size() == 1
               && invalidStatuses.front().state
                      == studio::StudioAudioEngine::
                          PluginRuntimeStatus::State::failed
               && invalidStatuses.front().message
                      .containsIgnoreCase("state"),
           "Invalid cabinet state is reported through the existing missing/failed processor UI.");

    auto renderProject = studio::Project::createDefault();
    auto& renderTrack = renderProject.tracks.front();
    const auto generator = addBundledInsert(
        renderTrack,
        "studio.device.generator",
        "Signal Generator");
    const auto amp = addBundledInsert(
        renderTrack,
        "studio.device.bass-amp",
        "Bass Amp");
    studio::StudioAudioEngine renderEngine;
    juce::AudioBuffer<float> rendered;
    expect(renderEngine.renderToBuffer(
               renderProject,
               rendered,
               48000.0,
               {
                   requestFor(renderTrack, generator),
                   requestFor(renderTrack, amp)
               })
               .wasOk(),
           "The bundled amp renders through the processor-inclusive offline path.");
    expect(rendered.getNumSamples() > 8 * 48000
               && magnitude(rendered, 0, 256, 4096) > 0.001f,
           "Offline amp rendering includes reported latency, tail, and processed audio.");
}
}

void bundledDeviceTests()
{
    registryMetadata();
    drumProcessor();
    drumTailAndVoiceReuse();
    liveDrumPadAudio();
    drumEngineRoutingAndRender();
    ampCabinetPublicationRace();
    ampProcessorAndCabinetState();
    ampRuntimeStateAndAutomation();
}
