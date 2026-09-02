#include "TestHarness.h"
#include "TestSuites.h"

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
}
