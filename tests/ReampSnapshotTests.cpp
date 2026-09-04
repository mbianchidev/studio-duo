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
    returnTrack.volumeDecibels = -12.0f;
    returnTrack.inserts.front().stateHash = "state-b";

    studio::CommandStack history;
    expect(history.perform(
               std::make_unique<studio::RecallToneSnapshotCommand>(
                   *snapshot),
               project,
               error),
           error.toRawUTF8());
    expect(std::abs(returnTrack.volumeDecibels + 3.0f) < 0.0001f
               && returnTrack.inserts.front().stateHash == "state-a",
           "Tone snapshot recall restores level and processor state.");
    expect(history.undo(project)
               && std::abs(returnTrack.volumeDecibels + 12.0f) < 0.0001f
               && returnTrack.inserts.front().stateHash == "state-b",
           "Tone snapshot recall is undoable.");

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
