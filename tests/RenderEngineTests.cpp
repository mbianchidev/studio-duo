#include "TestHarness.h"
#include "TestSuites.h"

#include "automation/AutomationTypes.h"
#include "render/RenderEngine.h"
#include "reamp/ReampSnapshotService.h"

#include <thread>
#include <limits>

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
    parityProject.tempoChanges = {
        { 0.0, 120.0, true },
        { 0.02, 240.0, false }
    };
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
    parityAutomation.timebase =
        studio::AutomationTimebase::beats;
    parityAutomation.interpolation =
        studio::AutomationInterpolation::linear;
    parityAutomation.points = {
        { juce::Uuid().toString(), 0.0, 0.5 },
        { juce::Uuid().toString(),
          parityProject.beatsAt(parityClip.durationSeconds),
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

    auto backgroundProject = studio::Project::createDefault();
    backgroundProject.metronomeEnabled = false;
    backgroundProject.tracks[0].clips.push_back(parityClip);
    const auto backgroundOutput =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile(
                "StudioDuoBackgroundExport",
                ".wav",
                false);
    auto backgroundExported = false;
    juce::String backgroundError;
    studio::StudioAudioEngine backgroundEngine;
    std::thread exportThread(
        [&]
        {
            const auto result = backgroundEngine.renderToWav(
                backgroundProject,
                backgroundOutput,
                48000.0);
            backgroundExported = result.wasOk();
            backgroundError = result.getErrorMessage();
        });
    exportThread.join();
    expect(backgroundExported
               && backgroundOutput.existsAsFile()
               && backgroundOutput.getSize() > 44,
           ("Stereo mix export can render on a worker thread without blocking the UI: "
            + backgroundError)
               .toRawUTF8());
    backgroundOutput.deleteFile();

    const auto backgroundPluginOutput =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile(
                "StudioDuoBackgroundPluginExport",
                ".wav",
                false);
    backgroundExported = false;
    backgroundError.clear();
    std::thread pluginExportThread(
        [&]
        {
            const auto result = backgroundEngine.renderToWav(
                parityProject,
                backgroundPluginOutput,
                48000.0,
                { parityRequest });
            backgroundExported = result.wasOk();
            backgroundError = result.getErrorMessage();
        });
    pluginExportThread.join();
    expect(backgroundExported
               && backgroundPluginOutput.existsAsFile()
               && backgroundPluginOutput.getSize() > 44,
           ("Plugin-inclusive mix export can render on a worker thread without blocking the UI: "
            + backgroundError)
               .toRawUTF8());
    backgroundPluginOutput.deleteFile();

    auto rangeProject = backgroundProject;
    rangeProject.loopEnabled = true;
    rangeProject.loopStartSeconds = 5.0 / 48000.0;
    rangeProject.loopEndSeconds = 37.0 / 48000.0;
    juce::AudioBuffer<float> rangeAudio;
    const auto rangeResult = backgroundEngine.renderRangeToBuffer(
        rangeProject,
        rangeAudio,
        48000.0,
        { 101.0 / 48000.0, 557.0 / 48000.0, 0.0 });
    juce::AudioBuffer<float> linearReference;
    const auto linearResult = backgroundEngine.renderToBuffer(
        backgroundProject, linearReference, 48000.0);
    auto linearDifference = 0.0f;
    if (rangeResult.wasOk() && linearResult.wasOk())
        for (int sample = 0; sample < rangeAudio.getNumSamples(); ++sample)
            linearDifference = std::max(
                linearDifference,
                std::abs(rangeAudio.getSample(0, sample)
                         - linearReference.getSample(0, sample + 101)));
    expect(rangeResult.wasOk() && linearResult.wasOk()
               && rangeAudio.getNumSamples() == 456
               && rangeAudio.getMagnitude(0, 456) > 0.09f
               && linearDifference < 0.000001f,
           ("An exact export range starts at the requested sample and ignores playback looping: "
            + rangeResult.getErrorMessage()).toRawUTF8());

    auto tailResult = backgroundEngine.renderRangeToBuffer(
        rangeProject, rangeAudio, 48000.0,
        { 101.0 / 48000.0, 557.0 / 48000.0, 64.0 / 48000.0 });
    expect(tailResult.wasOk()
               && rangeAudio.getNumSamples() == 520
               && rangeAudio.getMagnitude(0, 456, 64) == 0.0f,
           "A custom export tail excludes source audio beyond the selected end.");

    juce::AudioBuffer<float> pluginRange;
    const auto pluginRangeResult = backgroundEngine.renderRangeToBuffer(
        parityProject, pluginRange, 48000.0,
        { 101.0 / 48000.0, 557.0 / 48000.0, 0.0 },
        { parityRequest });
    auto rangeDifference = 0.0f;
    if (pluginRangeResult.wasOk())
        for (int sample = 0; sample < pluginRange.getNumSamples(); ++sample)
            rangeDifference = std::max(
                rangeDifference,
                std::abs(pluginRange.getSample(0, sample)
                         - offline.getSample(0, sample + 101 + 16)));
    expect(pluginRangeResult.wasOk()
               && pluginRange.getNumSamples() == 456
               && rangeDifference < 0.00001f,
           ("Range export preserves preceding plugin/automation state and removes graph latency: "
            + pluginRangeResult.getErrorMessage()
            + " maximum difference " + juce::String(rangeDifference, 8)).toRawUTF8());

    auto reverbProject = backgroundProject;
    studio::PluginInsert reverb = limiter;
    reverb.id = juce::Uuid().toString();
    reverb.pluginIdentifier = "studio.device.reverb";
    reverb.name = "Export tail reverb";
    reverbProject.tracks[0].inserts = { reverb };
    auto reverbRequest = parityRequest;
    reverbRequest.trackId = reverbProject.tracks[0].id;
    reverbRequest.insertId = reverb.id;
    reverbRequest.name = reverb.name;
    reverbRequest.deviceIdentifier = reverb.pluginIdentifier;
    const auto reverbResult = backgroundEngine.renderRangeToBuffer(
        reverbProject, rangeAudio, 48000.0, { 0.0, 0.02, 0.15 }, { reverbRequest });
    expect(reverbResult.wasOk()
               && rangeAudio.getNumSamples() == 8160
               && rangeAudio.getMagnitude(0, 960, 7200) > 0.0001f,
           "Custom export tails render actual reverb decay instead of appending silence.");
    const auto automaticTailResult = backgroundEngine.renderRangeToBuffer(
        reverbProject, rangeAudio, 48000.0, { 0.0, 0.02, std::nullopt }, { reverbRequest });
    expect(automaticTailResult.wasOk()
               && rangeAudio.getNumSamples() == 192960,
           "Automatic tails use the prepared graph's reported effect tail.");

    auto midiRangeProject = backgroundProject;
    auto& midiRangeTrack = midiRangeProject.tracks[0];
    midiRangeTrack.type = studio::TrackType::instrument;
    midiRangeTrack.armed = false;
    midiRangeTrack.clips.clear();
    studio::PluginInsert drum = limiter;
    drum.id = juce::Uuid().toString();
    drum.pluginIdentifier = "studio.device.drum-composer";
    drum.name = "Export MIDI range fixture";
    midiRangeTrack.inserts = { drum };
    studio::MidiClip lateMidi;
    lateMidi.durationBeats = midiRangeProject.beatsAt(0.12);
    studio::MidiNote lateNote;
    lateNote.pitch = 36;
    lateNote.startBeats = midiRangeProject.beatsAt(0.05);
    lateNote.durationBeats = midiRangeProject.beatsAt(0.01);
    lateMidi.notes = { lateNote };
    midiRangeTrack.midiClips = { lateMidi };
    auto drumRequest = parityRequest;
    drumRequest.trackId = midiRangeTrack.id;
    drumRequest.insertId = drum.id;
    drumRequest.name = drum.name;
    drumRequest.deviceIdentifier = drum.pluginIdentifier;
    const auto midiControlResult = backgroundEngine.renderRangeToBuffer(
        midiRangeProject, rangeAudio, 48000.0, { 0.0, 0.12, 0.0 }, { drumRequest });
    expect(midiControlResult.wasOk() && rangeAudio.getNumSamples() == 5760
               && rangeAudio.getMagnitude(0, 5760) > 0.001f,
           "The MIDI export boundary fixture produces audible scheduled notes.");
    const auto midiTailResult = backgroundEngine.renderRangeToBuffer(
        midiRangeProject, rangeAudio, 48000.0, { 0.0, 0.02, 0.1 }, { drumRequest });
    expect(midiTailResult.wasOk()
               && rangeAudio.getNumSamples() == 5760
               && rangeAudio.getMagnitude(0, 5760) == 0.0f,
           "Export tails cannot play MIDI notes scheduled after the selected end.");

    expect(backgroundEngine.renderRangeToBuffer(
               rangeProject, rangeAudio, 48000.0, { 1.0, 1.0, 0.0 }).failed()
               && backgroundEngine.renderRangeToBuffer(
                   rangeProject, rangeAudio, 48000.0, { 2.0, 1.0, 0.0 }).failed()
               && backgroundEngine.renderRangeToBuffer(
                   rangeProject, rangeAudio, 48000.0,
                   { 0.0, std::numeric_limits<double>::infinity(), 0.0 }).failed()
               && backgroundEngine.renderRangeToBuffer(
                   rangeProject, rangeAudio, std::numeric_limits<double>::quiet_NaN(),
                   { 0.0, 1.0, 0.0 }).failed()
               && backgroundEngine.renderRangeToBuffer(
                   rangeProject, rangeAudio, 48000.0, { 0.0, 1.0, -1.0 }).failed()
               && backgroundEngine.renderRangeToBuffer(
                   rangeProject, rangeAudio, 48000.0, { 0.0, 0.1 / 48000.0, 0.0 }).failed(),
           "Invalid, non-finite, reversed, and sub-sample export ranges fail explicitly.");

    studio::SongSection startMarker;
    startMarker.name = "Boundary";
    startMarker.timeSeconds = 101.0 / 48000.0;
    studio::SongSection endMarker;
    endMarker.name = "Boundary";
    endMarker.timeSeconds = 557.0 / 48000.0;
    rangeProject.sections = { startMarker, endMarker };
    studio::MixExportSettings mixSettings;
    mixSettings.range = studio::MixExportRange::markers;
    mixSettings.startMarkerId = startMarker.id;
    mixSettings.endMarkerId = endMarker.id;
    mixSettings.tailSeconds = 0.0;
    juce::String rangeError;
    const auto resolvedRange = studio::RenderEngine::resolveRange(
        rangeProject, mixSettings, rangeError);
    expect(resolvedRange.has_value()
               && std::abs(resolvedRange->startSeconds - startMarker.timeSeconds) < 1.0e-12
               && std::abs(resolvedRange->endSeconds - endMarker.timeSeconds) < 1.0e-12,
           ("Named export markers resolve by stable ID even with duplicate names: "
            + rangeError).toRawUTF8());

    const auto mixDestination = paritySource.getSiblingFile(
        paritySource.getFileNameWithoutExtension() + "-range.wav");
    mixSettings.audio.bitDepth = 32;
    mixSettings.audio.channels = 1;
    mixSettings.fadeInSeconds = 3.0 / 48000.0;
    mixSettings.fadeOutSeconds = 3.0 / 48000.0;
    const auto exportResult = studio::RenderEngine::exportMix(
        backgroundEngine, rangeProject, mixDestination, mixSettings);
    juce::AudioFormatManager exportFormats;
    exportFormats.registerBasicFormats();
    auto exportedReader = std::unique_ptr<juce::AudioFormatReader>(
        exportFormats.createReaderFor(mixDestination));
    juce::AudioBuffer<float> exportedAudio(1, 456);
    expect(exportResult.wasOk()
               && exportedReader != nullptr
               && exportedReader->numChannels == 1
               && exportedReader->sampleRate == 48000.0
               && exportedReader->bitsPerSample == 32
               && exportedReader->usesFloatingPointData
               && exportedReader->lengthInSamples == 456
               && exportedReader->read(&exportedAudio, 0, 456, 0, true, false)
               && exportedAudio.getSample(0, 0) == 0.0f
               && exportedAudio.getSample(0, 455) == 0.0f
               && exportedAudio.getMagnitude(0, 3, 450) > 0.09f,
           ("Marker export writes the selected encoding and exact endpoint fades: "
            + exportResult.getErrorMessage()).toRawUTF8());
    exportedReader.reset();
    juce::MemoryBlock previousMix;
    const auto savedPreviousMix = mixDestination.loadFileAsData(previousMix);
    mixSettings.endMarkerId = "missing-marker";
    const auto invalidExport = studio::RenderEngine::exportMix(
        backgroundEngine, rangeProject, mixDestination, mixSettings);
    juce::MemoryBlock retainedMix;
    expect(savedPreviousMix && invalidExport.failed()
               && mixDestination.loadFileAsData(retainedMix)
               && retainedMix == previousMix,
           "Missing export markers cannot replace an existing file or fall back to a full mix.");
    mixDestination.deleteFile();

    mixSettings.range = studio::MixExportRange::custom;
    mixSettings.startSeconds = 0.51 / 48000.0;
    mixSettings.endSeconds = 5.49 / 48000.0;
    mixSettings.fadeInSeconds = 4.9 / 48000.0;
    mixSettings.fadeOutSeconds = 0.0;
    expect(!studio::RenderEngine::resolveRange(rangeProject, mixSettings, rangeError).has_value(),
           "Fades must fit the actual sample-rounded interval, not just its unrounded duration.");

    backgroundEngine.shutdown();
    paritySource.deleteFile();
}
