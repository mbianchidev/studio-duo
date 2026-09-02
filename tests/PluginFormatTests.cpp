#include "TestHarness.h"
#include "TestSuites.h"

#include "plugin_host/PluginFormats.h"
#include "plugin_host/ClapPluginFormat.h"
#include "plugin_host/PluginBridgeClient.h"
#include "plugin_host/ValidatedPluginStateTarget.h"
#include "model/ProjectCommands.h"
#include "audio/StudioAudioEngine.h"

#include <atomic>
#include <thread>

void pluginFormatTests()
{
    juce::AudioPluginFormatManager manager;
    studio::PluginFormats::addSupportedFormats(manager);

    juce::StringArray names;
    for (auto* format : manager.getFormats())
        names.add(format->getName());
    expect(names.contains("VST3"),
           "Supported plugin formats include VST3.");
#if JUCE_MAC
    expect(names.contains("AudioUnit"),
           "Supported plugin formats include Audio Units on macOS.");
#endif
    expect(names.contains("CLAP"),
           "Supported plugin formats include CLAP.");

    studio::ClapPluginFormat clapFormat;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    clapFormat.findAllTypesForFile(descriptions,
                                   STUDIO_DUO_CLAP_FIXTURE_PATH);
    expect(descriptions.size() == 1
               && descriptions[0]->pluginFormatName == "CLAP",
           "CLAP scanning reads public descriptors from a bundle.");
    if (descriptions.isEmpty())
        return;

    juce::String error;
    auto instance = clapFormat.createInstanceFromDescription(
        *descriptions[0],
        48000.0,
        64,
        error);
    expect(instance != nullptr, error.toRawUTF8());
    if (instance == nullptr)
        return;
    auto* validatedState =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            instance.get());
    expect(validatedState != nullptr,
           "CLAP instances expose validated state errors.");

    instance->prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 64);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            audio.setSample(channel, sample, 1.0f);
    juce::MidiBuffer midi;
    instance->processBlock(audio, midi);
    expect(std::abs(audio.getSample(0, 32) - 0.5f) < 0.0001f,
           "CLAP instances process audio.");

    expect(instance->getParameters().size() == 300,
           "CLAP parameters are exposed without truncation.");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    const auto parameterCount = instance->getParameters().size();
    const auto* removedParameter = dynamic_cast<
        juce::HostedAudioProcessorParameter*>(
        parameterCount > 249
            ? instance->getParameters()[249]
            : nullptr);
    const auto* followingParameter = dynamic_cast<
        juce::HostedAudioProcessorParameter*>(
        parameterCount > 250
            ? instance->getParameters()[250]
            : nullptr);
    const auto* addedParameter = dynamic_cast<
        juce::HostedAudioProcessorParameter*>(
        parameterCount > 300
            ? instance->getParameters()[300]
            : nullptr);
    expect(instance->getLatencySamples() == 32
              && parameterCount == 301
              && instance->getParameters()[0]->getName(128)
                     == "Gain rescanned"
              && removedParameter != nullptr
              && removedParameter->getParameterID() == "250"
              && !removedParameter->isAutomatable()
              && followingParameter != nullptr
              && followingParameter->getParameterID() == "251"
              && addedParameter != nullptr
              && addedParameter->getParameterID() == "301"
              && instance->getParameters()[298]->getValue() > 0.0f
              && std::abs(instance->getParameters()[299]->getValue())
                     < 0.0001f,
           ("CLAP rescans preserve stable parameter-ID slots while restart, callback, latency, and thread-check requests drain"
            " (count "
            + juce::String(parameterCount)
            + ", removed "
            + (removedParameter != nullptr
                   ? removedParameter->getParameterID()
                   : juce::String("none"))
            + ", following "
            + (followingParameter != nullptr
                   ? followingParameter->getParameterID()
                   : juce::String("none"))
            + ", added "
            + (addedParameter != nullptr
                   ? addedParameter->getParameterID()
                   : juce::String("none"))
            + ", latency "
            + juce::String(instance->getLatencySamples())
            + ", name "
            + instance->getParameters()[0]->getName(128)
            + ", removed automatable "
            + juce::String(
                  removedParameter != nullptr
                      && removedParameter->isAutomatable()
                      ? 1
                      : 0)
            + ", callbacks "
            + juce::String(
                  parameterCount > 298
                      ? instance->getParameters()[298]
                            ->getValue()
                      : -1.0f)
            + ", violations "
            + juce::String(
                  parameterCount > 299
                      ? instance->getParameters()[299]
                            ->getValue()
                      : -1.0f)
            + ").")
               .toRawUTF8());
    auto* exactAutomation =
        dynamic_cast<studio::SampleAccurateAutomationTarget*>(
            instance.get());
    studio::PluginBridgeParameterEvent rangedEvent;
    rangedEvent.parameterIndex = 1;
    rangedEvent.sampleOffset = 17;
    rangedEvent.value = 0.5f;
    auto oversizedEvent = rangedEvent;
    oversizedEvent.parameterIndex = 0;
    oversizedEvent.sampleOffset = 0;
    oversizedEvent.flags =
        studio::PluginBridgeParameterEvent::rampFlag;
    oversizedEvent.rampEndOffset = 65536;
    oversizedEvent.rampEndValue = 1.0f;
    expect(exactAutomation != nullptr
               && !exactAutomation
                       ->supportsSampleAccurateAutomation(
                           std::span<
                               const studio::PluginBridgeParameterEvent>(
                               &oversizedEvent,
                               1)),
           "CLAP rejects oversized exact automation before partial delivery.");
    if (exactAutomation != nullptr)
    {
        audio.clear();
        exactAutomation->processBlockWithAutomation(
            audio,
            midi,
            std::span<const studio::PluginBridgeParameterEvent>(
                &rangedEvent,
                1));
    }
    juce::MemoryBlock rangedState;
    expect(exactAutomation != nullptr
               && validatedState->saveValidatedState(rangedState).wasOk()
               && std::abs(instance->getParameters()[1]->getValue() - 0.5f)
                      < 0.0001f,
           "CLAP timed automation converts normalized values to plain parameter ranges.");
    juce::MemoryBlock state;
    instance->getStateInformation(state);
    instance->getParameters()[0]->setValueNotifyingHost(0.25f);
    instance->setStateInformation(state.getData(),
                                  static_cast<int>(state.getSize()));
    expect(std::abs(instance->getParameters()[0]->getValue() - 0.5f)
               < 0.0001f,
           "CLAP opaque state restores parameter values.");
    instance->getParameters()[257]->setValueNotifyingHost(0.75f);
    instance->processBlock(audio, midi);
    juce::MemoryBlock highParameterState;
    instance->getStateInformation(highParameterState);
    instance->getParameters()[257]->setValueNotifyingHost(0.1f);
    instance->processBlock(audio, midi);
    instance->setStateInformation(
        highParameterState.getData(),
        static_cast<int>(highParameterState.getSize()));
    expect(std::abs(instance->getParameters()[257]->getValue() - 0.75f)
               < 0.0001f,
           "CLAP parameters beyond index 256 reach processing and state.");
    instance->getParameters()[295]->setValueNotifyingHost(1.0f);
    instance->processBlock(audio, midi);
    juce::MemoryBlock failedState;
    expect(validatedState != nullptr
               && validatedState->saveValidatedState(failedState).failed()
               && failedState.isEmpty(),
           "CLAP state save failures discard partial output.");
    instance->getParameters()[295]->setValueNotifyingHost(0.0f);
    instance->getParameters()[296]->setValueNotifyingHost(1.0f);
    instance->processBlock(audio, midi);
    expect(validatedState != nullptr
               && validatedState->saveValidatedState(failedState).failed()
               && failedState.isEmpty(),
           "CLAP partial state blobs fail round-trip validation.");
    instance->getParameters()[296]->setValueNotifyingHost(0.0f);
    instance->processBlock(audio, midi);
    instance->getParameters()[0]->setValueNotifyingHost(0.4f);
    instance->getParameters()[294]->setValueNotifyingHost(1.0f);
    instance->processBlock(audio, midi);
    expect(validatedState != nullptr
               && validatedState
                      ->saveValidatedState(failedState)
                      .failed()
               && failedState.isEmpty()
               && std::abs(
                      instance->getParameters()[0]->getValue()
                      - 0.4f)
                      < 0.0001f,
           "CLAP state validation uses a disposable instance and cannot mutate live DSP state on failure.");
    instance->getParameters()[294]->setValueNotifyingHost(0.0f);
    instance->processBlock(audio, midi);
    const std::uint8_t malformedState = 0;
    expect(validatedState != nullptr
               && validatedState->restoreValidatedState(
               &malformedState,
               1)
               .failed(),
           "CLAP state load failures are reported.");
    instance->releaseResources();
    instance->getParameters()[0]->setValueNotifyingHost(0.25f);
    juce::MemoryBlock stoppedState;
    instance->getStateInformation(stoppedState);
    instance->getParameters()[0]->setValueNotifyingHost(0.75f);
    instance->setStateInformation(
        stoppedState.getData(),
        static_cast<int>(stoppedState.getSize()));
    expect(std::abs(instance->getParameters()[0]->getValue() - 0.25f)
               < 0.0001f,
           "CLAP edits flush and persist while processing is stopped.");
    instance->getParameters()[297]->setValueNotifyingHost(0.75f);
    expect(validatedState != nullptr
               && validatedState->saveValidatedState(stoppedState).wasOk()
               && std::abs(
                      instance->getParameters()[297]->getValue()
                      - 0.75f)
                      < 0.0001f
               && std::abs(
                      instance->getParameters()[299]->getValue())
                      < 0.0001f,
           "Process-only CLAP parameters receive a silent process block and persist while stopped.");
    instance->releaseResources();

    studio::PluginBridgeClient sandbox(
        juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH));
    const auto sandboxStart = sandbox.startPlugin(
        *descriptions[0],
        48000.0,
        64);
    auto sandboxCaptureSucceeded = sandboxStart.wasOk();
    std::atomic<bool> keepProcessing { sandboxCaptureSucceeded };
    std::thread sandboxAudioThread;
    if (sandboxCaptureSucceeded)
    {
        sandboxAudioThread = std::thread(
            [&sandbox, &keepProcessing]
            {
                juce::AudioBuffer<float> block(2, 64);
                while (keepProcessing.load(
                    std::memory_order_acquire))
                {
                    block.clear();
                    sandbox.processBlock(block);
                }
            });
        for (auto capture = 0;
             capture < 12 && sandboxCaptureSucceeded;
             ++capture)
        {
            juce::MemoryBlock captured;
            sandboxCaptureSucceeded =
                sandbox.requestState(
                    captured,
                    std::chrono::seconds(2))
                    .wasOk()
                && !captured.isEmpty();
        }
        keepProcessing.store(false, std::memory_order_release);
        sandboxAudioThread.join();
    }
    sandbox.stop();
    expect(
        sandboxCaptureSucceeded,
        (juce::String(
             "Sandbox CLAP state capture remains deadlock-free under concurrent processing")
         + (sandboxStart.failed()
                ? ": " + sandboxStart.getErrorMessage()
                : juce::String()))
            .toRawUTF8());

    const auto monoSource = juce::File::getSpecialLocation(
                                juce::File::tempDirectory)
                                .getNonexistentChildFile(
                                    "StudioDuoMonoPlugin",
                                    ".wav",
                                    false);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            monoSource.createOutputStream();
        auto writer = wav.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions {}
                .withSampleRate(48000.0)
                .withNumChannels(2)
                .withBitsPerSample(24));
        juce::AudioBuffer<float> source(2, 64);
        for (int sample = 0; sample < source.getNumSamples(); ++sample)
        {
            source.setSample(0, sample, 0.2f);
            source.setSample(1, sample, 0.8f);
        }
        expect(writer != nullptr
                   && writer->writeFromAudioSampleBuffer(
                       source,
                       0,
                       source.getNumSamples()),
               "Mono plugin source can be written.");
    }
    auto monoProject = studio::Project::createDefault();
    studio::AudioClip monoClip;
    monoClip.sourceFile = monoSource;
    monoClip.durationSeconds = 64.0 / 48000.0;
    monoClip.sourceLengthSeconds = monoClip.durationSeconds;
    monoClip.sourceRangeEndSeconds = monoClip.durationSeconds;
    monoProject.tracks.front().clips.push_back(monoClip);
    studio::PluginInsert monoInsert;
    monoInsert.pluginIdentifier =
        descriptions[0]->createIdentifierString();
    monoInsert.name = descriptions[0]->name;
    monoInsert.format = "CLAP";
    monoInsert.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    monoProject.tracks.front().inserts.push_back(monoInsert);
    studio::AutomationLane monoAutomation;
    monoAutomation.target.type =
        studio::AutomationTargetType::pluginParameter;
    monoAutomation.target.trackId = monoProject.tracks.front().id;
    monoAutomation.target.insertId = monoInsert.id;
    monoAutomation.target.parameterId = "1";
    monoAutomation.target.parameterIndex = 0;
    monoAutomation.interpolation =
        studio::AutomationInterpolation::linear;
    monoAutomation.points = {
        { juce::Uuid().toString(), 0.0, 0.5 },
        { juce::Uuid().toString(), 64.0 / 48000.0, 1.0 }
    };
    monoProject.automationLanes.push_back(monoAutomation);
    studio::StudioAudioEngine::PluginRuntimeRequest monoRequest;
    monoRequest.trackId = monoProject.tracks.front().id;
    monoRequest.insertId = monoInsert.id;
    monoRequest.name = monoInsert.name;
    monoRequest.description = *descriptions[0];
    monoRequest.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    studio::StudioAudioEngine monoEngine;
    juce::AudioBuffer<float> monoRender;
    expect(monoEngine.renderToBuffer(
               monoProject,
               monoRender,
               48000.0,
               { monoRequest })
               .wasOk(),
           "Mono-output CLAP renders in process.");
    expect(monoRender.getNumChannels() == 2
               && monoRender.getNumSamples() > 32
               && std::abs(
                      monoRender.getSample(0, 17)
                      - 0.2f * (0.5f + 0.5f * 17.0f / 64.0f))
                      < 0.002f
               && std::abs(monoRender.getSample(1, 33)
                           - monoRender.getSample(0, 33))
                      < 0.0001f,
           "CLAP automation is sample-exact in one process call and mono output duplicates to stereo.");
    std::atomic<bool> runRealtimeStress { true };
    std::thread realtimeStressThread(
        [&monoEngine, &runRealtimeStress]
        {
            while (runRealtimeStress.load(
                std::memory_order_acquire))
            {
                monoEngine.seekSeconds(0.0);
                monoEngine.play();
                monoEngine.processActiveBlockForTesting(64);
            }
        });
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    const auto monoStatuses = monoEngine.pluginRuntimeStatuses();
    runRealtimeStress.store(false, std::memory_order_release);
    realtimeStressThread.join();
    expect(!monoStatuses.empty()
               && monoStatuses.front().latencySamples == 32
               && monoStatuses.front().parameters.size() == 301
               && monoStatuses.front().parameters.front().name
                      == "Gain rescanned",
           (           "Dynamic CLAP latency and metadata propagate into engine status and PDC timing without cloning mutable realtime buffers"
            " (latency "
            + juce::String(
                monoStatuses.empty()
                    ? -1
                    : monoStatuses.front().latencySamples)
            + ", parameters "
            + juce::String(
                monoStatuses.empty()
                    ? 0
                    : static_cast<int>(
                          monoStatuses.front().parameters.size()))
            + ", name "
            + (monoStatuses.empty()
                   || monoStatuses.front().parameters.empty()
                   ? juce::String("none")
                   : monoStatuses.front().parameters.front().name)
            + ").")
               .toRawUTF8());
    monoSource.deleteFile();

    auto project = studio::Project::createDefault();
    studio::PluginInsert insert;
    insert.pluginIdentifier = "ara-mode-test";
    insert.name = "ARA Mode Test";
    insert.format = "VST3";
    insert.araCapable = true;
    project.tracks.front().inserts.push_back(insert);
    studio::CommandStack history;
    error.clear();
    expect(history.perform(
               std::make_unique<studio::SetPluginBridgeModeCommand>(
                   project.tracks.front().id,
                   insert.id,
                   studio::PluginBridgeMode::araCompatibility),
               project,
               error),
           error.toRawUTF8());
    expect(project.tracks.front().inserts.front().bridgeMode
               == studio::PluginBridgeMode::araCompatibility,
           "ARA compatibility mode is a typed project edit.");
    expect(history.undo(project)
               && project.tracks.front().inserts.front().bridgeMode
                      == studio::PluginBridgeMode::sandboxed,
           "Plugin isolation mode changes are undoable.");

    const auto araSourceFile = juce::File::getSpecialLocation(
                                   juce::File::tempDirectory)
                                   .getNonexistentChildFile(
                                       "StudioDuoAraSource",
                                       ".wav",
                                       false);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            araSourceFile.createOutputStream();
        auto writer = wav.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions {}
                .withSampleRate(48000.0)
                .withNumChannels(1)
                .withBitsPerSample(24));
        juce::AudioBuffer<float> source(1, 480);
        source.clear();
        expect(writer != nullptr
                   && writer->writeFromAudioSampleBuffer(
                       source,
                       0,
                       source.getNumSamples()),
               "ARA descriptor test source can be written.");
    }
    project.tempoChanges = {
        { 0.0, 120.0, false },
        { 2.0, 90.0, false }
    };
    project.meterChanges = {
        { 0.0, 4, 4 },
        { 2.0, 3, 4 }
    };
    studio::AudioClip araClip;
    araClip.name = "ARA clip";
    araClip.sourceFile = araSourceFile;
    araClip.startSeconds = 1.0;
    araClip.sourceOffsetSeconds = 0.002;
    araClip.durationSeconds = 0.005;
    project.tracks.front().clips.push_back(araClip);
    const auto descriptor = studio::AraDocumentHost::describeProject(
        project,
        project.tracks.front().id);
    expect(descriptor != nullptr
               && descriptor->name == project.name
               && descriptor->audioRegions.size() == 1
               && descriptor->audioRegions.front().regionId == araClip.id
               && descriptor->tempoEntries.size() >= 2
               && descriptor->meterEntries.size() == 2
               && descriptor->revision.isNotEmpty(),
           "ARA project descriptors include sources, regions, tempo, and meter.");

    const juce::MemoryBlock processorState("processor", 9);
    const juce::MemoryBlock araState("ara-document", 12);
    const auto packedState = studio::AraDocumentHost::packState(
        processorState,
        araState);
    juce::MemoryBlock unpackedProcessor;
    juce::MemoryBlock unpackedAra;
    expect(studio::AraDocumentHost::unpackState(
               packedState,
               unpackedProcessor,
               unpackedAra)
               && unpackedProcessor == processorState
               && unpackedAra == araState,
           "ARA and processor state share one content-addressed state blob.");
    araSourceFile.deleteFile();

    studio::StudioAudioEngine engine;
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = project.tracks.front().id;
    request.insertId = insert.id;
    request.name = insert.name;
    request.description = *descriptions[0];
    request.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    expect(engine.updateProject(project, { request }).wasOk(),
           "Trusted plugin runtime request is accepted.");
    for (int attempt = 0;
         attempt < 100 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    const auto statuses = engine.pluginRuntimeStatuses();
    expect(!statuses.empty()
               && statuses.front().state
                      == studio::StudioAudioEngine::PluginRuntimeStatus::State::ready
               && statuses.front().message.containsIgnoreCase("in-process")
               && statuses.front().parameters.size() == 300
               && statuses.front().parameters.front().name == "Gain",
           ("Trusted CLAP processing and parameter metadata activate in process"
            " ("
            + (statuses.empty()
                   ? juce::String("no status")
                   : statuses.front().message)
            + ").")
               .toRawUTF8());
    error.clear();
    expect(engine.setPluginParameter(insert.id, 0, 0.25f, error),
           error.toRawUTF8());
    const auto changedStatuses = engine.pluginRuntimeStatuses();
    expect(!changedStatuses.empty()
               && !changedStatuses.front().parameters.empty()
               && std::abs(
                      changedStatuses.front().parameters.front().value
                      - 0.25f)
                      < 0.0001f,
           "Hosted parameters can be edited through the runtime.");
    const auto captures = engine.capturePluginStates(1000);
    expect(captures.size() == 1
               && captures.front().result.wasOk()
               && !captures.front().state.isEmpty(),
           "Active plugin state can be captured for project persistence.");
}
