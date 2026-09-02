#include "TestHarness.h"
#include "TestSuites.h"

#include "mix/SoloResolver.h"
#include "mix/RoutingGraphCompiler.h"
#include "audio/StudioAudioEngine.h"
#include "model/ProjectModel.h"

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
    sourceFile.deleteFile();
    clipRenderFile.deleteFile();
}
