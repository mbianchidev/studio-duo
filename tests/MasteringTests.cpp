#include "TestHarness.h"
#include "TestSuites.h"

#include "mastering/MasteringEngine.h"
#include "mastering/MasteringReleaseService.h"
#include "project_io/ProjectCollectionService.h"
#include "project_io/ProjectFile.h"
#include "plugin_host/PluginStateStore.h"
#include "model/ProjectCommands.h"
#include "model/ProjectModel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <cstring>

#ifndef STUDIO_DUO_DDP_FIXTURE_PATH
#define STUDIO_DUO_DDP_FIXTURE_PATH ""
#endif

namespace
{
bool writeConstantWave(const juce::File& file,
                       float value,
                       double sampleRate,
                       double seconds)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream =
        file.createOutputStream();
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(sampleRate)
            .withNumChannels(2)
            .withBitsPerSample(24));
    if (writer == nullptr)
        return false;
    juce::AudioBuffer<float> audio(
        2,
        static_cast<int>(std::llround(sampleRate * seconds)));
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        audio.clear(channel, 0, audio.getNumSamples());
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            audio.setSample(channel, sample, value);
    return writer->writeFromAudioSampleBuffer(
        audio,
        0,
        audio.getNumSamples());
}

bool writeSineWave(const juce::File& file,
                   double frequency,
                   float peak,
                   double sampleRate,
                   double seconds)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream =
        file.createOutputStream();
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(sampleRate)
            .withNumChannels(2)
            .withBitsPerSample(24));
    if (writer == nullptr)
        return false;
    juce::AudioBuffer<float> audio(
        2,
        static_cast<int>(std::llround(sampleRate * seconds)));
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            audio.setSample(
                channel,
                sample,
                peak
                    * static_cast<float>(
                        std::sin(
                            juce::MathConstants<double>::twoPi
                            * frequency
                            * static_cast<double>(sample)
                            / sampleRate)));
    return writer->writeFromAudioSampleBuffer(
        audio,
        0,
        audio.getNumSamples());
}
}

void masteringTests()
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto seconds = 3;
    juce::AudioBuffer<float> calibration(1,
                                         static_cast<int>(sampleRate)
                                             * seconds);
    const auto peak = static_cast<float>(
        std::pow(10.0, (-23.0 + 3.010299956639812) / 20.0));
    for (int sample = 0; sample < calibration.getNumSamples(); ++sample)
        calibration.setSample(
            0,
            sample,
            peak
                * static_cast<float>(
                    std::sin(
                        juce::MathConstants<double>::twoPi
                        * 1000.0
                        * static_cast<double>(sample)
                        / sampleRate)));

    const auto analysis = studio::MasteringEngine::analyse(
        calibration,
        sampleRate);
    expect(analysis.integratedLoudnessLufs.has_value()
               && std::abs(
                      *analysis.integratedLoudnessLufs
                      - (-23.0))
                      < 0.2,
           "BS.1770 analysis measures a 1 kHz calibration tone at -23 LUFS.");

    juce::AudioBuffer<float> interSamplePeak(2, 4096);
    for (int channel = 0; channel < interSamplePeak.getNumChannels(); ++channel)
        for (int sample = 0;
             sample < interSamplePeak.getNumSamples();
             ++sample)
            interSamplePeak.setSample(
                channel,
                sample,
                static_cast<float>(
                    0.7
                    * std::sin(
                        juce::MathConstants<double>::twoPi
                            * 12000.0
                            * static_cast<double>(sample)
                            / sampleRate
                        + juce::MathConstants<double>::pi * 0.25)));

    const auto peakAnalysis = studio::MasteringEngine::analyse(
        interSamplePeak,
        sampleRate);
    expect(peakAnalysis.truePeakDbtp
                   > peakAnalysis.samplePeakDbfs + 0.1,
           ("True-peak analysis detects inter-sample peaks above PCM sample peaks (sample "
            + juce::String(peakAnalysis.samplePeakDbfs, 3)
            + " dBFS, true "
            + juce::String(peakAnalysis.truePeakDbtp, 3)
            + " dBTP).")
               .toRawUTF8());

    juce::AudioBuffer<float> dynamicProgramme(
        2,
        static_cast<int>(sampleRate * 12.0));
    for (int sample = 0;
         sample < dynamicProgramme.getNumSamples();
         ++sample)
    {
        const auto rmsLevel = sample < dynamicProgramme.getNumSamples() / 2
            ? -30.0
            : -20.0;
        const auto value = static_cast<float>(
            std::pow(10.0, (rmsLevel + 3.010299956639812) / 20.0)
            * std::sin(
                juce::MathConstants<double>::twoPi
                * 1000.0
                * static_cast<double>(sample)
                / sampleRate));
        dynamicProgramme.setSample(0, sample, value);
        dynamicProgramme.setSample(1, sample, value);
    }
    const auto dynamicAnalysis = studio::MasteringEngine::analyse(
        dynamicProgramme,
        sampleRate);
    expect(dynamicAnalysis.loudnessRangeLu.has_value()
               && *dynamicAnalysis.loudnessRangeLu > 5.0
               && dynamicAnalysis.correlation > 0.99,
           "EBU R128 analysis reports programme loudness range and stereo correlation.");

    auto project = studio::Project::createDefault();
    project.mastering.title = "Fixture Album";
    project.mastering.artist = "Fixture Artist";
    project.mastering.songwriter = "Fixture Writer";
    project.mastering.label = "Fixture Label";
    project.mastering.catalogNumber = "CAT-001";
    project.mastering.mcn = "4006381333931";
    project.mastering.releaseDate = "2026-09-17";
    project.mastering.outputGainDecibels = -0.5;

    studio::MasteringTrack firstTrack;
    firstTrack.title = "First";
    firstTrack.artist = "Fixture Artist";
    firstTrack.isrc = "USABC2600001";
    firstTrack.gapBeforeSeconds = 1.0;
    firstTrack.fadeInSeconds = 0.1;
    firstTrack.fadeOutSeconds = 0.2;
    firstTrack.indexMarkersSeconds = { 0.0, 1.0 };
    studio::MasteringSourceMix firstMain;
    firstMain.name = "Main";
    firstMain.file = juce::File("/tmp/first-main.wav");
    firstMain.sourceHash = juce::String::repeatedString("a", 64);
    firstMain.durationSeconds = 2.0;
    studio::MasteringSourceMix firstAlternate;
    firstAlternate.name = "Alternate";
    firstAlternate.file = juce::File("/tmp/first-alt.wav");
    firstAlternate.sourceHash = juce::String::repeatedString("b", 64);
    firstAlternate.durationSeconds = 2.5;
    firstTrack.sources = { firstMain, firstAlternate };
    firstTrack.selectedSourceId = firstMain.id;

    studio::MasteringTrack secondTrack;
    secondTrack.title = "Second";
    secondTrack.gapBeforeSeconds = 0.5;
    secondTrack.overlapPreviousSeconds = 0.25;
    studio::MasteringSourceMix secondMain;
    secondMain.name = "Main";
    secondMain.file = juce::File("/tmp/second.wav");
    secondMain.durationSeconds = 3.0;
    secondTrack.sources = { secondMain };
    secondTrack.selectedSourceId = secondMain.id;
    project.mastering.tracks = { firstTrack, secondTrack };

    studio::MasteringReference reference;
    reference.name = "Reference";
    reference.file = juce::File("/tmp/reference.wav");
    reference.sourceHash = juce::String::repeatedString("c", 64);
    project.mastering.references = { reference };

    juce::String modelError;
    const auto restored = studio::Project::fromVar(
        project.toVar(),
        modelError);
    const auto placements = restored.has_value()
        ? restored->mastering.placements()
        : std::vector<studio::MasteringTrackPlacement> {};
    expect(restored.has_value()
               && restored->mastering.tracks.size() == 2
               && restored->mastering.tracks.front().sources.size() == 2
               && restored->mastering.references.size() == 1
               && placements.size() == 2
               && std::abs(placements[0].startSeconds - 1.0) < 0.0001
               && std::abs(placements[1].startSeconds - 3.25) < 0.0001
               && std::abs(restored->mastering.durationSeconds() - 6.25)
                      < 0.0001,
           ("Mastering album sequencing and release metadata survive project serialization: "
            + modelError)
               .toRawUTF8());

    const auto renderDirectory = juce::File::getSpecialLocation(
                                     juce::File::tempDirectory)
                                     .getNonexistentChildFile(
                                         "StudioDuoMastering",
                                         {},
                                         false);
    renderDirectory.createDirectory();
    const auto mainFile = renderDirectory.getChildFile("main.wav");
    const auto alternateFile =
        renderDirectory.getChildFile("alternate.wav");
    const auto secondFile = renderDirectory.getChildFile("second.wav");
    const auto referenceFile =
        renderDirectory.getChildFile("reference.wav");
    expect(writeConstantWave(mainFile, 0.25f, sampleRate, 1.0)
               && writeConstantWave(
                   alternateFile,
                   0.5f,
                   sampleRate,
                   1.0)
               && writeConstantWave(
                   secondFile,
                   0.25f,
                   sampleRate,
                   1.0)
               && writeConstantWave(
                   referenceFile,
                   0.9f,
                   sampleRate,
                   1.0),
           "Mastering render fixtures can be written.");

    studio::MasteringAlbum renderAlbum;
    renderAlbum.outputGainDecibels = -6.020599913279624;
    auto renderFirst = firstTrack;
    renderFirst.gapBeforeSeconds = 0.1;
    renderFirst.overlapPreviousSeconds = 0.0;
    renderFirst.fadeInSeconds = 0.1;
    renderFirst.fadeOutSeconds = 0.1;
    renderFirst.sources[0].file = mainFile;
    renderFirst.sources[0].sourceHash =
        juce::SHA256(mainFile).toHexString();
    renderFirst.sources[0].durationSeconds = 1.0;
    renderFirst.sources[1].file = alternateFile;
    renderFirst.sources[1].sourceHash =
        juce::SHA256(alternateFile).toHexString();
    renderFirst.sources[1].durationSeconds = 1.0;
    renderFirst.selectedSourceId = renderFirst.sources[1].id;
    auto renderSecond = secondTrack;
    renderSecond.gapBeforeSeconds = 0.2;
    renderSecond.overlapPreviousSeconds = 0.1;
    renderSecond.fadeInSeconds = 0.1;
    renderSecond.fadeOutSeconds = 0.1;
    renderSecond.sources[0].file = secondFile;
    renderSecond.sources[0].sourceHash =
        juce::SHA256(secondFile).toHexString();
    renderSecond.sources[0].durationSeconds = 1.0;
    renderAlbum.tracks = { renderFirst, renderSecond };
    reference.file = referenceFile;
    reference.sourceHash = juce::SHA256(referenceFile).toHexString();
    renderAlbum.references = { reference };

    juce::String renderError;
    const auto rendered = studio::MasteringEngine::renderAlbum(
        renderAlbum,
        sampleRate,
        renderError);
    const auto middleFirst = static_cast<int>(sampleRate * 0.6);
    const auto expectedSamples = static_cast<int>(sampleRate * 2.2);
    expect(rendered.has_value()
               && rendered->placements.size() == 2
               && rendered->audio.getNumSamples() == expectedSamples
               && std::abs(rendered->audio.getSample(0, 0)) < 0.00001f
               && std::abs(
                      rendered->audio.getSample(0, middleFirst)
                      - 0.25f)
                      < 0.001f
               && rendered->audio.getMagnitude(
                      0,
                      0,
                      rendered->audio.getNumSamples())
                      < 0.46f,
           ("Mastering rendering uses the selected mix, gaps, fades, overlap, output gain, and excludes references: "
            + renderError)
               .toRawUTF8());

    studio::MasteringExportSettings exportSettings;
    exportSettings.format = studio::MasteringExportFormat::wav;
    exportSettings.sampleRate = 44100.0;
    exportSettings.bitDepth = 16;
    exportSettings.dither = studio::MasteringDither::tpdf;
    exportSettings.distributionPresetId = "streaming-balanced";
    const auto firstMaster =
        renderDirectory.getChildFile("release-a.wav");
    const auto signingDirectory =
        renderDirectory.getChildFile("signing");
    juce::String exportError;
    const auto firstReport =
        studio::MasteringReleaseService::exportMaster(
            renderAlbum,
            firstMaster,
            exportSettings,
            signingDirectory,
            exportError);
    const auto secondReport =
        studio::MasteringReleaseService::exportMaster(
            renderAlbum,
            firstMaster,
            exportSettings,
            signingDirectory,
            exportError);
    exportSettings.format = studio::MasteringExportFormat::flac;
    exportSettings.bitDepth = 24;
    exportSettings.dither = studio::MasteringDither::none;
    const auto flacMaster =
        renderDirectory.getChildFile("release.flac");
    const auto flacReport =
        studio::MasteringReleaseService::exportMaster(
            renderAlbum,
            flacMaster,
            exportSettings,
            signingDirectory,
            exportError);
    exportSettings.format =
        studio::MasteringExportFormat::oggVorbis;
    exportSettings.bitDepth = 32;
    const auto oggReference =
        renderDirectory.getChildFile("reference.ogg");
    const auto oggReport =
        studio::MasteringReleaseService::exportMaster(
            renderAlbum,
            oggReference,
            exportSettings,
            signingDirectory,
            exportError);
    juce::String signatureError;
    juce::String untrustedSignatureError;
    expect(firstReport.has_value()
               && secondReport.has_value()
               && flacReport.has_value()
               && oggReport.has_value()
               && firstMaster.existsAsFile()
               && flacMaster.existsAsFile()
               && oggReference.existsAsFile()
               && firstReport->outputHash == secondReport->outputHash
               && firstReport->sampleRate == 44100.0
               && firstReport->bitDepth == 16
               && firstReport->integratedLoudnessLufs.has_value()
               && firstReport->signature.isNotEmpty()
               && studio::MasteringReleaseService::verifyReport(
                   *firstReport,
                   signingDirectory,
                   signatureError)
               && !studio::MasteringReleaseService::verifyReport(
                   *firstReport,
                   renderDirectory.getChildFile("untrusted-signing"),
                   untrustedSignatureError),
           ("WAV and FLAC release exports use deterministic dither, final-file measurements, distribution warnings, and verifiable signatures: "
            + exportError
            + " "
            + signatureError)
               .toRawUTF8());

    exportSettings.format = studio::MasteringExportFormat::mp3;
    const auto mp3Master = renderDirectory.getChildFile("reference.mp3");
    const auto mp3Report = studio::MasteringReleaseService::exportMaster(
        renderAlbum,
        mp3Master,
        exportSettings,
        signingDirectory,
        exportError);
    expect(mp3Report.has_value()
               && mp3Master.existsAsFile()
               && mp3Report->format == "mp3"
               && mp3Report->integratedLoudnessLufs.has_value()
               && studio::MasteringReleaseService::verifyReport(
                   *mp3Report, signingDirectory, signatureError),
           ("MP3 masters are encoded, remeasured and signed like lossless releases: "
            + exportError).toRawUTF8());

    exportSettings.format = studio::MasteringExportFormat::aiff;
    exportSettings.bitDepth = 24;
    exportSettings.channels = 1;
    exportSettings.normalizePeak = true;
    exportSettings.normalizePeakDbfs = -3.0;
    const auto aiffMaster = renderDirectory.getChildFile("normalized.aiff");
    const auto aiffReport = studio::MasteringReleaseService::exportMaster(
        renderAlbum, aiffMaster, exportSettings, signingDirectory, exportError);
    juce::AudioFormatManager exportFormats;
    exportFormats.registerBasicFormats();
    auto aiffReader = std::unique_ptr<juce::AudioFormatReader>(
        exportFormats.createReaderFor(aiffMaster));
    expect(aiffReport.has_value() && aiffReader != nullptr
               && aiffReport->format == "aiff"
               && aiffReader->numChannels == 1
               && aiffReader->bitsPerSample == 24
               && std::abs(aiffReport->samplePeakDbfs + 3.0) < 0.01
               && studio::MasteringReleaseService::verifyReport(
                   *aiffReport, signingDirectory, signatureError)
               && static_cast<int>(exportSettings.toVar().getProperty("channels", 0)) == 1
               && static_cast<bool>(exportSettings.toVar().getProperty("normalizePeak", false)),
           ("Mastering shares AIFF, mono and explicit peak normalization with mix export: "
            + exportError).toRawUTF8());
    aiffReader.reset();

    auto ddpAlbum = renderAlbum;
    ddpAlbum.tracks[0].gapBeforeSeconds = 2.0;
    ddpAlbum.tracks[0].overlapPreviousSeconds = 0.0;
    ddpAlbum.tracks[1].gapBeforeSeconds = 0.0;
    ddpAlbum.tracks[1].overlapPreviousSeconds = 0.0;
    const auto ddpDirectory =
        renderDirectory.getChildFile("release.ddp");
    exportError.clear();
    const auto ddpReport =
        studio::MasteringReleaseService::exportDdp(
            ddpAlbum,
            ddpDirectory,
            juce::File(STUDIO_DUO_DDP_FIXTURE_PATH),
            signingDirectory,
            exportError);
    signatureError.clear();
    expect(ddpReport.has_value()
               && ddpDirectory.getChildFile("DDPID").existsAsFile()
               && ddpDirectory.getChildFile("DDPMS").existsAsFile()
               && ddpDirectory.getChildFile("IMAGE.DAT").existsAsFile()
               && ddpDirectory.getChildFile("PQ_DESCR").existsAsFile()
               && ddpDirectory.getChildFile(
                      "CHECKSUMS.sha256")
                      .existsAsFile()
               && studio::MasteringReleaseService::verifyReport(
                   *ddpReport,
                   signingDirectory,
                   signatureError),
           ("External DDP encoding produces a validated, hashed, signed fileset: "
            + exportError
            + " "
            + signatureError)
               .toRawUTF8());

    auto collectionProject = studio::Project::createDefault();
    collectionProject.metadata.comment =
        "${PROJECT_DIR}/../literal metadata";
    studio::AudioClip collectedClip;
    collectedClip.name = "Collected";
    collectedClip.sourceFile = mainFile;
    collectedClip.sourceHash = juce::SHA256(mainFile).toHexString();
    collectedClip.durationSeconds = 1.0;
    collectedClip.sourceLengthSeconds = 1.0;
    collectedClip.sourceRangeEndSeconds = 1.0;
    collectionProject.tracks.front().clips.push_back(collectedClip);
    collectionProject.mastering = renderAlbum;
    const auto sourcePackage =
        renderDirectory.getChildFile("Source.studioduo");
    const juce::MemoryBlock pluginState(
        "fixture processor state",
        std::strlen("fixture processor state"));
    juce::String pluginStateError;
    const auto storedPluginState =
        studio::PluginStateStore::store(
            sourcePackage,
            pluginState,
            pluginStateError);
    expect(storedPluginState.has_value(),
           ("Plugin-state fixture can be stored: "
            + pluginStateError)
               .toRawUTF8());
    studio::PluginInsert collectedInsert;
    collectedInsert.pluginIdentifier = "fixture.plugin";
    collectedInsert.name = "Fixture plugin";
    collectedInsert.format = "VST3";
    collectedInsert.stateFile = storedPluginState.has_value()
        ? storedPluginState->relativePath
        : juce::String();
    collectedInsert.stateHash = storedPluginState.has_value()
        ? storedPluginState->hash
        : juce::String();
    collectionProject.tracks.front().inserts.push_back(collectedInsert);
    const auto toneRenderFile =
        sourcePackage.getChildFile("renders")
            .getChildFile("tones")
            .getChildFile("fixture.wav");
    toneRenderFile.getParentDirectory().createDirectory();
    expect(secondFile.copyFileTo(toneRenderFile),
           "Package-relative tone render fixture can be copied.");
    studio::ReampRoute collectedRoute;
    collectedRoute.sourceTrackId =
        collectionProject.tracks[0].id;
    collectedRoute.returnTrackId =
        collectionProject.tracks[1].id;
    collectionProject.reampRoutes.push_back(collectedRoute);
    studio::ToneSnapshot collectedSnapshot;
    collectedSnapshot.reampRouteId = collectedRoute.id;
    collectedSnapshot.sourceTrackId =
        collectedRoute.sourceTrackId;
    collectedSnapshot.returnTrackId =
        collectedRoute.returnTrackId;
    collectedSnapshot.sourceFingerprint = "fixture-source";
    collectedSnapshot.chainFingerprint = "fixture-chain";
    collectedSnapshot.renderFile =
        toneRenderFile.getRelativePathFrom(sourcePackage);
    collectedSnapshot.renderHash =
        juce::SHA256(toneRenderFile).toHexString();
    collectionProject.toneSnapshots.push_back(collectedSnapshot);
    const auto portablePackage =
        renderDirectory.getChildFile("Portable.studioduo");
    juce::String collectionError;
    const auto collectionReport =
        studio::ProjectCollectionService::savePortableCopy(
            collectionProject,
            sourcePackage,
            portablePackage,
            collectionError);
    const auto portableSaveError = collectionError;
    std::optional<studio::ProjectCollectionReport> validationReport;
    if (collectionReport.has_value())
        validationReport =
            studio::ProjectCollectionService::validatePortableCopy(
                portablePackage,
                collectionError);
    const auto portableManifest =
        juce::JSON::parse(
            portablePackage.getChildFile("manifest.json")
                .loadFileAsString());
    const auto activeSession = portableManifest.getDynamicObject() != nullptr
        ? portableManifest.getDynamicObject()
              ->getProperty("activeSession")
              .toString()
        : juce::String();
    const auto portableSessionText =
        portablePackage.getChildFile(activeSession)
            .loadFileAsString();
    const auto portableProject = studio::ProjectFile::load(
        portablePackage,
        collectionError);

    auto repairProject = collectionProject;
    repairProject.tracks.front().clips.front().sourceFile =
        renderDirectory.getChildFile("missing.wav");
    const auto repairReport =
        studio::ProjectCollectionService::repairMissingResources(
            repairProject,
            { renderDirectory },
            collectionError,
            sourcePackage);
    const auto portableClipExists = portableProject.has_value()
        && portableProject->tracks.front()
               .clips.front()
               .sourceFile.existsAsFile();
    const auto portableMasterExists = portableProject.has_value()
        && portableProject->mastering.tracks.front()
               .selectedSource()
               ->file.existsAsFile();
    const auto portablePluginStateExists =
        portablePackage.getChildFile(collectedInsert.stateFile)
            .existsAsFile();
    const auto literalMetadataPreserved = portableProject.has_value()
        && portableProject->metadata.comment
            == "${PROJECT_DIR}/../literal metadata";
    const auto portableToneRenderExists =
        portableProject.has_value()
        && juce::File(
               portableProject->toneSnapshots.front().renderFile)
               .existsAsFile();
    const auto hasPortableToken =
        portableSessionText.contains("${PROJECT_DIR}/media/");
    const auto hasAbsolutePackage =
        portableSessionText.contains(
            portablePackage.getFullPathName());
    const auto repairedByHash =
        repairProject.tracks.front()
            .clips.front()
            .sourceFile.existsAsFile()
        && juce::SHA256(
               repairProject.tracks.front()
                   .clips.front()
                   .sourceFile)
                   .toHexString()
            == collectedClip.sourceHash;
    expect(collectionReport.has_value()
               && collectionReport->missingResources == 0
               && validationReport.has_value()
               && validationReport->invalidResources == 0
               && hasPortableToken
               && !hasAbsolutePackage
               && portableClipExists
               && portableMasterExists
               && portablePluginStateExists
               && literalMetadataPreserved
               && portableToneRenderExists
               && repairReport.repairedResources == 1
               && repairedByHash,
           (("Portable copies collect and verify hashed media with relocatable paths, and repair missing files by content hash: "
             + portableSaveError
             + " "
             + collectionError)
               + " collection="
               + (collectionReport.has_value() ? "1" : "0")
               + "/"
               + juce::String(collectionReport.has_value()
                                  ? collectionReport->missingResources
                                  : -1)
               + " validation="
               + (validationReport.has_value() ? "1" : "0")
               + "/"
               + juce::String(validationReport.has_value()
                                  ? validationReport->invalidResources
                                  : -1)
               + " token="
               + (hasPortableToken ? "1" : "0")
               + " absolute="
               + (hasAbsolutePackage ? "1" : "0")
               + " clip="
               + (portableClipExists ? "1" : "0")
               + " master="
               + (portableMasterExists ? "1" : "0")
               + " plugin="
               + (portablePluginStateExists ? "1" : "0")
               + " metadata="
               + (literalMetadataPreserved ? "1" : "0")
               + " tone="
               + (portableToneRenderExists ? "1" : "0")
               + " repaired="
               + juce::String(repairReport.repairedResources)
               + "/"
               + (repairedByHash ? "1" : "0"))
               .toRawUTF8());

    juce::String tamperError;
    std::optional<studio::ProjectCollectionReport>
        tamperedValidation;
    auto tamperedSession = juce::JSON::parse(portableSessionText);
    if (auto* tamperedObject = tamperedSession.getDynamicObject())
    {
        if (auto* tamperedTracks =
                tamperedObject->getProperty("tracks").getArray();
            tamperedTracks != nullptr
            && !tamperedTracks->isEmpty())
        {
            auto* firstTrackObject =
                tamperedTracks->getReference(0).getDynamicObject();
            auto* clips = firstTrackObject != nullptr
                ? firstTrackObject->getProperty("clips").getArray()
                : nullptr;
            if (clips != nullptr && !clips->isEmpty())
            {
                clips->getReference(0)
                    .getDynamicObject()
                    ->setProperty(
                        "sourceHash",
                        juce::String::repeatedString("d", 64));
                portablePackage.getChildFile(activeSession)
                    .replaceWithText(
                        juce::JSON::toString(
                            tamperedSession,
                            true));
                tamperedValidation =
                    studio::ProjectCollectionService::
                        validatePortableCopy(
                            portablePackage,
                            tamperError);
            }
        }
    }
    expect(tamperedValidation.has_value()
               && tamperedValidation->invalidResources > 0,
           "Portable validation cross-checks manifest hashes against project references.");

    const auto legacyRoot =
        renderDirectory.getChildFile("legacy-repair");
    const auto legacyA =
        legacyRoot.getChildFile("a").getChildFile("legacy.wav");
    const auto legacyB =
        legacyRoot.getChildFile("b").getChildFile("legacy.wav");
    legacyA.getParentDirectory().createDirectory();
    legacyB.getParentDirectory().createDirectory();
    expect(writeConstantWave(legacyA, 0.1f, sampleRate, 0.1)
               && writeConstantWave(legacyB, 0.2f, sampleRate, 0.1),
           "Ambiguous legacy repair fixtures can be written.");
    auto ambiguousProject = studio::Project::createDefault();
    auto ambiguousClip = collectedClip;
    ambiguousClip.sourceFile =
        renderDirectory.getChildFile("missing").getChildFile("legacy.wav");
    ambiguousClip.sourceHash.clear();
    ambiguousProject.tracks.front().clips.push_back(ambiguousClip);
    juce::String ambiguousError;
    const auto ambiguousReport =
        studio::ProjectCollectionService::repairMissingResources(
            ambiguousProject,
            { legacyRoot },
            ambiguousError);
    expect(ambiguousReport.repairedResources == 0
               && ambiguousReport.missingResources == 1
               && ambiguousError.containsIgnoreCase("missing"),
           "Hashless repair refuses ambiguous same-named files.");

    const auto aliasSource =
        renderDirectory.getChildFile("alias-source.wav");
    expect(writeSineWave(
               aliasSource,
               30000.0,
               0.8f,
               96000.0,
               0.2),
           "Sample-rate conversion fixture can be written.");
    studio::MasteringAlbum aliasAlbum;
    studio::MasteringTrack aliasTrack;
    studio::MasteringSourceMix aliasMix;
    aliasMix.file = aliasSource;
    aliasMix.sourceHash =
        juce::SHA256(aliasSource).toHexString();
    aliasMix.durationSeconds = 0.2;
    aliasTrack.sources = { aliasMix };
    aliasTrack.selectedSourceId = aliasMix.id;
    aliasAlbum.tracks = { aliasTrack };
    juce::String aliasError;
    const auto downsampled = studio::MasteringEngine::renderAlbum(
        aliasAlbum,
        44100.0,
        aliasError);
    expect(downsampled.has_value()
               && downsampled->audio.getNumSamples() > 4000
               && downsampled->audio.getMagnitude(
                      0,
                      2000,
                      downsampled->audio.getNumSamples() - 4000)
                      < 0.01f,
           ("Band-limited mastering SRC rejects frequencies above the destination Nyquist limit: "
            + aliasError)
               .toRawUTF8());

    auto commandProject = studio::Project::createDefault();
    studio::CommandStack masteringCommands;
    auto editedAlbum = commandProject.mastering;
    editedAlbum.title = "Command Album";
    juce::String commandError;
    const auto commandApplied = masteringCommands.perform(
        std::make_unique<studio::SetMasteringAlbumCommand>(
            commandProject.mastering,
            editedAlbum),
        commandProject,
        commandError);
    const auto commandUndone = masteringCommands.undo(commandProject);
    const auto commandRedone =
        masteringCommands.redo(commandProject, commandError);
    expect(commandApplied
               && commandUndone
               && commandRedone
               && commandProject.mastering.title == "Command Album",
           ("Mastering album edits participate in project undo and redo: "
            + commandError)
               .toRawUTF8());
    renderDirectory.deleteRecursively();
}
