#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"

#include <cmath>

namespace
{
bool closeTo(double left, double right, double tolerance = 0.000001)
{
    return std::abs(left - right) <= tolerance;
}

juce::File createLoopSource()
{
    const auto sourceFile = juce::File::getSpecialLocation(
                                juce::File::tempDirectory)
                                .getNonexistentChildFile(
                                    "StudioDuoTransport",
                                    ".wav",
                                    false);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream =
        sourceFile.createOutputStream();
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(1)
            .withBitsPerSample(24));
    juce::AudioBuffer<float> source(1, 100);
    for (int sample = 0; sample < source.getNumSamples(); ++sample)
        source.setSample(0, sample, 0.2f);
    expect(writer != nullptr
               && writer->writeFromAudioSampleBuffer(
                   source,
                   0,
                   source.getNumSamples()),
           "Transport loop source can be written.");
    if (writer != nullptr)
        writer->flush();
    return sourceFile;
}
}

void transportTests()
{
    auto project = studio::Project::createDefault();
    project.name = "Transport verification";
    project.tempo = 110.0;
    project.timeSignatureNumerator = 5;
    project.timeSignatureDenominator = 4;
    project.tempoChanges = {
        { 0.0, 110.0, true },
        { 3.0, 170.0, false }
    };
    project.meterChanges = {
        { 0.0, 5, 4 },
        { 6.0, 7, 8 }
    };
    project.metronomeEnabled = true;
    project.metronomeSubdivision = 3;
    project.metronomeOutputChannel = 2;
    project.metronomeLevel = 0.5f;
    project.metronomeAccentLevel = 0.9f;
    project.punchEnabled = true;
    project.punchInSeconds = 8.0;
    project.punchOutSeconds = 10.0;
    project.countInBars = 2;
    project.preRollSeconds = 0.5;
    project.postRollSeconds = 1.0;
    project.loopEnabled = true;
    project.loopStartSeconds = 2.0;
    project.loopEndSeconds = 12.0;

    juce::String error;
    expect(project.validateTransport(error), error.toRawUTF8());
    expect(closeTo(project.timelineEndSeconds(), 12.0),
           "Timeline extent includes transport ranges beyond existing clips.");

    const auto package = juce::File::getSpecialLocation(
                             juce::File::tempDirectory)
                             .getNonexistentChildFile(
                                 "StudioDuoTransport",
                                 ".studioduo",
                                 false);
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "Transport project can be saved.");
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value(), error.toRawUTF8());
    expect(loaded.has_value()
               && loaded->tempoChanges == project.tempoChanges
               && loaded->meterChanges == project.meterChanges
               && loaded->metronomeSubdivision
                      == project.metronomeSubdivision
               && loaded->metronomeOutputChannel
                      == project.metronomeOutputChannel
               && closeTo(loaded->metronomeLevel,
                          project.metronomeLevel)
               && closeTo(loaded->metronomeAccentLevel,
                          project.metronomeAccentLevel)
               && loaded->punchEnabled
               && closeTo(loaded->punchInSeconds,
                          project.punchInSeconds)
               && closeTo(loaded->punchOutSeconds,
                          project.punchOutSeconds)
               && loaded->countInBars == project.countInBars
               && closeTo(loaded->preRollSeconds,
                          project.preRollSeconds)
               && closeTo(loaded->postRollSeconds,
                          project.postRollSeconds)
               && loaded->loopEnabled
               && closeTo(loaded->loopStartSeconds,
                          project.loopStartSeconds)
               && closeTo(loaded->loopEndSeconds,
                          project.loopEndSeconds),
           "Tempo, meter, metronome, punch, roll, and loop data survive save and reopen.");

    auto duplicateTempo = project;
    duplicateTempo.tempoChanges.push_back({ 3.0, 180.0, false });
    error.clear();
    expect(!duplicateTempo.validateTransport(error)
               && error.containsIgnoreCase("unique"),
           "Transport validation rejects duplicate tempo-map positions.");

    auto invalidValue = project.toVar();
    invalidValue.getDynamicObject()->setProperty(
        "punchOutSeconds",
        project.punchInSeconds);
    error.clear();
    expect(!studio::Project::fromVar(invalidValue, error).has_value()
               && error.containsIgnoreCase("punch"),
           "Project loading rejects invalid punch ranges instead of normalizing them.");

    auto invalidState =
        studio::ProjectTransportState::fromProject(project);
    invalidState.metronomeSubdivision = 9;
    studio::CommandStack commands;
    error.clear();
    expect(!commands.perform(
               std::make_unique<studio::SetProjectTransportCommand>(
                   studio::ProjectTransportState::fromProject(project),
                   invalidState),
               project,
               error)
               && error.containsIgnoreCase("metronome"),
           "Undoable transport edits reject invalid metronome settings.");

    auto punchProject = studio::Project::createDefault();
    punchProject.tempo = 120.0;
    punchProject.punchEnabled = true;
    punchProject.punchInSeconds = 8.0;
    punchProject.punchOutSeconds = 10.0;
    punchProject.countInBars = 1;
    punchProject.preRollSeconds = 0.5;
    punchProject.postRollSeconds = 1.0;
    const auto punchPlan = punchProject.recordingPlan(9.0);
    expect(closeTo(punchPlan.transportStartSeconds, 5.5)
               && closeTo(punchPlan.captureStartSeconds, 8.0)
               && closeTo(punchPlan.captureEndSeconds, 10.0)
               && closeTo(punchPlan.transportEndSeconds, 11.0),
           "Count-in, pre-roll, punch, and post-roll produce exact transport boundaries.");
    punchProject.loopEnabled = true;
    punchProject.loopStartSeconds = 4.0;
    punchProject.loopEndSeconds = 12.0;
    expect(!punchProject.recordingPlan(9.0).loopEnabled,
           "Punch recording takes priority over loop recording when both are enabled.");

    const auto enteringPunch =
        studio::recordingCaptureRange(90, 20, 100, 200);
    const auto leavingPunch =
        studio::recordingCaptureRange(190, 20, 100, 200);
    const auto afterPunch =
        studio::recordingCaptureRange(210, 20, 100, 200);
    expect(enteringPunch.sourceOffset == 10
               && enteringPunch.samples == 10
               && leavingPunch.sourceOffset == 0
               && leavingPunch.samples == 10
               && afterPunch.samples == 0,
           "Punch capture clips callback blocks at sample-accurate in/out boundaries.");

    studio::RecordingPlan invalidPlan;
    invalidPlan.transportStartSeconds = 2.0;
    invalidPlan.captureStartSeconds = 1.0;
    studio::StudioAudioEngine invalidPlanEngine;
    const auto invalidPlanResult =
        invalidPlanEngine.startRecording({}, invalidPlan);
    expect(invalidPlanResult.failed()
               && invalidPlanResult.getErrorMessage()
                      .containsIgnoreCase("transport range"),
           "Recording plans are validated before devices or files are touched.");

    auto clickProject = studio::Project::createDefault();
    clickProject.metronomeSubdivision = 3;
    clickProject.metronomeOutputChannel = 2;
    clickProject.metronomeLevel = 0.25f;
    clickProject.metronomeAccentLevel = 1.0f;
    clickProject.meterChanges = {
        { 0.25, 3, 4 }
    };
    studio::StudioAudioEngine clickEngine;
    expect(clickEngine.updateProject(clickProject).wasOk(),
           "Metronome project publishes for real-time playback.");
    clickEngine.seekSeconds(0.0);
    clickEngine.play();
    const auto clickBlock =
        clickEngine.renderActiveBlockForTesting(14000, 4);
    const auto firstAccent = clickBlock.getMagnitude(2, 0, 800);
    const auto subdivisionClick =
        clickBlock.getMagnitude(2, 8000, 800);
    const auto meterAccent =
        clickBlock.getMagnitude(2, 12000, 800);
    expect(clickBlock.getMagnitude(0, 0, 14000) < 0.000001f
               && clickBlock.getMagnitude(1, 0, 14000) < 0.000001f
               && firstAccent > 0.1f
               && subdivisionClick > 0.01f
               && firstAccent > subdivisionClick * 2.0f
               && meterAccent > subdivisionClick * 2.0f
               && clickBlock.getMagnitude(3, 0, 14000) > 0.1f,
           "Metronome routing, triplet subdivisions, and meter-change accents render on the selected output pair.");

    juce::AudioBuffer<float> exportedClick;
    expect(clickEngine.renderToBuffer(
               clickProject,
               exportedClick,
               48000.0)
               .wasOk()
               && exportedClick.getMagnitude(
                      0,
                      0,
                      exportedClick.getNumSamples())
                      < 0.000001f
               && exportedClick.getMagnitude(
                      1,
                      0,
                      exportedClick.getNumSamples())
                      < 0.000001f,
           "Metronome routing stays out of final buffer renders.");

    const auto loopSource = createLoopSource();
    auto loopProject = studio::Project::createDefault();
    loopProject.metronomeEnabled = false;
    loopProject.loopEnabled = true;
    loopProject.loopStartSeconds = 0.0;
    loopProject.loopEndSeconds = 100.0 / 48000.0;
    studio::AudioClip loopClip;
    loopClip.sourceFile = loopSource;
    loopClip.durationSeconds = loopProject.loopEndSeconds;
    loopClip.sourceLengthSeconds = loopClip.durationSeconds;
    loopClip.sourceRangeEndSeconds = loopClip.durationSeconds;
    loopProject.tracks.front().clips.push_back(loopClip);

    studio::StudioAudioEngine loopEngine;
    juce::AudioBuffer<float> offlineLoop;
    expect(loopEngine.renderToBuffer(
               loopProject,
               offlineLoop,
               48000.0)
               .wasOk()
               && offlineLoop.getSample(0, 25) > 0.19f
               && offlineLoop.getSample(0, 125) > 0.19f
               && offlineLoop.getSample(0, 225) > 0.19f,
           "Plugin-free buffer rendering follows the same loop boundaries as real-time playback.");

    expect(loopEngine.updateProject(loopProject).wasOk(),
           "Loop project publishes for real-time playback.");
    loopEngine.seekSeconds(0.0);
    loopEngine.play();
    const auto realtimeLoop =
        loopEngine.renderActiveBlockForTesting(250);
    expect(realtimeLoop.getSample(0, 25) > 0.19f
               && realtimeLoop.getSample(0, 125) > 0.19f
               && realtimeLoop.getSample(0, 225) > 0.19f,
           "Real-time playback wraps audio at mid-block loop boundaries.");

    auto extendedLoop = studio::Project::createDefault();
    extendedLoop.metronomeEnabled = false;
    extendedLoop.loopEnabled = true;
    extendedLoop.loopStartSeconds = 7.99;
    extendedLoop.loopEndSeconds = 8.01;
    studio::StudioAudioEngine extendedLoopEngine;
    expect(extendedLoopEngine.updateProject(extendedLoop).wasOk(),
           "Extended loop project publishes.");
    extendedLoopEngine.seekSeconds(7.999);
    extendedLoopEngine.play();
    extendedLoopEngine.processActiveBlockForTesting(200);
    expect(extendedLoopEngine.isPlaying()
               && extendedLoopEngine.positionSeconds() > 8.0,
           "Loop playback can cross the current content end before wrapping.");

    loopSource.deleteFile();
    package.deleteRecursively();
}
