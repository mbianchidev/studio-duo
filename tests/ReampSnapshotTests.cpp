#include "TestHarness.h"
#include "TestSuites.h"

#include "reamp/ReampSnapshotService.h"
#include "model/ProjectCommands.h"

void reampSnapshotTests()
{
    auto project = studio::Project::createDefault();
    auto& source = project.tracks.front();
    auto& returnTrack = project.tracks[1];
    studio::AudioClip clip;
    clip.name = "DI";
    source.clips.push_back(clip);

    studio::PluginInsert insert;
    insert.pluginIdentifier = "studio.device.gain";
    insert.name = "Gain";
    insert.format = "Studio Duo";
    insert.bundledDevice = true;
    insert.stateHash = "state-a";
    returnTrack.inserts.push_back(insert);
    returnTrack.volumeDecibels = -3.0f;

    studio::ReampRoute route;
    route.type = studio::TonePathType::plugin;
    route.sourceTrackId = source.id;
    route.returnTrackId = returnTrack.id;
    project.reampRoutes.push_back(route);

    studio::PluginInsert sourceInsert;
    sourceInsert.pluginIdentifier = "studio.device.polarity";
    sourceInsert.name = "Source polarity";
    sourceInsert.format = "Studio Duo";
    sourceInsert.bundledDevice = true;
    sourceInsert.stateHash = "source-state-a";
    source.inserts.push_back(sourceInsert);

    studio::AutomationLane sourceAutomation;
    sourceAutomation.name = "DI volume";
    sourceAutomation.target.type =
        studio::AutomationTargetType::trackVolume;
    sourceAutomation.target.trackId = source.id;
    sourceAutomation.timebase =
        studio::AutomationTimebase::beats;
    sourceAutomation.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.5
    });
    project.automationLanes.push_back(sourceAutomation);
    studio::AutomationLane returnAutomation;
    returnAutomation.name = "Tone volume";
    returnAutomation.target.type =
        studio::AutomationTargetType::trackVolume;
    returnAutomation.target.trackId = returnTrack.id;
    returnAutomation.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.5
    });
    project.automationLanes.push_back(returnAutomation);

    juce::String error;
    const auto snapshot = studio::ReampSnapshotService::capture(
        project,
        route.id,
        "Heavy",
        error);
    expect(snapshot.has_value(), error.toRawUTF8());
    expect(snapshot.has_value()
               && snapshot->sourceFingerprint.isNotEmpty()
               && snapshot->chainFingerprint.isNotEmpty()
               && studio::ReampSnapshotService::staleReason(
                      project,
                      *snapshot)
                      .isEmpty(),
           "Fresh reamp snapshots capture deterministic source and chain fingerprints.");

    source.clips.front().gainDecibels = 1.0f;
    expect(studio::ReampSnapshotService::staleReason(project, *snapshot)
               .containsIgnoreCase("source"),
           "DI edits mark a tone render stale.");
    source.clips.front().gainDecibels = 0.0f;

    source.volumeDecibels = -6.0f;
    expect(studio::ReampSnapshotService::staleReason(project, *snapshot)
               .containsIgnoreCase("source"),
           "DI mixer changes mark a tone render stale.");
    source.volumeDecibels = 0.0f;

    source.inserts.front().stateHash = "source-state-b";
    expect(studio::ReampSnapshotService::staleReason(project, *snapshot)
               .containsIgnoreCase("source"),
           "DI processor changes mark a tone render stale.");
    source.inserts.front().stateHash = "source-state-a";

    project.automationLanes.front().points.front().value = 0.75;
    expect(studio::ReampSnapshotService::staleReason(project, *snapshot)
               .containsIgnoreCase("source"),
           "DI automation changes mark a tone render stale.");
    project.automationLanes.front().points.front().value = 0.5;

    project.tempo = 90.0;
    expect(studio::ReampSnapshotService::staleReason(project, *snapshot)
               .containsIgnoreCase("source"),
           "Tempo changes mark beat-based DI automation stale.");
    project.tempo = 120.0;

    returnTrack.volumeDecibels = -12.0f;
    returnTrack.inserts.front().stateHash = "state-b";

    studio::CommandStack history;
    auto matchedSnapshot = *snapshot;
    matchedSnapshot.comparisonGainDecibels = 2.5f;
    expect(history.perform(
               std::make_unique<studio::RecallToneSnapshotCommand>(
                   matchedSnapshot,
                   studio::ToneSnapshotRecallMode::levelMatched),
               project,
               error),
           error.toRawUTF8());
    const auto matchedReturnLane = std::find_if(
        project.automationLanes.cbegin(),
        project.automationLanes.cend(),
        [&returnTrack](const auto& lane)
        {
            return lane.target.trackId == returnTrack.id
                && lane.target.type
                    == studio::AutomationTargetType::trackVolume;
        });
    expect(std::abs(returnTrack.volumeDecibels + 3.0f) < 0.0001f
               && returnTrack.inserts.front().stateHash == "state-a"
               && matchedReturnLane != project.automationLanes.cend()
               && std::abs(
                      matchedReturnLane->trimOffset
                      - 2.5 / 72.0)
                      < 0.0001,
           "Level-matched tone recall trims automated return faders.");
    expect(studio::ReampSnapshotService::staleReason(
               project,
               matchedSnapshot)
               .isEmpty(),
           "Activating a level-matched snapshot does not make its render stale.");
    expect(history.undo(project)
               && std::abs(returnTrack.volumeDecibels + 12.0f) < 0.0001f
               && returnTrack.inserts.front().stateHash == "state-b",
           "Tone snapshot recall is undoable.");
    expect(history.perform(
               std::make_unique<studio::RecallToneSnapshotCommand>(
                   matchedSnapshot),
               project,
               error),
           error.toRawUTF8());
    expect(std::abs(returnTrack.volumeDecibels + 3.0f) < 0.0001f,
           "Render recall retains the captured level without comparison trim.");
    const auto rawReturnLane = std::find_if(
        project.automationLanes.cbegin(),
        project.automationLanes.cend(),
        [&returnTrack](const auto& lane)
        {
            return lane.target.trackId == returnTrack.id
                && lane.target.type
                    == studio::AutomationTargetType::trackVolume;
        });
    expect(rawReturnLane != project.automationLanes.cend()
               && std::abs(rawReturnLane->trimOffset) < 0.0001,
           "Raw render recall does not apply the comparison trim.");
    expect(studio::ReampSnapshotService::staleReason(
               project,
               matchedSnapshot)
               .isEmpty(),
           "Activating a captured-level snapshot remains fresh.");
    expect(history.undo(project),
           "Raw tone snapshot recall remains undoable.");

    const auto mixerSnapshot = studio::MixerSnapshotService::capture(
        project,
        { source.id },
        "DI only",
        error);
    expect(mixerSnapshot.has_value(), error.toRawUTF8());
    project.toneSnapshots.push_back(*snapshot);
    project.mixerSnapshots.push_back(*mixerSnapshot);
    studio::RenderReport persistedReport;
    persistedReport.scope = "reamp:" + snapshot->id;
    persistedReport.status = "success";
    persistedReport.createdAt = "2026-09-01T00:00:00Z";
    project.renderReports.push_back(persistedReport);
    const auto roundTrip = studio::Project::fromVar(project.toVar(), error);
    expect(roundTrip.has_value()
               && roundTrip->toneSnapshots.size() == 1
               && roundTrip->mixerSnapshots.size() == 1
               && roundTrip->renderReports.size() == 1,
           "Tone snapshots, mixer snapshots, and render reports persist.");
    const auto untouchedVolume = returnTrack.volumeDecibels;
    source.volumeDecibels = -18.0f;
    expect(history.perform(
               std::make_unique<studio::RecallMixerSnapshotCommand>(
                   *mixerSnapshot),
               project,
               error),
           error.toRawUTF8());
    expect(std::abs(source.volumeDecibels) < 0.0001f
               && std::abs(returnTrack.volumeDecibels - untouchedVolume)
                      < 0.0001f,
           "Scoped mixer recall changes only captured tracks.");
}
