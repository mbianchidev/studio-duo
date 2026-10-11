#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "devices/DeviceRegistry.h"
#include "devices/DrumDeviceProcessor.h"
#include "plugin_host/ClapPluginFormat.h"

#include <algorithm>
#include <map>
#include <cmath>

namespace
{
studio::PluginInsert rack(const juce::String& name, const juce::String& parent = {})
{
    studio::PluginInsert insert;
    insert.name = name;
    insert.kind = studio::PluginInsertKind::serialRack;
    insert.containerId = parent;
    return insert;
}

studio::PluginInsert device(const juce::String& identifier, const juce::String& parent)
{
    studio::PluginInsert insert;
    insert.name = identifier;
    insert.pluginIdentifier = identifier;
    insert.bundledDevice = true;
    insert.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    insert.containerId = parent;
    return insert;
}

std::vector<studio::StudioAudioEngine::PluginRuntimeRequest> requestsFor(
    const studio::Project& project,
    const std::map<juce::String, juce::MemoryBlock>& states = {})
{
    std::vector<studio::StudioAudioEngine::PluginRuntimeRequest> requests;
    for (const auto& track : project.tracks)
        for (const auto& insert : track.inserts)
        {
            if (insert.isContainer())
                continue;
            studio::StudioAudioEngine::PluginRuntimeRequest request;
            request.trackId = track.id;
            request.insertId = insert.id;
            request.name = insert.name;
            request.deviceIdentifier = insert.pluginIdentifier;
            request.bridgeMode = insert.bridgeMode;
            request.latencySamples = insert.latencySamples;
            request.tailSeconds = insert.tailSeconds;
            if (const auto state = states.find(insert.id); state != states.cend())
                request.state = state->second;
            requests.push_back(std::move(request));
        }
    return requests;
}

bool waitForRuntime(studio::StudioAudioEngine& engine)
{
    for (auto attempt = 0; attempt < 500; ++attempt)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        if (!engine.pluginRuntimeTransitionPending())
        {
            const auto statuses = engine.pluginRuntimeStatuses();
            return std::all_of(statuses.cbegin(), statuses.cend(), [](const auto& status)
            {
                return status.state == studio::StudioAudioEngine::PluginRuntimeStatus::State::ready
                    || status.state == studio::StudioAudioEngine::PluginRuntimeStatus::State::bypassed;
            });
        }
    }
    return false;
}

void rackRenderAndMidiTests()
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("studio-duo-rack-signal", ".wav");
    {
        juce::AudioBuffer<float> signal(2, 2048);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < signal.getNumSamples(); ++sample)
                signal.setSample(channel, sample, 0.25f);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
        expect(writer != nullptr && writer->writeFromAudioSampleBuffer(signal, 0, signal.getNumSamples()),
               "The rack render test writes only synthetic audio.");
        if (writer != nullptr)
            writer->flush();
    }
    auto project = studio::Project::createDefault();
    project.metronomeEnabled = false;
    studio::AudioClip clip;
    clip.sourceFile = file;
    clip.durationSeconds = clip.sourceLengthSeconds = 2048.0 / 48000.0;
    project.tracks.front().clips = { clip };
    project.tracks.front().volumeDecibels = -60.0f;
    studio::Track aux;
    aux.type = studio::TrackType::aux;
    aux.name = "Parallel send return";
    auto splitter = rack("Send splitter");
    splitter.kind = studio::PluginInsertKind::parallelSplitter;
    auto branchA = rack("Processed send", splitter.id);
    auto branchB = rack("Dry send", splitter.id);
    branchA.gainDecibels = branchB.gainDecibels = -6.0206f;
    auto gain = device("studio.device.gain", branchA.id);
    aux.inserts = { splitter, branchA, gain, branchB };
    project.tracks.insert(project.tracks.end() - 1, aux);
    studio::RoutingConnection send;
    send.name = "Parallel FX send";
    send.kind = studio::RouteKind::send;
    send.tap = studio::RouteTap::preFader;
    send.sourceTrackId = project.tracks.front().id;
    send.destination.trackId = aux.id;
    project.routingConnections.push_back(send);
    auto gainProcessor = studio::DeviceRegistry::create(gain.pluginIdentifier);
    gainProcessor->getParameters()[0]->setValue(0.625f);
    juce::MemoryBlock gainState;
    gainProcessor->getStateInformation(gainState);
    studio::StudioAudioEngine engine;
    juce::AudioBuffer<float> rendered;
    const studio::StudioAudioEngine::RenderRange range { 0.0, 2048.0 / 48000.0, 0.0 };
    const auto result = engine.renderRangeToBuffer(project, rendered, 48000.0, range,
                                                    requestsFor(project, { { gain.id, gainState } }));
    const auto expected = 0.25f * (juce::Decibels::decibelsToGain(6.0f) * 0.5f + 0.5f)
        + 0.25f * juce::Decibels::decibelsToGain(-60.0f);
    expect(result.wasOk() && rendered.getNumSamples() == 2048
               && std::abs(rendered.getSample(0, 512) - expected) < 0.000002f
               && std::abs(rendered.getSample(1, 512) - expected) < 0.000002f,
           "Offline sends render adjustable parallel FX on aux inserts without changing the dry track.");

    auto limiter = device("studio.device.limiter", branchB.id);
    auto polarity = device("studio.device.polarity", branchB.id);
    auto invert = studio::DeviceRegistry::create(polarity.pluginIdentifier);
    invert->getParameters()[0]->setValue(1.0f);
    juce::MemoryBlock invertState;
    invert->getStateInformation(invertState);
    auto insertProject = studio::Project::createDefault();
    insertProject.metronomeEnabled = false;
    insertProject.tracks.front().clips = { clip };
    auto outer = rack("Nested insert rack");
    splitter.containerId = outer.id;
    insertProject.tracks.front().inserts = { outer, splitter, branchA, gain, branchB, limiter, polarity };
    const auto aligned = engine.renderRangeToBuffer(insertProject, rendered, 48000.0, range,
                                                     requestsFor(insertProject, { { polarity.id, invertState } }));
    expect(aligned.wasOk() && rendered.getNumSamples() == 2048
               && rendered.getMagnitude(0, 0, rendered.getNumSamples()) < 0.000002f,
           "Offline nested inserts align unequal branch latencies and trim the compensated prefix.");
    expect(file.deleteFile(), "The synthetic rack render audio is removed.");

    auto midiProject = studio::Project::createDefault();
    midiProject.metronomeEnabled = false;
    studio::Track instrument;
    instrument.type = studio::TrackType::instrument;
    instrument.name = "Mock nested drum instruments";
    instrument.armed = true;
    auto drum = device("studio.device.drum-composer", {});
    instrument.inserts = { drum };
    midiProject.tracks.insert(midiProject.tracks.end() - 1, instrument);
    studio::StudioAudioEngine referenceEngine;
    expect(referenceEngine.updateProject(midiProject, requestsFor(midiProject)).wasOk()
               && waitForRuntime(referenceEngine), "The reference bundled instrument is ready.");
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, juce::uint8(112)), 17);
    midi.addEvent(juce::MidiMessage::noteOff(1, 36), 127);
    const auto reference = referenceEngine.renderActiveBlockWithMidiForTesting(midi, 1024);
    auto secondDrum = drum;
    secondDrum.id = juce::Uuid().toString();
    drum.containerId = branchA.id;
    secondDrum.containerId = branchB.id;
    midiProject.findTrack(instrument.id)->inserts = { outer, splitter, branchA, drum, branchB, secondDrum };
    studio::StudioAudioEngine layeredEngine;
    expect(layeredEngine.updateProject(midiProject, requestsFor(midiProject)).wasOk()
               && waitForRuntime(layeredEngine), "Nested parallel instrument branches are ready.");
    const auto layered = layeredEngine.renderActiveBlockWithMidiForTesting(midi, 1024);
    auto difference = 0.0f;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 1024; ++sample)
            difference = std::max(difference,
                std::abs(reference.getSample(channel, sample) - layered.getSample(channel, sample)));
    expect(reference.getMagnitude(0, 0, reference.getNumSamples()) > 0.001f && difference < 0.000002f,
           "Nested instrument branches receive identical sample-offset MIDI and layer at their saved output levels.");

    studio::Track source;
    source.type = studio::TrackType::midi;
    source.armed = true;
    splitter.containerId.clear();
    source.inserts = { splitter, branchA, branchB };
    auto* destination = midiProject.findTrack(instrument.id);
    destination->armed = false;
    drum.containerId.clear();
    destination->inserts = { drum };
    midiProject.tracks.insert(midiProject.tracks.end() - 1, source);
    studio::RoutingConnection midiRoute;
    midiRoute.signalType = studio::SignalType::midi;
    midiRoute.sourceTrackId = source.id;
    midiRoute.destination.trackId = instrument.id;
    midiProject.routingConnections.push_back(midiRoute);
    studio::StudioAudioEngine midiEngine;
    expect(midiEngine.updateProject(midiProject, requestsFor(midiProject)).wasOk()
               && waitForRuntime(midiEngine), "A parallel MIDI rack routes to a downstream instrument.");
    juce::ignoreUnused(midiEngine.renderActiveBlockWithMidiForTesting(midi, 1024));
    const auto events = midiEngine.takeMidiEventsForTesting();
    const auto destinationKey = studio::StudioAudioEngine::runtimeKeyForTesting(instrument.id);
    expect(std::count_if(events.cbegin(), events.cend(), [destinationKey](const auto& event)
        { return event.runtimeKey == destinationKey && event.size == 3
              && (event.data[0] & 0xf0) == 0x90 && event.data[1] == 36 && event.samplePosition == 17; }) == 1,
           "Parallel MIDI passthrough is merged without duplicating notes at downstream instruments.");

    auto stemProject = studio::Project::createDefault();
    stemProject.metronomeEnabled = false;
    instrument.inserts = { drum };
    instrument.armed = true;
    stemProject.tracks.insert(stemProject.tracks.end() - 1, instrument);
    studio::Track stem;
    stem.name = "Mock nested kick stem";
    stem.type = studio::TrackType::aux;
    stemProject.tracks.insert(stemProject.tracks.end() - 1, stem);
    studio::RoutingConnection stemRoute;
    stemRoute.sourceTrackId = instrument.id;
    stemRoute.sourceInsertId = drum.id;
    stemRoute.sourceBusIndex = studio::DrumDeviceProcessor::kickOutput;
    stemRoute.tap = studio::RouteTap::preFader;
    stemRoute.destination.trackId = stem.id;
    stemProject.routingConnections.push_back(stemRoute);
    auto stemProcessor = studio::DeviceRegistry::create(drum.pluginIdentifier);
    for (auto* parameter : stemProcessor->getParameters())
        if (const auto* named = dynamic_cast<const juce::AudioProcessorParameterWithID*>(parameter);
            named != nullptr && (named->paramID == "main" || named->paramID == "room"))
            parameter->setValue(0.0f);
    juce::MemoryBlock stemState;
    stemProcessor->getStateInformation(stemState);
    studio::StudioAudioEngine plainStemEngine;
    expect(plainStemEngine.updateProject(stemProject, requestsFor(stemProject, { { drum.id, stemState } })).wasOk()
               && waitForRuntime(plainStemEngine), "The mock bundled auxiliary output is ready.");
    const auto plainStem = plainStemEngine.renderActiveBlockWithMidiForTesting(midi, 1024);
    outer.containerId.clear();
    outer.gainDecibels = -6.0206f;
    drum.containerId = outer.id;
    stemProject.findTrack(instrument.id)->inserts = { outer, drum };
    studio::StudioAudioEngine nestedStemEngine;
    expect(nestedStemEngine.updateProject(stemProject, requestsFor(stemProject, { { drum.id, stemState } })).wasOk()
               && waitForRuntime(nestedStemEngine), "The serial-rack auxiliary output is ready.");
    const auto nestedStem = nestedStemEngine.renderActiveBlockWithMidiForTesting(midi, 1024);
    difference = 0.0f;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 1024; ++sample)
            difference = std::max(difference,
                std::abs(plainStem.getSample(channel, sample) * 0.5f - nestedStem.getSample(channel, sample)));
    expect(plainStem.getMagnitude(0, 0, 1024) > 0.001f && difference < 0.000002f,
           "A serial rack's output level applies to routed auxiliary outputs as well as its main bus.");
}
}

void pluginRackEngineTests()
{
    rackRenderAndMidiTests();
    auto project = studio::Project::createDefault();
    project.metronomeEnabled = false;
    auto& track = project.tracks.front();
    track.inputMonitoring = true;
    track.stereoInput = true;
    auto splitter = rack("Parallel cancellation");
    splitter.kind = studio::PluginInsertKind::parallelSplitter;
    auto branchA = rack("Dry", splitter.id);
    auto branchB = rack("Inverted", splitter.id);
    branchA.gainDecibels = -6.0206f;
    branchB.gainDecibels = -6.0206f;
    auto gain = device("studio.device.gain", branchA.id);
    auto polarity = device("studio.device.polarity", branchB.id);
    track.inserts = { splitter, branchA, gain, branchB, polarity };
    auto invert = studio::DeviceRegistry::create(polarity.pluginIdentifier);
    invert->getParameters()[0]->setValue(1.0f);
    juce::MemoryBlock invertState;
    invert->getStateInformation(invertState);
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine),
           "Parallel branches prepare their ordinary plugin runtimes.");
    juce::AudioBuffer<float> input(2, 1024);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < input.getNumSamples(); ++sample)
            input.setSample(channel, sample, 0.25f);
    const auto output = engine.renderActiveBlockWithInputForTesting(input);
    expect(output.getMagnitude(0, 0, output.getNumSamples()) < 0.000001f
               && output.getMagnitude(1, 0, output.getNumSamples()) < 0.000001f,
           "Parallel effect branches receive independent copies of the same input and sum, rather than processing serially.");

    auto limiter = device("studio.device.limiter", branchB.id);
    project.tracks.front().inserts.insert(project.tracks.front().inserts.end() - 1, limiter);
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine),
           "A longer parallel branch prepares its actual reported plugin latency.");
    input.clear();
    input.setSample(0, 13, 0.1f);
    input.setSample(1, 13, 0.1f);
    const auto aligned = engine.renderActiveBlockWithInputForTesting(input);
    expect(aligned.getMagnitude(0, 0, aligned.getNumSamples()) < 0.000001f
               && aligned.getMagnitude(1, 0, aligned.getNumSamples()) < 0.000001f,
           "Parallel branches with different processor latencies align sample-exactly before summing.");
    expect(engine.resetPluginProcessing(), "Seeking can reset rack compensation buffers in place.");
    const auto reset = engine.renderActiveBlockWithInputForTesting(input);
    expect(reset.getMagnitude(0, 0, reset.getNumSamples()) < 0.000001f,
           "Reset clears parallel delay histories along with the leaf processors.");
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < input.getNumSamples(); ++sample)
            input.setSample(channel, sample, 0.25f);
    juce::ignoreUnused(engine.renderActiveBlockWithInputForTesting(input));
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine), "Rack snapshots can refresh without replacing their processors.");
    const auto afterSnapshot = engine.renderActiveBlockWithInputForTesting(input);
    expect(afterSnapshot.getMagnitude(0, 0, afterSnapshot.getNumSamples()) < 0.000001f,
           "Snapshot-only edits retain existing parallel delay history instead of producing startup silence or phase jumps.");

    juce::String error;
    expect(engine.setPluginParameter(gain.id, 0, 0.625f, error), error.toRawUTF8());
    project.tracks.front().inserts[1].gainDecibels = -12.0f;
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine),
           "Changing a rack branch level rebuilds the processing tree.");
    const auto statuses = engine.pluginRuntimeStatuses();
    const auto gainStatus = std::find_if(statuses.cbegin(), statuses.cend(),
        [&gain](const auto& status) { return status.insertId == gain.id; });
    expect(gainStatus != statuses.cend() && !gainStatus->parameters.empty()
               && std::abs(gainStatus->parameters.front().value - 0.625f) < 0.000001f,
           "Rack placement and branch-level edits preserve unsaved settings of existing plugins.");
    project.tracks.front().inserts.front().bypassed = true;
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine), "A splitter bypasses all of its child runtimes.");
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine), "A snapshot-only update can occur while a rack is bypassed.");
    project.tracks.front().inserts.front().bypassed = false;
    expect(engine.updateProject(project, requestsFor(project, { { polarity.id, invertState } })).wasOk()
               && waitForRuntime(engine), "The bypassed rack can be re-enabled.");
    const auto reenabledStatuses = engine.pluginRuntimeStatuses();
    const auto reenabledGain = std::find_if(reenabledStatuses.cbegin(), reenabledStatuses.cend(),
        [&gain](const auto& status) { return status.insertId == gain.id; });
    expect(reenabledGain != reenabledStatuses.cend() && !reenabledGain->parameters.empty()
               && std::abs(reenabledGain->parameters.front().value - 0.625f) < 0.000001f,
           "Bypass, snapshot refresh, and re-enable retain unsaved child-plugin settings.");

    studio::ClapPluginFormat clapFormat;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    clapFormat.findAllTypesForFile(descriptions, STUDIO_DUO_CLAP_FIXTURE_PATH);
    expect(descriptions.size() == 1, "The synthetic CLAP fixture is available for dynamic rack timing.");
    if (descriptions.isEmpty())
        return;
    auto dynamicProject = studio::Project::createDefault();
    dynamicProject.metronomeEnabled = false;
    dynamicProject.tracks.front().inputMonitoring = true;
    auto external = device("fixture", branchA.id);
    external.bundledDevice = false;
    external.pluginIdentifier = descriptions[0]->createIdentifierString();
    dynamicProject.tracks.front().inserts = { splitter, branchA, external, branchB };
    studio::StudioAudioEngine::PluginRuntimeRequest externalRequest;
    externalRequest.trackId = dynamicProject.tracks.front().id;
    externalRequest.insertId = external.id;
    externalRequest.name = "Dynamic rack fixture";
    externalRequest.description = *descriptions[0];
    externalRequest.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    studio::StudioAudioEngine dynamicEngine;
    expect(dynamicEngine.updateProject(dynamicProject, { externalRequest }).wasOk()
               && waitForRuntime(dynamicEngine)
               && dynamicEngine.parallelBranchDelaysForTesting() == std::vector<int> { 0, 0 },
           "Parallel compensation uses the instantiated processor's initial latency.");
    const auto dynamicStatuses = dynamicEngine.pluginRuntimeStatuses();
    if (dynamicStatuses.empty())
        return;
    const auto trigger = std::find_if(dynamicStatuses.front().parameters.cbegin(),
        dynamicStatuses.front().parameters.cend(),
        [](const auto& parameter) { return parameter.id == "293"; });
    expect(trigger != dynamicStatuses.front().parameters.cend(), "The synthetic latency-change parameter exists.");
    if (trigger == dynamicStatuses.front().parameters.cend())
        return;
    expect(dynamicEngine.setPluginParameter(external.id, trigger->index, 1.0f, error), error.toRawUTF8());
    dynamicEngine.play();
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        dynamicEngine.processActiveBlockForTesting(64);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        juce::ignoreUnused(dynamicEngine.pluginRuntimeStatuses());
        if (!dynamicEngine.pluginRuntimeTransitionPending()
            && dynamicEngine.parallelBranchDelaysForTesting() == std::vector<int> { 0, 64 })
            break;
    }
    expect(dynamicEngine.parallelBranchDelaysForTesting() == std::vector<int> { 0, 64 },
           ("Live plugin latency changes refresh parallel branch alignment without rebuilding plugin instances (latency "
            + juce::String(dynamicEngine.pluginRuntimeStatuses().front().latencySamples)
            + ", branch delay " + juce::String(dynamicEngine.parallelBranchDelaysForTesting().back())
            + ").").toRawUTF8());
}
