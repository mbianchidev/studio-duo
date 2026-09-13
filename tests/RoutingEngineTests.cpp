#include "TestHarness.h"
#include "TestSuites.h"

#include "mix/SoloResolver.h"
#include "mix/RoutingGraphCompiler.h"
#include "audio/StudioAudioEngine.h"
#include "model/ProjectModel.h"
#include "plugin_host/PluginFormats.h"

void routingEngineTests()
{
    auto project = studio::Project::createDefault();
    project.tracks.front().solo = true;
    const auto sourceId = project.tracks.front().id;

    studio::Track aux;
    aux.name = "Parallel";
    aux.type = studio::TrackType::aux;
    const auto auxId = aux.id;
    project.tracks.insert(project.tracks.end() - 1, aux);

    studio::Track safe;
    safe.name = "Talkback";
    safe.soloSafe = true;
    const auto safeId = safe.id;
    project.tracks.insert(project.tracks.end() - 1, safe);

    studio::RoutingConnection send;
    send.name = "Parallel send";
    send.kind = studio::RouteKind::send;
    send.sourceTrackId = sourceId;
    send.destination.type = studio::RouteEndpointType::track;
    send.destination.trackId = auxId;
    project.routingConnections.push_back(send);
    studio::RoutingConnection siblingSend = send;
    siblingSend.id = juce::Uuid().toString();
    siblingSend.name = "Sibling send";
    siblingSend.sourceTrackId = project.tracks[1].id;
    project.routingConnections.push_back(siblingSend);

    juce::String error;
    const auto solo = studio::SoloResolver::resolve(project, error);
    expect(solo.has_value(), error.toRawUTF8());
    expect(solo.has_value()
               && solo->isAudible(sourceId)
               && solo->isAudible(auxId)
               && solo->isAudible(safeId)
               && !solo->isAudible(project.tracks[1].id),
           "Solo keeps routed destinations and solo-safe tracks audible without unmuting bus siblings.");

    project.findTrack(sourceId)->solo = false;
    project.findTrack(auxId)->solo = true;
    const auto busSolo = studio::SoloResolver::resolve(project, error);
    expect(busSolo.has_value()
               && busSolo->isAudible(auxId)
               && busSolo->isAudible(sourceId),
           "Soloing an aux auditions its upstream source paths.");

    auto compileProject = studio::Project::createDefault();
    const auto compileSourceId = compileProject.tracks.front().id;
    studio::Track compileAux;
    compileAux.name = "Compile Aux";
    compileAux.type = studio::TrackType::aux;
    const auto compileAuxId = compileAux.id;
    compileProject.tracks.insert(compileProject.tracks.end() - 1, compileAux);

    studio::Track compileVca;
    compileVca.name = "Compile VCA";
    compileVca.type = studio::TrackType::vca;
    compileVca.volumeDecibels = -6.0f;
    compileVca.controlledTrackIds = { compileSourceId };
    compileProject.tracks.insert(compileProject.tracks.end() - 1, compileVca);
    compileProject.findTrack(compileSourceId)->polarityInverted = true;

    studio::RoutingConnection preSend;
    preSend.name = "Pre";
    preSend.kind = studio::RouteKind::send;
    preSend.tap = studio::RouteTap::preFader;
    preSend.sourceTrackId = compileSourceId;
    preSend.destination.type = studio::RouteEndpointType::track;
    preSend.destination.trackId = compileAuxId;
    preSend.gainDecibels = -3.0f;
    compileProject.routingConnections.push_back(preSend);

    studio::RoutingConnection hardware;
    hardware.name = "Hardware 5-6";
    hardware.kind = studio::RouteKind::hardwareOutput;
    hardware.sourceTrackId = compileSourceId;
    hardware.destination.type = studio::RouteEndpointType::hardwareOutput;
    hardware.destination.firstChannel = 4;
    hardware.destination.channels = 2;
    compileProject.routingConnections.push_back(hardware);

    const auto compiled = studio::RoutingGraphCompiler::compile(
        compileProject,
        error);
    expect(compiled.has_value(), error.toRawUTF8());
    const auto* compiledSource = compiled.has_value()
        ? compiled->findTrack(compileSourceId)
        : nullptr;
    expect(compiledSource != nullptr
               && compiledSource->polarityInverted
               && std::abs(compiledSource->vcaGain
                           - juce::Decibels::decibelsToGain(-6.0f))
                      < 0.0001f,
           "Compiled tracks include polarity and VCA gain.");
    expect(compiled.has_value()
               && compiled->routes.size() >= 2
               && std::any_of(
                   compiled->routes.cbegin(),
                   compiled->routes.cend(),
                   [&preSend, &compileAuxId](const auto& route)
                   {
                       return route.id == preSend.id
                           && route.tap == studio::RouteTap::preFader
                           && route.destinationTrackId == compileAuxId;
                   })
               && std::any_of(
                   compiled->routes.cbegin(),
                   compiled->routes.cend(),
                   [&hardware](const auto& route)
                   {
                       return route.id == hardware.id
                           && route.hardwareFirstChannel == 4
                           && route.hardwareChannels == 2;
                   }),
           "Compiled routes retain fan-out taps and hardware channel maps.");

    const auto sourceFile = juce::File::getSpecialLocation(
                                juce::File::tempDirectory)
                                .getNonexistentChildFile(
                                    "StudioDuoRouting",
                                    ".wav",
                                    false);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            sourceFile.createOutputStream();
        auto writer = wav.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions {}
                .withSampleRate(48000.0)
                .withNumChannels(1)
                .withBitsPerSample(24));
        juce::AudioBuffer<float> sourceBuffer(1, 480);
        sourceBuffer.clear();
        sourceBuffer.applyGain(0.2f);
        for (int sample = 0; sample < sourceBuffer.getNumSamples(); ++sample)
            sourceBuffer.setSample(0, sample, 0.2f);
        expect(writer != nullptr
                   && writer->writeFromAudioSampleBuffer(
                       sourceBuffer,
                       0,
                       sourceBuffer.getNumSamples()),
               "Routing test source WAV can be written.");
        if (writer != nullptr)
            writer->flush();
    }
    {
        juce::WavAudioFormat wav;
        auto input = sourceFile.createInputStream();
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            wav.createReaderFor(input.release(), true));
        juce::AudioBuffer<float> check(1, 1);
        const auto read = reader != nullptr
            && reader->read(&check, 0, 1, 100, true, false);
        expect(read && check.getSample(0, 0) > 0.19f,
               "Routing test source WAV contains the expected signal.");
    }

    auto takeProject = studio::Project::createDefault();
    takeProject.metronomeEnabled = false;
    auto& takeParent = takeProject.tracks.front();
    const auto takeParentId = takeParent.id;
    takeParent.clips.clear();
    takeParent.versionsCollapsed = true;
    studio::Track firstTake;
    firstTake.name = "v1";
    firstTake.parentTrackId = takeParentId;
    firstTake.versionNumber = 1;
    studio::AudioClip firstTakeClip;
    firstTakeClip.sourceFile = sourceFile;
    firstTakeClip.durationSeconds = 0.01;
    firstTakeClip.sourceLengthSeconds = 0.01;
    firstTakeClip.sourceRangeEndSeconds = 0.01;
    firstTake.clips.push_back(firstTakeClip);
    studio::Track secondTake = firstTake;
    secondTake.id = juce::Uuid().toString();
    secondTake.name = "v2";
    secondTake.versionNumber = 2;
    secondTake.clips.front().id = juce::Uuid().toString();
    secondTake.clips.front().startSeconds = 0.02;
    studio::Track thirdTake = secondTake;
    thirdTake.id = juce::Uuid().toString();
    thirdTake.name = "v3";
    thirdTake.versionNumber = 3;
    thirdTake.clips.front().id = juce::Uuid().toString();
    thirdTake.clips.front().startSeconds = 0.04;
    takeParent.activeTakeTrackId = thirdTake.id;
    takeProject.tracks.insert(takeProject.tracks.begin() + 1, firstTake);
    takeProject.tracks.insert(takeProject.tracks.begin() + 2, secondTake);
    takeProject.tracks.insert(takeProject.tracks.begin() + 3, thirdTake);
    studio::StudioAudioEngine takeEngine;
    juce::AudioBuffer<float> collapsedTakes;
    expect(takeEngine.renderToBuffer(
               takeProject,
               collapsedTakes,
               48000.0)
               .wasOk(),
           "Collapsed takes render through their parent.");
    takeProject.findTrack(takeParentId)->versionsCollapsed = false;
    juce::AudioBuffer<float> expandedTakes;
    expect(takeEngine.renderToBuffer(
               takeProject,
               expandedTakes,
               48000.0)
               .wasOk(),
           "Expanded takes render as audible layers.");
    expect(collapsedTakes.getSample(0, 100) < 0.001f
               && collapsedTakes.getSample(0, 2020) > 0.19f,
           "Collapsed parents play only the active take at its timeline position.");
    expect(expandedTakes.getSample(0, 100) > 0.19f
               && expandedTakes.getSample(0, 1060) > 0.19f
               && expandedTakes.getSample(0, 2020) > 0.19f,
           "Expanded parents play the first and every subsequent unmuted take clip.");

    expect(takeEngine.updateProject(takeProject).wasOk(),
           "Expanded take projects publish for real-time playback.");
    takeEngine.seekSeconds(0.0);
    takeEngine.play();
    const auto liveTakes = takeEngine.renderActiveBlockForTesting(2600);
    expect(liveTakes.getSample(0, 100) > 0.19f,
           "Real-time playback includes the first expanded take clip.");
    expect(liveTakes.getSample(0, 1060) > 0.19f,
           "Real-time playback includes the second expanded take clip.");
    expect(liveTakes.getSample(0, 2020) > 0.19f,
           "Real-time playback includes the third expanded take clip.");

    auto renderProject = studio::Project::createDefault();
    auto* renderSource = renderProject.findTrack(
        renderProject.tracks.front().id);
    const auto renderSourceId = renderSource->id;
    renderSource->volumeDecibels = -6.0f;
    studio::AudioClip renderClip;
    renderClip.sourceFile = sourceFile;
    renderClip.durationSeconds = 0.01;
    renderClip.sourceLengthSeconds = 0.01;
    renderClip.sourceRangeEndSeconds = 0.01;
    renderSource->clips.push_back(renderClip);

    studio::Track renderAux;
    renderAux.name = "Render Aux";
    renderAux.type = studio::TrackType::aux;
    const auto renderAuxId = renderAux.id;
    renderProject.tracks.insert(renderProject.tracks.end() - 1, renderAux);
    expect(renderProject.findTrack(renderSourceId) != nullptr
               && renderProject.findTrack(renderSourceId)->clips.size() == 1,
           "Routing render source survives track insertion.");

    studio::RoutingConnection renderSend;
    renderSend.name = "Render send";
    renderSend.kind = studio::RouteKind::send;
    renderSend.tap = studio::RouteTap::preFader;
    renderSend.sourceTrackId = renderSourceId;
    renderSend.destination.type = studio::RouteEndpointType::track;
    renderSend.destination.trackId = renderAuxId;
    renderProject.routingConnections.push_back(renderSend);
    const auto renderGraph = studio::RoutingGraphCompiler::compile(
        renderProject,
        error);
    const auto* renderCompiledSource = renderGraph.has_value()
        ? renderGraph->findTrack(renderSourceId)
        : nullptr;
    expect(renderCompiledSource != nullptr
               && renderCompiledSource->audible
               && renderCompiledSource->processing,
           "Routing render source compiles as audible and processing.");

    studio::StudioAudioEngine engine;
    expect(engine.updateProject(renderProject).wasOk(),
           "Routed projects publish a real-time processing snapshot.");
    for (int attempt = 0;
         attempt < 100 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    const auto routeBufferCapacity =
        engine.minimumRouteBufferCapacityForTesting();
    expect(routeBufferCapacity
               >= studio::PluginBridgeSharedState::maxBlockSize,
           ("Every routed callback buffer supports the maximum callback chunk"
            " (capacity "
            + juce::String(routeBufferCapacity)
            + ").")
               .toRawUTF8());
    const auto clipRenderFile = sourceFile.getSiblingFile(
        sourceFile.getFileNameWithoutExtension() + "-clip.wav");
    expect(engine.renderClipToWav(renderClip, clipRenderFile, 48000.0).wasOk(),
           "Routing test clip renders independently.");
    {
        juce::WavAudioFormat wav;
        auto input = clipRenderFile.createInputStream();
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            wav.createReaderFor(input.release(), true));
        juce::AudioBuffer<float> check(1, 1);
        const auto read = reader != nullptr
            && reader->read(&check, 0, 1, 100, true, false);
        expect(read && check.getSample(0, 0) > 0.19f,
               "Routing test clip processing retains source audio.");
    }
    juce::AudioBuffer<float> preFaderOutput;
    error.clear();
    expect(engine.renderToBuffer(
               renderProject,
               preFaderOutput,
               48000.0)
               .wasOk(),
           "Pre-fader routing can render to a deterministic buffer.");

    renderProject.routingConnections.back().tap =
        studio::RouteTap::postFader;
    juce::AudioBuffer<float> postFaderOutput;
    expect(engine.renderToBuffer(
               renderProject,
               postFaderOutput,
               48000.0)
               .wasOk(),
           "Post-fader routing can render to a deterministic buffer.");
    const auto preSample = preFaderOutput.getNumSamples() > 100
        ? preFaderOutput.getSample(0, 100)
        : 0.0f;
    const auto postSample = postFaderOutput.getNumSamples() > 100
        ? postFaderOutput.getSample(0, 100)
        : 0.0f;
    expect(preFaderOutput.getNumSamples() > 100
               && postFaderOutput.getNumSamples() > 100
               && preSample > postSample + 0.08f,
           "Pre-fader sends are independent from the source fader.");

    juce::AudioPluginFormatManager fixtureFormats;
    studio::PluginFormats::addSupportedFormats(fixtureFormats);
    juce::OwnedArray<juce::PluginDescription> fixtureDescriptions;
    for (auto* format : fixtureFormats.getFormats())
    {
        if (format->getName() != "VST3")
            continue;
        format->findAllTypesForFile(
            fixtureDescriptions,
            STUDIO_DUO_ROUTING_FIXTURE_PATH);
        break;
    }
    expect(fixtureDescriptions.size() == 1,
           "The routing fixture can be discovered.");

    if (!fixtureDescriptions.isEmpty())
    {
        auto sidechainProject = studio::Project::createDefault();
        sidechainProject.metronomeEnabled = false;
        sidechainProject.tracks[0].volumeDecibels = -60.0f;
        sidechainProject.tracks[1].volumeDecibels = -60.0f;

        auto firstClip = renderClip;
        sidechainProject.tracks[0].clips.push_back(firstClip);
        auto secondClip = renderClip;
        secondClip.id = juce::Uuid().toString();
        secondClip.gainDecibels = -6.0206f;
        sidechainProject.tracks[1].clips.push_back(secondClip);

        studio::Track sidechainDestination;
        sidechainDestination.name = "Sidechain destination";
        sidechainDestination.type = studio::TrackType::aux;
        studio::PluginInsert firstProbe;
        firstProbe.pluginIdentifier =
            fixtureDescriptions[0]->createIdentifierString();
        firstProbe.name = "First sidechain probe";
        firstProbe.format = "VST3";
        firstProbe.bridgeMode =
            studio::PluginBridgeMode::trustedInProcess;
        studio::PluginInsert secondProbe = firstProbe;
        secondProbe.id = juce::Uuid().toString();
        secondProbe.name = "Second sidechain probe";
        sidechainDestination.inserts = { firstProbe, secondProbe };
        const auto destinationId = sidechainDestination.id;
        sidechainProject.tracks.insert(
            sidechainProject.tracks.end() - 1,
            sidechainDestination);

        studio::RoutingConnection firstSidechain;
        firstSidechain.name = "First isolated sidechain";
        firstSidechain.kind = studio::RouteKind::sidechain;
        firstSidechain.tap = studio::RouteTap::preFader;
        firstSidechain.sourceTrackId =
            sidechainProject.tracks[0].id;
        firstSidechain.destination.type =
            studio::RouteEndpointType::pluginSidechain;
        firstSidechain.destination.trackId = destinationId;
        firstSidechain.destination.insertId = firstProbe.id;
        sidechainProject.routingConnections.push_back(
            firstSidechain);

        auto secondSidechain = firstSidechain;
        secondSidechain.id = juce::Uuid().toString();
        secondSidechain.name = "Second isolated sidechain";
        secondSidechain.sourceTrackId =
            sidechainProject.tracks[1].id;
        secondSidechain.destination.insertId = secondProbe.id;
        sidechainProject.routingConnections.push_back(
            secondSidechain);

        const auto makeRequest =
            [&fixtureDescriptions, destinationId](
                const studio::PluginInsert& insert)
        {
            studio::StudioAudioEngine::PluginRuntimeRequest request;
            request.trackId = destinationId;
            request.insertId = insert.id;
            request.name = insert.name;
            request.description = *fixtureDescriptions[0];
            request.sidechainChannels = 2;
            request.bridgeMode =
                studio::PluginBridgeMode::trustedInProcess;
            return request;
        };

        juce::AudioBuffer<float> isolatedSidechains;
        expect(engine.renderToBuffer(
                   sidechainProject,
                   isolatedSidechains,
                   48000.0,
                   {
                       makeRequest(firstProbe),
                       makeRequest(secondProbe)
                   })
                   .wasOk(),
               "Per-insert sidechains render.");
        const auto isolatedSample =
            isolatedSidechains.getNumSamples() > 100
            ? isolatedSidechains.getSample(0, 100)
            : 0.0f;
        expect(isolatedSample > 0.29f
                   && isolatedSample < 0.32f,
               ("Sidechain routes feed only their selected inserts (sample "
                + juce::String(isolatedSample, 4)
                + ").")
                   .toRawUTF8());

        const auto impulseFile = sourceFile.getSiblingFile(
            sourceFile.getFileNameWithoutExtension()
                + "-sidechain-impulse.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream =
                impulseFile.createOutputStream();
            auto writer = wav.createWriterFor(
                stream,
                juce::AudioFormatWriterOptions {}
                    .withSampleRate(48000.0)
                    .withNumChannels(1)
                    .withBitsPerSample(24));
            juce::AudioBuffer<float> impulse(1, 64);
            impulse.clear();
            impulse.setSample(0, 0, 0.2f);
            expect(writer != nullptr
                       && writer->writeFromAudioSampleBuffer(
                           impulse,
                           0,
                           impulse.getNumSamples()),
                   "Sidechain latency impulse can be written.");
            if (writer != nullptr)
                writer->flush();
        }

        auto latencyProject = studio::Project::createDefault();
        latencyProject.metronomeEnabled = false;
        studio::AudioClip impulseClip;
        impulseClip.sourceFile = impulseFile;
        impulseClip.durationSeconds = 64.0 / 48000.0;
        impulseClip.sourceLengthSeconds =
            impulseClip.durationSeconds;
        impulseClip.sourceRangeEndSeconds =
            impulseClip.durationSeconds;
        latencyProject.tracks[0].clips.push_back(impulseClip);
        impulseClip.id = juce::Uuid().toString();
        latencyProject.tracks[1].clips.push_back(impulseClip);
        latencyProject.tracks[1].volumeDecibels = -60.0f;

        studio::PluginInsert limiter;
        limiter.pluginIdentifier = "studio.device.limiter";
        limiter.name = "Latency before sidechain";
        limiter.format = "Studio Duo";
        limiter.bundledDevice = true;
        limiter.bridgeMode =
            studio::PluginBridgeMode::trustedInProcess;
        limiter.latencySamples = 16;
        auto latencyProbe = firstProbe;
        latencyProbe.id = juce::Uuid().toString();
        latencyProbe.name = "Latency-aligned sidechain";
        latencyProject.tracks[0].inserts = {
            limiter,
            latencyProbe
        };

        studio::RoutingConnection latencySidechain;
        latencySidechain.name = "Latency sidechain";
        latencySidechain.kind = studio::RouteKind::sidechain;
        latencySidechain.tap = studio::RouteTap::preFader;
        latencySidechain.sourceTrackId =
            latencyProject.tracks[1].id;
        latencySidechain.destination.type =
            studio::RouteEndpointType::pluginSidechain;
        latencySidechain.destination.trackId =
            latencyProject.tracks[0].id;
        latencySidechain.destination.insertId =
            latencyProbe.id;
        latencyProject.routingConnections.push_back(
            latencySidechain);

        studio::StudioAudioEngine::PluginRuntimeRequest limiterRequest;
        limiterRequest.trackId = latencyProject.tracks[0].id;
        limiterRequest.insertId = limiter.id;
        limiterRequest.name = limiter.name;
        limiterRequest.deviceIdentifier = limiter.pluginIdentifier;
        limiterRequest.latencySamples = limiter.latencySamples;
        limiterRequest.bridgeMode =
            studio::PluginBridgeMode::trustedInProcess;
        auto latencyProbeRequest =
            makeRequest(latencyProbe);
        latencyProbeRequest.trackId =
            latencyProject.tracks[0].id;

        juce::AudioBuffer<float> alignedSidechain;
        expect(engine.renderToBuffer(
                   latencyProject,
                   alignedSidechain,
                   48000.0,
                   {
                       limiterRequest,
                       latencyProbeRequest
                   })
                   .wasOk(),
               "Insert-position sidechain compensation renders.");
        const auto sidechainAtStart =
            alignedSidechain.getSample(0, 0);
        const auto sidechainAtLatency =
            alignedSidechain.getSample(0, 16);
        expect(std::abs(sidechainAtStart) < 0.001f
                   && sidechainAtLatency > 0.19f,
               ("Sidechains align to the target insert after preceding plugin latency (start "
                + juce::String(sidechainAtStart, 5)
                + ", latency "
                + juce::String(sidechainAtLatency, 5)
                + ").")
                   .toRawUTF8());
        impulseFile.deleteFile();

        auto monitoringProject = studio::Project::createDefault();
        monitoringProject.metronomeEnabled = false;
        monitoringProject.tracks[0].inputMonitoring = true;
        monitoringProject.tracks[0].inputChannel = 0;
        monitoringProject.tracks[0].volumeDecibels = -6.0206f;
        auto monitoringLimiter = limiter;
        monitoringLimiter.id = juce::Uuid().toString();
        monitoringLimiter.name = "Monitoring latency";
        monitoringProject.tracks[0].inserts.push_back(
            monitoringLimiter);

        studio::Track monitoringControlRoom;
        monitoringControlRoom.name = "Monitoring control room";
        monitoringControlRoom.type =
            studio::TrackType::controlRoom;
        monitoringControlRoom.hardwareOutputChannel = 2;
        const auto monitoringControlRoomId =
            monitoringControlRoom.id;
        monitoringProject.tracks.insert(
            monitoringProject.tracks.end() - 1,
            monitoringControlRoom);
        studio::RoutingConnection monitoringRoute;
        monitoringRoute.name = "Monitoring control-room route";
        monitoringRoute.kind = studio::RouteKind::controlRoom;
        monitoringRoute.sourceTrackId =
            monitoringProject.masterTrackId();
        monitoringRoute.destination.type =
            studio::RouteEndpointType::track;
        monitoringRoute.destination.trackId =
            monitoringControlRoomId;
        monitoringProject.routingConnections.push_back(
            monitoringRoute);

        studio::StudioAudioEngine monitoringEngine;
        studio::StudioAudioEngine::PluginRuntimeRequest
            monitoringRequest;
        monitoringRequest.trackId =
            monitoringProject.tracks[0].id;
        monitoringRequest.insertId = monitoringLimiter.id;
        monitoringRequest.name = monitoringLimiter.name;
        monitoringRequest.deviceIdentifier =
            monitoringLimiter.pluginIdentifier;
        monitoringRequest.latencySamples =
            monitoringLimiter.latencySamples;
        monitoringRequest.bridgeMode =
            studio::PluginBridgeMode::trustedInProcess;
        expect(monitoringEngine.updateProject(
                   monitoringProject,
                   { monitoringRequest })
                   .wasOk(),
               "A monitored project publishes its processing graph.");
        for (int attempt = 0;
             attempt < 100
             && monitoringEngine.pluginRuntimeTransitionPending();
             ++attempt)
            juce::Thread::sleep(10);

        juce::AudioBuffer<float> monitoredInput(1, 64);
        monitoredInput.clear();
        monitoredInput.setSample(0, 0, 0.2f);
        const auto monitoredOutput =
            monitoringEngine.renderActiveBlockWithInputForTesting(
                monitoredInput,
                4);
        expect(std::abs(monitoredOutput.getSample(0, 0))
                       < 0.001f
                   && monitoredOutput.getSample(0, 16) > 0.05f
                   && std::abs(
                          monitoredOutput.getSample(2, 16)
                          - monitoredOutput.getSample(0, 16))
                       < 0.001f,
               "Software monitoring follows track processing, PDC, and control-room routing while stopped.");

        auto midiRuntimeProject = studio::Project::createDefault();
        midiRuntimeProject.metronomeEnabled = false;
        midiRuntimeProject.tracks[0].clips.push_back(renderClip);
        midiRuntimeProject.tracks[0].volumeDecibels = -60.0f;

        studio::Track midiSource;
        midiSource.name = "Live MIDI source";
        midiSource.type = studio::TrackType::midi;
        midiSource.armed = true;
        const auto midiSourceId = midiSource.id;
        midiRuntimeProject.tracks.insert(
            midiRuntimeProject.tracks.end() - 1,
            midiSource);

        studio::Track instrumentDestination;
        instrumentDestination.name = "MIDI destination";
        instrumentDestination.type =
            studio::TrackType::instrument;
        auto instrumentProbe = firstProbe;
        instrumentProbe.id = juce::Uuid().toString();
        instrumentProbe.name = "MIDI instrument probe";
        instrumentDestination.inserts.push_back(
            instrumentProbe);
        const auto instrumentDestinationId =
            instrumentDestination.id;
        midiRuntimeProject.tracks.insert(
            midiRuntimeProject.tracks.end() - 1,
            instrumentDestination);

        studio::RoutingConnection midiRuntimeRoute;
        midiRuntimeRoute.name = "Live MIDI route";
        midiRuntimeRoute.signalType = studio::SignalType::midi;
        midiRuntimeRoute.kind = studio::RouteKind::mainOutput;
        midiRuntimeRoute.sourceTrackId = midiSourceId;
        midiRuntimeRoute.destination.type =
            studio::RouteEndpointType::track;
        midiRuntimeRoute.destination.trackId =
            instrumentDestinationId;
        midiRuntimeProject.routingConnections.push_back(
            midiRuntimeRoute);

        studio::StudioAudioEngine midiRuntimeEngine;
        studio::StudioAudioEngine::PluginRuntimeRequest
            instrumentRequest;
        instrumentRequest.trackId = instrumentDestinationId;
        instrumentRequest.insertId = instrumentProbe.id;
        instrumentRequest.name = instrumentProbe.name;
        instrumentRequest.description = *fixtureDescriptions[0];
        instrumentRequest.bridgeMode =
            studio::PluginBridgeMode::trustedInProcess;
        expect(midiRuntimeEngine.updateProject(
                   midiRuntimeProject,
                   { instrumentRequest })
                   .wasOk(),
               "A MIDI-routed instrument project publishes.");
        for (int attempt = 0;
             attempt < 500
             && midiRuntimeEngine.pluginRuntimeTransitionPending();
             ++attempt)
        {
            if (auto* messages =
                    juce::MessageManager::getInstanceWithoutCreating())
                messages->runDispatchLoopUntil(10);
            else
                juce::Thread::sleep(10);
        }
        const auto midiStatuses =
            midiRuntimeEngine.pluginRuntimeStatuses();
        expect(!midiStatuses.empty()
                   && midiStatuses.front().state
                       == studio::StudioAudioEngine::
                           PluginRuntimeStatus::State::ready,
               midiStatuses.empty()
                   ? "MIDI instrument runtime status is unavailable."
                   : midiStatuses.front().message.toRawUTF8());

        juce::MidiBuffer liveMidi;
        liveMidi.addEvent(
            juce::MidiMessage::noteOn(
                1,
                61,
                static_cast<juce::uint8>(96)),
            12);
        const auto midiRuntimeOutput =
            midiRuntimeEngine.renderActiveBlockWithMidiForTesting(
                liveMidi,
                64);
        const auto midiSample =
            midiRuntimeOutput.getSample(0, 12);
        expect(midiSample > 0.7f,
               ("Armed MIDI tracks route exact-sample events into instrument processing while stopped (sample "
                + juce::String(midiSample, 5)
                + ").")
                   .toRawUTF8());

        auto sandboxMidiProject = midiRuntimeProject;
        auto* sandboxInstrument =
            sandboxMidiProject.findTrack(
                instrumentDestinationId);
        sandboxInstrument->inserts.front().bridgeMode =
            studio::PluginBridgeMode::sandboxed;
        auto sandboxRequest = instrumentRequest;
        sandboxRequest.bridgeMode =
            studio::PluginBridgeMode::sandboxed;

        studio::StudioAudioEngine sandboxMidiEngine(
            juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH));
        expect(sandboxMidiEngine.updateProject(
                   sandboxMidiProject,
                   { sandboxRequest })
                   .wasOk(),
               "A sandboxed MIDI instrument project publishes.");
        for (int attempt = 0;
             attempt < 500
             && sandboxMidiEngine.pluginRuntimeTransitionPending();
             ++attempt)
            juce::Thread::sleep(10);
        const auto sandboxMidiStatuses =
            sandboxMidiEngine.pluginRuntimeStatuses();
        expect(!sandboxMidiStatuses.empty()
                   && sandboxMidiStatuses.front().state
                       == studio::StudioAudioEngine::
                           PluginRuntimeStatus::State::ready,
               sandboxMidiStatuses.empty()
                   ? "Sandbox MIDI runtime status is unavailable."
                   : sandboxMidiStatuses.front().message.toRawUTF8());
        auto sandboxMidiPeak = 0.0f;
        for (int block = 0; block < 12; ++block)
        {
            const auto output = block == 0
                ? sandboxMidiEngine
                      .renderActiveBlockWithMidiForTesting(
                          liveMidi,
                          64)
                : sandboxMidiEngine.renderActiveBlockForTesting(
                      64);
            sandboxMidiPeak = std::max(
                sandboxMidiPeak,
                output.getMagnitude(
                    0,
                    0,
                    output.getNumSamples()));
            juce::Thread::sleep(3);
        }
        expect(sandboxMidiPeak > 0.7f,
               ("Sandboxed instruments receive routed MIDI through the bridge (peak "
                + juce::String(sandboxMidiPeak, 5)
                + ").")
                   .toRawUTF8());

        juce::OwnedArray<juce::PluginDescription>
            clapDescriptions;
        for (auto* format : fixtureFormats.getFormats())
        {
            if (format->getName() != "CLAP")
                continue;
            format->findAllTypesForFile(
                clapDescriptions,
                STUDIO_DUO_CLAP_FIXTURE_PATH);
            break;
        }
        expect(clapDescriptions.size() == 1,
               "The CLAP MIDI fixture can be discovered.");
        if (!clapDescriptions.isEmpty())
        {
            auto midiEffectProject = midiRuntimeProject;
            auto* midiEffectTrack =
                midiEffectProject.findTrack(midiSourceId);
            studio::PluginInsert midiEffect;
            midiEffect.pluginIdentifier =
                clapDescriptions[0]->createIdentifierString();
            midiEffect.name = "CLAP MIDI transpose";
            midiEffect.format = "CLAP";
            midiEffect.bridgeMode =
                studio::PluginBridgeMode::sandboxed;
            midiEffectTrack->inserts.push_back(midiEffect);

            studio::StudioAudioEngine::PluginRuntimeRequest
                midiEffectRequest;
            midiEffectRequest.trackId = midiSourceId;
            midiEffectRequest.insertId = midiEffect.id;
            midiEffectRequest.name = midiEffect.name;
            midiEffectRequest.description = *clapDescriptions[0];
            midiEffectRequest.bridgeMode =
                studio::PluginBridgeMode::sandboxed;

            studio::StudioAudioEngine midiEffectEngine(
                juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH));
            expect(midiEffectEngine.updateProject(
                       midiEffectProject,
                       {
                           midiEffectRequest,
                           instrumentRequest
                       })
                       .wasOk(),
                   "A sandboxed MIDI-effect route publishes.");
            for (int attempt = 0;
                 attempt < 500
                 && midiEffectEngine.pluginRuntimeTransitionPending();
                 ++attempt)
            {
                if (auto* messages =
                        juce::MessageManager::getInstanceWithoutCreating())
                    messages->runDispatchLoopUntil(10);
                else
                    juce::Thread::sleep(10);
            }
            const auto midiEffectStatuses =
                midiEffectEngine.pluginRuntimeStatuses();
            expect(midiEffectStatuses.size() == 2
                       && std::all_of(
                           midiEffectStatuses.cbegin(),
                           midiEffectStatuses.cend(),
                           [](const auto& status)
                           {
                               return status.state
                                   == studio::StudioAudioEngine::
                                       PluginRuntimeStatus::State::ready;
                           }),
                   midiEffectStatuses.empty()
                       ? "MIDI-effect runtime statuses are unavailable."
                       : midiEffectStatuses.front().message.toRawUTF8());

            juce::MidiBuffer sourceMidi;
            sourceMidi.addEvent(
                juce::MidiMessage::noteOn(
                    1,
                    60,
                    static_cast<juce::uint8>(96)),
                12);
            auto midiEffectPeak = 0.0f;
            for (int block = 0; block < 24; ++block)
            {
                const auto output = block == 0
                    ? midiEffectEngine
                          .renderActiveBlockWithMidiForTesting(
                              sourceMidi,
                              64)
                    : midiEffectEngine
                          .renderActiveBlockForTesting(64);
                midiEffectPeak = std::max(
                    midiEffectPeak,
                    output.getMagnitude(
                        0,
                        0,
                        output.getNumSamples()));
                juce::Thread::sleep(3);
            }
            expect(midiEffectPeak > 0.7f,
                   ("Sandboxed CLAP MIDI output routes into downstream instruments (peak "
                    + juce::String(midiEffectPeak, 5)
                    + ", late "
                    + juce::String(
                        midiEffectEngine.pluginLateBlockCount())
                    + ").")
                       .toRawUTF8());
        }
    }

    sourceFile.deleteFile();
    clipRenderFile.deleteFile();
}
