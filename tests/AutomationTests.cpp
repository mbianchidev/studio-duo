#include "TestHarness.h"
#include "TestSuites.h"

#include "automation/AutomationScheduler.h"
#include "automation/AutomationRecorder.h"
#include "model/ProjectCommands.h"
#include "audio/StudioAudioEngine.h"

void automationTests()
{
    auto project = studio::Project::createDefault();
    project.tempoChanges = {
        { 0.0, 120.0, true },
        { 4.0, 240.0, false }
    };

    studio::AutomationLane lane;
    lane.name = "Volume";
    lane.target.type = studio::AutomationTargetType::trackVolume;
    lane.target.trackId = project.tracks.front().id;
    lane.timebase = studio::AutomationTimebase::beats;
    lane.interpolation = studio::AutomationInterpolation::linear;
    lane.points = {
        { juce::Uuid().toString(), 0.0, 0.0 },
        { juce::Uuid().toString(), 6.0, 1.0 }
    };

    juce::String error;
    const auto compiled = studio::AutomationScheduler::compile(
        project,
        lane,
        48000.0,
        error);
    expect(compiled.has_value(), error.toRawUTF8());
    const auto endpoint = static_cast<std::int64_t>(
        std::llround(project.secondsAtBeat(6.0) * 48000.0));
    expect(compiled.has_value()
               && compiled->points.size() == 2
               && compiled->points.back().sample == endpoint,
           "Beat automation compiles through tempo ramps to exact samples.");
    expect(compiled.has_value()
               && std::abs(compiled->valueAt(endpoint / 2) - 0.5) < 0.0001,
           "Linear automation evaluates deterministically between points.");
    const auto events = compiled.has_value()
        ? studio::AutomationScheduler::eventsForBlock(
              *compiled,
              4,
              4)
        : std::vector<studio::CompiledAutomationEvent> {};
    expect(events.size() == 1
               && events.front().sampleOffset == 0
               && std::abs(events.front().value
                           - compiled->valueAt(4))
                      < 0.0001
               && events.front().ramp
               && events.front().rampEndOffset == 4
               && std::abs(events.front().rampEndValue
                           - compiled->valueAt(8))
                      < 0.0001,
           "Linear plugin automation emits one exact ramp segment per span.");

    project.automationLanes.push_back(lane);
    const auto decoded = studio::Project::fromVar(project.toVar(), error);
    expect(decoded.has_value()
               && decoded->automationLanes.size() == 1
               && decoded->automationLanes.front().target.trackId
                      == project.tracks.front().id,
           "Automation lanes and points persist in the project.");

    studio::CommandStack history;
    auto changed = lane;
    changed.points.back().value = 0.25;
    expect(history.perform(
               std::make_unique<studio::SetAutomationLaneCommand>(
                   lane,
                   changed),
               project,
               error),
           error.toRawUTF8());
    expect(std::abs(project.automationLanes.front().points.back().value
                    - 0.25)
               < 0.0001,
           "Automation lane edits are typed and undoable.");
    expect(history.undo(project)
               && std::abs(
                      project.automationLanes.front().points.back().value
                      - 1.0)
                      < 0.0001,
           "Undo restores automation points.");

    studio::Track aux;
    aux.name = "Automation Aux";
    aux.type = studio::TrackType::aux;
    const auto auxId = aux.id;
    expect(history.perform(std::make_unique<studio::AddTrackCommand>(aux),
                           project,
                           error),
           error.toRawUTF8());
    studio::RoutingConnection send;
    send.name = "Automated send";
    send.kind = studio::RouteKind::send;
    send.sourceTrackId = project.tracks.front().id;
    send.destination.type = studio::RouteEndpointType::track;
    send.destination.trackId = auxId;
    expect(history.perform(
               std::make_unique<studio::AddRoutingConnectionCommand>(send),
               project,
               error),
           error.toRawUTF8());
    studio::AutomationLane sendLane;
    sendLane.name = "Send level";
    sendLane.target.type = studio::AutomationTargetType::sendGain;
    sendLane.target.trackId = project.tracks.front().id;
    sendLane.target.routeId = send.id;
    sendLane.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.5
    });
    expect(history.perform(
               std::make_unique<studio::AddAutomationLaneCommand>(sendLane),
               project,
               error),
           error.toRawUTF8());
    expect(history.perform(
               std::make_unique<studio::RemoveRoutingConnectionCommand>(
                   send.id),
               project,
               error)
               && std::none_of(
                   project.automationLanes.cbegin(),
                   project.automationLanes.cend(),
                   [&sendLane](const auto& candidate)
                   {
                       return candidate.id == sendLane.id;
                   }),
           "Removing a route removes its automation target.");
    expect(history.undo(project)
               && std::any_of(
                   project.automationLanes.cbegin(),
                   project.automationLanes.cend(),
                   [&sendLane](const auto& candidate)
                   {
                       return candidate.id == sendLane.id;
                   }),
           "Undo restores route automation.");

    studio::AutomationLane recordingLane;
    recordingLane.name = "Recording";
    recordingLane.target = lane.target;
    recordingLane.points = {
        { juce::Uuid().toString(), 0.0, 0.0 },
        { juce::Uuid().toString(), 1.0, 1.0 }
    };
    const auto read = studio::AutomationRecorder::applyGesture(
        recordingLane,
        studio::AutomationMode::read,
        { 0.25, 0.5, 0.2, 0.8 });
    expect(read.points.size() == recordingLane.points.size(),
           "Read mode does not write automation.");

    const auto touch = studio::AutomationRecorder::applyGesture(
        recordingLane,
        studio::AutomationMode::touch,
        { 0.25, 0.5, 0.2, 0.8 });
    expect(touch.points.size() >= 4
               && touch.points.back().position > 0.5
               && std::abs(touch.points.back().value - 1.0) < 0.0001,
           "Touch mode returns to the existing lane after release.");
    const auto zeroLengthTouch =
        studio::AutomationRecorder::applyGesture(
            recordingLane,
            studio::AutomationMode::touch,
            { 0.25, 0.25, 0.8, 0.8 });
    const auto compiledTouch = studio::AutomationScheduler::compile(
        project,
        zeroLengthTouch,
        48000.0,
        error);
    const auto touchSample = static_cast<std::int64_t>(
        std::llround(0.25 * 48000.0));
    expect(compiledTouch.has_value()
               && std::abs(compiledTouch->valueAt(touchSample) - 0.8)
                      < 0.0001
               && compiledTouch->valueAt(touchSample + 1) < 0.5,
           "Zero-length touch gestures preserve the touched value for at least one sample.");

    const auto latch = studio::AutomationRecorder::applyGesture(
        recordingLane,
        studio::AutomationMode::latch,
        { 0.25, 0.75, 0.2, 0.8 });
    expect(std::abs(latch.points.back().position - 1.0) < 0.0001
               && std::abs(latch.points[latch.points.size() - 2].value - 0.8)
                      < 0.0001,
           "Latch mode holds the touched value until the pass ends.");

    const auto trim = studio::AutomationRecorder::applyGesture(
        recordingLane,
        studio::AutomationMode::trim,
        { 0.25, 0.5, 0.0, 0.1 });
    expect(std::abs(trim.trimOffset - 0.1) < 0.0001,
           "Trim mode records an offset over the base lane.");

    const auto preview = studio::AutomationRecorder::applyGesture(
        recordingLane,
        studio::AutomationMode::preview,
        { 0.25, 0.5, 0.2, 0.8 });
    expect(preview.points.size() == recordingLane.points.size()
               && std::abs(preview.trimOffset - recordingLane.trimOffset)
                      < 0.0001,
           "Preview mode remains non-destructive until committed.");

    const auto sourceFile = juce::File::getSpecialLocation(
                                juce::File::tempDirectory)
                                .getNonexistentChildFile(
                                    "StudioDuoAutomation",
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
        juce::AudioBuffer<float> source(1, 4096);
        for (int sample = 0; sample < source.getNumSamples(); ++sample)
            source.setSample(0, sample, 0.2f);
        expect(writer != nullptr
                   && writer->writeFromAudioSampleBuffer(
                       source,
                       0,
                       source.getNumSamples()),
               "Automation test source can be written.");
        if (writer != nullptr)
            writer->flush();
    }

    auto renderProject = studio::Project::createDefault();
    studio::AudioClip clip;
    clip.sourceFile = sourceFile;
    clip.durationSeconds = 0.01;
    clip.sourceLengthSeconds = 0.01;
    clip.sourceRangeEndSeconds = 0.01;
    renderProject.tracks.front().clips.push_back(clip);
    studio::AutomationLane fader;
    fader.name = "Sample fader";
    fader.target.type = studio::AutomationTargetType::trackVolume;
    fader.target.trackId = renderProject.tracks.front().id;
    fader.interpolation = studio::AutomationInterpolation::step;
    fader.points = {
        { juce::Uuid().toString(), 0.0, 5.0 / 6.0 },
        { juce::Uuid().toString(), 100.0 / 48000.0, 0.0 }
    };
    renderProject.automationLanes.push_back(fader);

    studio::StudioAudioEngine engine;
    juce::AudioBuffer<float> rendered;
    expect(engine.renderToBuffer(renderProject, rendered, 48000.0).wasOk(),
           "Automated mixer controls render.");
    expect(rendered.getSample(0, 99) > 0.19f
               && std::abs(rendered.getSample(0, 100)) < 0.001f,
           "Track automation changes on the exact scheduled sample.");

    auto muteProject = studio::Project::createDefault();
    const auto muteSourceId = muteProject.tracks.front().id;
    muteProject.tracks.front().clips.push_back(clip);
    studio::Track muteAux;
    muteAux.name = "Mute aux";
    muteAux.type = studio::TrackType::aux;
    const auto muteAuxId = muteAux.id;
    muteProject.tracks.insert(
        muteProject.tracks.end() - 1,
        muteAux);
    studio::RoutingConnection muteSend;
    muteSend.kind = studio::RouteKind::send;
    muteSend.tap = studio::RouteTap::postFader;
    muteSend.sourceTrackId = muteSourceId;
    muteSend.destination.type =
        studio::RouteEndpointType::track;
    muteSend.destination.trackId = muteAuxId;
    muteProject.routingConnections.push_back(muteSend);
    studio::AutomationLane muteLane;
    muteLane.target.type =
        studio::AutomationTargetType::trackMute;
    muteLane.target.trackId = muteSourceId;
    muteLane.interpolation =
        studio::AutomationInterpolation::step;
    muteLane.points = {
        { juce::Uuid().toString(), 0.0, 1.0 }
    };
    muteProject.automationLanes.push_back(muteLane);

    juce::AudioBuffer<float> mutedPostFader;
    expect(engine.renderToBuffer(
               muteProject,
               mutedPostFader,
               48000.0)
               .wasOk(),
           "Automated mute post-fader routing renders.");
    expect(mutedPostFader.getMagnitude(
               0,
               0,
               mutedPostFader.getNumSamples())
               < 0.001f,
           "Automated track mute silences post-fader sends offline.");

    muteProject.routingConnections.back().tap =
        studio::RouteTap::preFader;
    muteProject.automationLanes.front().enabled = false;
    juce::AudioBuffer<float> unmutedPreFader;
    expect(engine.renderToBuffer(
               muteProject,
               unmutedPreFader,
               48000.0)
               .wasOk(),
           "Unmuted pre-fader routing renders.");
    const auto unmutedPreFaderMagnitude =
        unmutedPreFader.getMagnitude(
            0,
            0,
            unmutedPreFader.getNumSamples());
    muteProject.automationLanes.front().enabled = true;
    juce::AudioBuffer<float> mutedPreFader;
    expect(engine.renderToBuffer(
               muteProject,
               mutedPreFader,
               48000.0)
               .wasOk(),
           "Automated mute pre-fader routing renders.");
    const auto preFaderMagnitude = mutedPreFader.getMagnitude(
        0,
        0,
        mutedPreFader.getNumSamples());
    expect(preFaderMagnitude > 0.1f,
           ("Automated track mute leaves pre-fader sends audible offline (magnitude "
            + juce::String(preFaderMagnitude, 4)
            + ", unmuted "
            + juce::String(unmutedPreFaderMagnitude, 4)
            + ").")
               .toRawUTF8());

    auto loopProject = studio::Project::createDefault();
    loopProject.tracks.front().clips.push_back(clip);
    loopProject.loopEnabled = true;
    loopProject.loopStartSeconds = 0.0;
    loopProject.loopEndSeconds = 100.0 / 48000.0;
    studio::PluginInsert loopInsert;
    loopInsert.pluginIdentifier = "studio.device.gain";
    loopInsert.name = "Loop gain";
    loopInsert.format = "Studio Duo";
    loopInsert.bundledDevice = true;
    loopInsert.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    loopProject.tracks.front().inserts.push_back(loopInsert);
    studio::AutomationLane loopMute;
    loopMute.target.type =
        studio::AutomationTargetType::trackMute;
    loopMute.target.trackId = loopProject.tracks.front().id;
    loopMute.interpolation =
        studio::AutomationInterpolation::step;
    loopMute.points = {
        { juce::Uuid().toString(), 0.0, 0.0 },
        { juce::Uuid().toString(), 50.0 / 48000.0, 1.0 }
    };
    loopProject.automationLanes.push_back(loopMute);
    studio::StudioAudioEngine::PluginRuntimeRequest loopRequest;
    loopRequest.trackId = loopProject.tracks.front().id;
    loopRequest.insertId = loopInsert.id;
    loopRequest.name = loopInsert.name;
    loopRequest.deviceIdentifier = loopInsert.pluginIdentifier;
    loopRequest.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    juce::AudioBuffer<float> looped;
    expect(engine.renderToBuffer(
               loopProject,
               looped,
               48000.0,
               { loopRequest })
               .wasOk(),
           "Looped plugin-inclusive automation renders.");
    expect(looped.getNumSamples() > 175
               && looped.getSample(0, 25) > 0.1f
               && std::abs(looped.getSample(0, 75)) < 0.001f
               && looped.getSample(0, 125) > 0.1f
               && std::abs(looped.getSample(0, 175)) < 0.001f,
           "Track automation wraps exactly with audio at mid-block loop boundaries.");

    auto stressProject = studio::Project::createDefault();
    auto stressClip = clip;
    stressClip.durationSeconds = 4096.0 / 48000.0;
    stressClip.sourceLengthSeconds = stressClip.durationSeconds;
    stressClip.sourceRangeEndSeconds = stressClip.durationSeconds;
    stressProject.tracks.front().clips.push_back(stressClip);
    studio::PluginInsert compressorInsert;
    compressorInsert.pluginIdentifier = "studio.device.compressor";
    compressorInsert.name = "Automation compressor";
    compressorInsert.format = "Studio Duo";
    compressorInsert.bundledDevice = true;
    compressorInsert.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    stressProject.tracks.front().inserts.push_back(compressorInsert);
    for (int parameter = 0; parameter < 6; ++parameter)
    {
        studio::AutomationLane parameterLane;
        parameterLane.target.type =
            studio::AutomationTargetType::deviceParameter;
        parameterLane.target.trackId =
            stressProject.tracks.front().id;
        parameterLane.target.insertId = compressorInsert.id;
        parameterLane.target.parameterIndex = parameter;
        parameterLane.interpolation =
            studio::AutomationInterpolation::linear;
        parameterLane.points = {
            { juce::Uuid().toString(), 0.0, 0.2 },
            { juce::Uuid().toString(),
              stressClip.durationSeconds,
              0.8 }
        };
        stressProject.automationLanes.push_back(
            std::move(parameterLane));
    }
    studio::StudioAudioEngine stressEngine;
    studio::StudioAudioEngine::PluginRuntimeRequest stressRequest;
    stressRequest.trackId = stressProject.tracks.front().id;
    stressRequest.insertId = compressorInsert.id;
    stressRequest.name = compressorInsert.name;
    stressRequest.deviceIdentifier =
        compressorInsert.pluginIdentifier;
    stressRequest.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    juce::AudioBuffer<float> stressed;
    expect(stressEngine.renderToBuffer(
               stressProject,
               stressed,
               48000.0,
               { stressRequest })
               .wasOk(),
           "Multi-lane linear plugin automation renders.");
    expect(stressEngine.pluginAutomationEventDropCount() == 0
               && std::isfinite(stressed.getSample(0, 2048)),
           "Compact plugin ramps preserve every automated lane without event overflow.");
    sourceFile.deleteFile();
}
