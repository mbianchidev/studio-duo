#include "TestHarness.h"
#include "TestSuites.h"

#include "automation/AutomationTypes.h"
#include "render/RenderEngine.h"
#include "reamp/ReampSnapshotService.h"

void renderEngineTests()
{
    juce::AudioBuffer<float> reference(2, 128);
    juce::AudioBuffer<float> candidate(2, 128);
    reference.clear();
    candidate.clear();
    reference.applyGain(0.5f);
    candidate.applyGain(0.25f);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 128; ++sample)
        {
            reference.setSample(channel, sample, 0.5f);
            candidate.setSample(channel, sample, 0.25f);
        }
    expect(std::abs(studio::RenderEngine::levelMatchGainDecibels(
                        reference,
                        candidate)
                    - 6.0206)
               < 0.01,
           "Level-matched A/B uses deterministic gated RMS gain.");

    auto project = studio::Project::createDefault();
    studio::PluginInsert generator;
    generator.pluginIdentifier = "studio.device.generator";
    generator.name = "Signal Generator";
    generator.format = "Studio Duo";
    generator.bundledDevice = true;
    generator.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    project.tracks[1].inserts.push_back(generator);
    studio::ReampRoute route;
    route.type = studio::TonePathType::plugin;
    route.sourceTrackId = project.tracks.front().id;
    route.returnTrackId = project.tracks[1].id;
    project.reampRoutes.push_back(route);
    juce::String error;
    const auto snapshot = studio::ReampSnapshotService::capture(
        project,
        route.id,
        "Generated",
        error);
    expect(snapshot.has_value(), error.toRawUTF8());

    const auto outputDirectory = juce::File::getSpecialLocation(
                                     juce::File::tempDirectory)
                                     .getNonexistentChildFile(
                                         "StudioDuoRender",
                                         {},
                                         false);
    outputDirectory.createDirectory();
    studio::StudioAudioEngine engine;
    const auto reports = studio::RenderEngine::batchToneSnapshots(
        engine,
        project,
        {
            *snapshot,
            studio::ToneSnapshot {}
        },
        outputDirectory,
        [](const studio::Project& renderProject)
        {
            std::vector<studio::StudioAudioEngine::PluginRuntimeRequest>
                requests;
            for (const auto& track : renderProject.tracks)
                for (const auto& insert : track.inserts)
                {
                    studio::StudioAudioEngine::PluginRuntimeRequest request;
                    request.trackId = track.id;
                    request.insertId = insert.id;
                    request.name = insert.name;
                    request.deviceIdentifier = insert.pluginIdentifier;
                    request.bridgeMode = insert.bridgeMode;
                    requests.push_back(std::move(request));
                }
            return requests;
        });
    expect(reports.size() == 2
               && reports[0].status == "success"
               && reports[1].status == "failed"
               && juce::File(reports[0].outputFile).existsAsFile(),
           "Batch rendering returns explicit success and failure reports.");
    outputDirectory.deleteRecursively();

    const auto paritySource = juce::File::getSpecialLocation(
                                  juce::File::tempDirectory)
                                  .getNonexistentChildFile(
                                      "StudioDuoRenderParity",
                                      ".wav",
                                      false);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            paritySource.createOutputStream();
        auto writer = wav.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions {}
                .withSampleRate(48000.0)
                .withNumChannels(2)
                .withBitsPerSample(24));
        juce::AudioBuffer<float> source(2, 2048);
        for (int sample = 0; sample < source.getNumSamples(); ++sample)
        {
            const auto value = sample == 0
                ? 0.2f
                : static_cast<float>(
                      0.1
                      * std::sin(
                          juce::MathConstants<double>::twoPi
                          * 997.0
                          * static_cast<double>(sample)
                          / 48000.0));
            source.setSample(0, sample, value);
            source.setSample(1, sample, value);
        }
        expect(writer != nullptr
                   && writer->writeFromAudioSampleBuffer(
                       source,
                       0,
                       source.getNumSamples()),
               "Render parity source can be written.");
        if (writer != nullptr)
            writer->flush();
    }

    auto parityProject = studio::Project::createDefault();
    parityProject.metronomeEnabled = false;
    studio::AudioClip parityClip;
    parityClip.sourceFile = paritySource;
    parityClip.durationSeconds = 2048.0 / 48000.0;
    parityClip.sourceLengthSeconds = parityClip.durationSeconds;
    parityClip.sourceRangeEndSeconds = parityClip.durationSeconds;
    parityProject.tracks[0].clips.push_back(parityClip);
    parityClip.id = juce::Uuid().toString();
    parityProject.tracks[1].clips.push_back(parityClip);

    studio::PluginInsert limiter;
    limiter.pluginIdentifier = "studio.device.limiter";
    limiter.name = "Parity limiter";
    limiter.format = "Studio Duo";
    limiter.bundledDevice = true;
    limiter.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    parityProject.tracks[0].inserts.push_back(limiter);

    studio::AutomationLane parityAutomation;
    parityAutomation.name = "Parity volume";
    parityAutomation.target.type =
        studio::AutomationTargetType::trackVolume;
    parityAutomation.target.trackId = parityProject.tracks[0].id;
    parityAutomation.interpolation =
        studio::AutomationInterpolation::linear;
    parityAutomation.points = {
        { juce::Uuid().toString(), 0.0, 0.5 },
        { juce::Uuid().toString(),
          parityClip.durationSeconds,
          1.0 }
    };
    parityProject.automationLanes.push_back(parityAutomation);

    studio::StudioAudioEngine::PluginRuntimeRequest parityRequest;
    parityRequest.trackId = parityProject.tracks[0].id;
    parityRequest.insertId = limiter.id;
    parityRequest.name = limiter.name;
    parityRequest.deviceIdentifier = limiter.pluginIdentifier;
    parityRequest.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;

    studio::StudioAudioEngine offlineEngine;
    juce::AudioBuffer<float> offline;
    expect(offlineEngine.renderToBuffer(
               parityProject,
               offline,
               48000.0,
               { parityRequest })
               .wasOk(),
           "Deterministic plugin rendering produces an offline reference.");

    studio::StudioAudioEngine realtimeEngine;
    expect(realtimeEngine.updateProject(
               parityProject,
               { parityRequest })
               .wasOk(),
           "Deterministic plugin rendering publishes a live graph.");
    for (int attempt = 0;
         attempt < 200
         && realtimeEngine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    realtimeEngine.seekSeconds(0.0);
    realtimeEngine.play();
    const auto realtime = realtimeEngine.renderActiveBlockForTesting(
        offline.getNumSamples());
    auto maximumDifference = 0.0f;
    for (int channel = 0; channel < offline.getNumChannels(); ++channel)
        for (int sample = 0; sample < offline.getNumSamples(); ++sample)
            maximumDifference = std::max(
                maximumDifference,
                std::abs(
                    offline.getSample(channel, sample)
                    - realtime.getSample(channel, sample)));
    expect(realtime.getNumSamples() == offline.getNumSamples()
               && maximumDifference < 0.00001f,
           ("Deterministic offline and live renders null within tolerance (maximum difference "
            + juce::String(maximumDifference, 8)
            + ").")
               .toRawUTF8());

    auto linkedReampProject = studio::Project::createDefault();
    linkedReampProject.metronomeEnabled = false;
    linkedReampProject.tracks[0].clips.push_back(parityClip);
    auto staleReturnCopy = parityClip;
    staleReturnCopy.id = juce::Uuid().toString();
    staleReturnCopy.gainDecibels = 6.0f;
    linkedReampProject.tracks[1].clips.push_back(staleReturnCopy);
    linkedReampProject.tracks[1].solo = true;
    studio::ReampRoute linkedRoute;
    linkedRoute.type = studio::TonePathType::plugin;
    linkedRoute.sourceTrackId = linkedReampProject.tracks[0].id;
    linkedRoute.returnTrackId = linkedReampProject.tracks[1].id;
    linkedRoute.ownsReturnTrack = true;
    linkedReampProject.reampRoutes.push_back(linkedRoute);

    juce::AudioBuffer<float> linkedTone;
    expect(offlineEngine.renderToBuffer(
               linkedReampProject,
               linkedTone,
               48000.0)
               .wasOk(),
           "A plugin tone path renders from its linked DI playlist.");
    expect(linkedTone.getNumSamples() > 0
               && linkedTone.getSample(0, 0) > 0.19f
               && linkedTone.getSample(0, 0) < 0.21f,
           "A plugin return references current DI audio instead of stale clip copies.");
    linkedReampProject.tracks[0].clips.front().gainDecibels = -6.0f;
    juce::AudioBuffer<float> editedTone;
    expect(offlineEngine.renderToBuffer(
               linkedReampProject,
               editedTone,
               48000.0)
               .wasOk(),
           "A linked plugin tone path rerenders after a DI edit.");
    expect(editedTone.getNumSamples() > 0
               && editedTone.getSample(0, 0)
                      < linkedTone.getSample(0, 0) * 0.6f,
           "Plugin tone paths follow the current DI playlist instead of stale clip copies.");
    paritySource.deleteFile();
}
