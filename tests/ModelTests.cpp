#include "model/ProjectCommands.h"
#include "audio/AudioAnalysis.h"
#include "audio/RecordingWaveform.h"
#include "plugin_host/PluginCatalog.h"
#include "plugin_host/PluginBridgeProtocol.h"
#include "project_io/ProjectFile.h"
#include "TestHarness.h"
#include "TestSuites.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
void serializationRoundTrip()
{
    auto project = studio::Project::createDefault();
    project.name = "Serialization";
    project.tempo = 168.0;
    project.tempoChanges = {
        { 0.0, 168.0, true },
        { 4.0, 180.0, false }
    };
    project.meterChanges = {
        { 0.0, 4, 4 },
        { 8.0, 7, 8 }
    };
    project.metronomeEnabled = false;
    project.metronomeSubdivision = 2;
    project.metronomeOutputChannel = 2;
    project.punchEnabled = true;
    project.punchInSeconds = 2.0;
    project.punchOutSeconds = 6.0;
    project.countInBars = 2;
    project.preRollSeconds = 1.0;
    project.postRollSeconds = 1.5;
    studio::EditGroup editGroup;
    editGroup.name = "Rhythm guitars";
    editGroup.trackIds = {
        project.tracks[0].id,
        project.tracks[1].id
    };
    editGroup.timingReferenceTrackId = project.tracks[0].id;
    editGroup.quantizeStrength = 0.75;
    editGroup.protectedAnchorsSeconds = { 1.0, 4.0 };
    project.editGroups.push_back(editGroup);
    studio::ReampRoute reampRoute;
    reampRoute.name = "Lead hardware";
    reampRoute.sourceTrackId = project.tracks[0].id;
    reampRoute.returnTrackId = project.tracks[1].id;
    const auto reampReturnTrackId = reampRoute.returnTrackId;
    reampRoute.outputChannel = 2;
    reampRoute.inputChannel = 1;
    reampRoute.latencySamples = 256;
    reampRoute.alignmentOffsetSamples = -8;
    reampRoute.polarityInverted = true;
    project.reampRoutes.push_back(reampRoute);
    project.tracks.front().inputChannel = 1;
    project.tracks.front().stereoInput = true;
    project.tracks.front().inputMonitoring = true;

    studio::AudioClip clip;
    clip.name = "DI";
    clip.startSeconds = 1.5;
    clip.durationSeconds = 3.25;
    clip.sourceLengthSeconds = 4.0;
    clip.sourceRangeEndSeconds = 3.25;
    clip.stretchMode = studio::StretchMode::drums;
    clip.playbackRate = 1.25;
    clip.fadeInSeconds = 0.25;
    clip.fadeOutSeconds = 0.5;
    clip.polarityInverted = true;
    clip.reversed = true;
    clip.warpMarkers.push_back({ 1.0, 1.5 });
    clip.transientSourceSeconds = { 0.5, 1.5 };
    project.tracks.front().clips.push_back(clip);

    studio::PluginInsert insert;
    insert.pluginIdentifier = "VST3-test-id";
    insert.name = "Test Gate";
    insert.manufacturer = "Studio Duo";
    insert.format = "VST3";
    insert.latencySamples = 128;
    insert.tailSeconds = 1.5;
    project.tracks.front().inserts.push_back(insert);

    studio::Track version;
    version.name = "v1";
    version.parentTrackId = project.tracks.front().id;
    version.versionNumber = 1;
    const auto versionId = version.id;
    project.tracks.insert(project.tracks.begin() + 1, version);
    project.tracks.front().activeTakeTrackId = versionId;
    project.tracks.front().compRegions.push_back({
        juce::Uuid().toString(),
        versionId,
        1.5,
        1.0
    });
    studio::Track bus;
    bus.name = "Guitar Bus";
    bus.type = studio::TrackType::bus;
    const auto busId = bus.id;
    project.tracks.insert(project.tracks.end() - 1, bus);
    project.tracks.front().outputTrackId = busId;

    juce::String error;
    const auto decoded = studio::Project::fromVar(project.toVar(), error);

    expect(decoded.has_value(), error.toRawUTF8());
    expect(decoded.has_value() && decoded->name == project.name, "Project name survives serialization.");
    expect(decoded.has_value() && std::abs(decoded->tempo - project.tempo) < 0.0001,
           "Tempo survives serialization.");
    expect(decoded.has_value()
               && decoded->tempoChanges.size() == 2
               && decoded->tempoChanges[0].rampToNext
               && decoded->meterChanges.size() == 2
               && decoded->meterChanges[1].numerator == 7,
           "Tempo and meter maps survive serialization.");
    expect(decoded.has_value()
               && !decoded->metronomeEnabled
               && decoded->metronomeSubdivision == 2
               && decoded->metronomeOutputChannel == 2
               && decoded->punchEnabled
               && std::abs(decoded->punchInSeconds - 2.0) < 0.0001
               && std::abs(decoded->punchOutSeconds - 6.0) < 0.0001
               && decoded->countInBars == 2
               && std::abs(decoded->preRollSeconds - 1.0) < 0.0001
               && std::abs(decoded->postRollSeconds - 1.5) < 0.0001,
           "Recording transport settings survive serialization.");
    expect(decoded.has_value()
               && decoded->editGroups.size() == 1
               && decoded->editGroups.front().trackIds.size() == 2
               && decoded->editGroups.front().timingReferenceTrackId
                      == decoded->tracks[0].id
               && std::abs(decoded->editGroups.front().quantizeStrength - 0.75) < 0.0001
               && decoded->editGroups.front().protectedAnchorsSeconds.size() == 2,
           "Linked edit groups survive serialization.");
    expect(decoded.has_value()
               && decoded->reampRoutes.size() == 1
               && decoded->reampRoutes.front().sourceTrackId
                      == decoded->tracks[0].id
               && decoded->reampRoutes.front().returnTrackId
                      == reampReturnTrackId
               && decoded->reampRoutes.front().latencySamples == 256
               && decoded->reampRoutes.front().alignmentOffsetSamples == -8
               && decoded->reampRoutes.front().polarityInverted,
           "Hardware reamp relationships survive serialization.");
    expect(decoded.has_value() && decoded->tracks.front().clips.size() == 1,
           "Clips survive serialization.");
    expect(decoded.has_value()
               && std::abs(decoded->tracks.front().clips.front().sourceRangeEnd() - 3.25) < 0.0001,
           "Clip recovery bounds survive serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().clips.front().stretchMode
                      == studio::StretchMode::drums
               && std::abs(decoded->tracks.front().clips.front().playbackRate - 1.25) < 0.0001
               && decoded->tracks.front().clips.front().warpMarkers.size() == 1
               && decoded->tracks.front().clips.front().transientSourceSeconds.size() == 2
               && decoded->tracks.front().clips.front().polarityInverted
               && decoded->tracks.front().clips.front().reversed,
           "Clip stretch, warp, transient, fade, and polarity data survive serialization.");
    expect(decoded.has_value() && decoded->tracks.front().inserts.size() == 1,
           "Plugin inserts survive serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().inputChannel == 1
               && decoded->tracks.front().stereoInput
               && decoded->tracks.front().inputMonitoring,
           "Track input routing survives serialization.");
    expect(decoded.has_value()
               && decoded->tracks[1].parentTrackId == decoded->tracks.front().id
               && decoded->tracks[1].versionNumber == 1,
           "Grouped recording versions survive serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().activeTakeTrackId == decoded->tracks[1].id
               && decoded->tracks.front().compRegions.size() == 1
               && decoded->tracks.front().compRegions.front().sourceTrackId
                      == decoded->tracks[1].id,
           "Active playlists and comp regions survive serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().inserts.front().bridgeMode
                      == studio::PluginBridgeMode::sandboxed,
           "Plugin bridge mode survives serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().inserts.front().latencySamples == 128
               && std::abs(decoded->tracks.front().inserts.front().tailSeconds - 1.5) < 0.0001,
           "Plugin latency and tail metadata survive serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().outputTrackId == busId
               && decoded->findTrack(busId) != nullptr
               && decoded->findTrack(busId)->type == studio::TrackType::bus,
           "Bus output routing survives serialization.");
}

void legacyProjectMigration()
{
    auto legacy = studio::Project::createDefault().toVar();
    auto* object = legacy.getDynamicObject();
    object->setProperty("formatVersion", 1);
    if (auto* tracks = object->getProperty("tracks").getArray())
        for (auto& track : *tracks)
            if (auto* trackObject = track.getDynamicObject())
                trackObject->removeProperty("outputTrackId");

    juce::String error;
    const auto migrated = studio::Project::fromVar(legacy, error);
    expect(migrated.has_value(), error.toRawUTF8());
    expect(migrated.has_value()
               && migrated->tracks.front().outputTrackId.isEmpty()
               && migrated->resolvedOutputTrackId(migrated->tracks.front())
                      == migrated->masterTrackId(),
           "Version 1 projects migrate to direct master outputs.");
}

void commandHistory()
{
    auto project = studio::Project::createDefault();
    studio::CommandStack history;
    juce::String error;

    studio::Track extraTrack;
    extraTrack.name = "Added Track";
    extraTrack.armed = true;
    const auto extraTrackId = extraTrack.id;
    expect(history.perform(std::make_unique<studio::AddTrackCommand>(extraTrack), project, error),
           error.toRawUTF8());
    expect(project.findTrack(extraTrackId) != nullptr, "Add track command creates a track.");
    expect(project.tracks[project.tracks.size() - 2].id == extraTrackId,
           "Audio tracks are inserted before the master.");
    expect(history.undo(project), "Add track command can be undone.");
    expect(project.findTrack(extraTrackId) == nullptr, "Undo removes the added track.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.findTrack(extraTrackId) != nullptr, "Redo restores the added track.");

    expect(history.perform(std::make_unique<studio::RenameTrackCommand>(extraTrackId,
                                                                        "Lead Guitar"),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(extraTrackId)->name == "Lead Guitar",
           "Rename track command updates the track label.");
    expect(history.undo(project), "Track rename can be undone.");
    expect(project.findTrack(extraTrackId)->name == "Added Track",
           "Undo restores the previous track label.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.findTrack(extraTrackId)->name == "Lead Guitar",
           "Redo restores the edited track label.");
    error.clear();
    expect(!history.perform(std::make_unique<studio::RenameTrackCommand>(extraTrackId, "   "),
                            project,
                            error),
           "Track names cannot be empty.");

    const auto transportBefore = studio::ProjectTransportState::fromProject(project);
    auto transportAfter = transportBefore;
    transportAfter.tempo = 140.0;
    transportAfter.punchEnabled = true;
    transportAfter.punchInSeconds = 2.0;
    transportAfter.punchOutSeconds = 6.0;
    expect(history.perform(std::make_unique<studio::SetProjectTransportCommand>(
                               transportBefore,
                               transportAfter),
                           project,
                           error),
           error.toRawUTF8());
    expect(std::abs(project.tempo - 140.0) < 0.0001 && project.punchEnabled,
           "Transport command applies project recording settings.");
    expect(history.undo(project), "Transport changes can be undone.");
    expect(std::abs(project.tempo - transportBefore.tempo) < 0.0001
               && !project.punchEnabled,
           "Undo restores project recording settings.");
    expect(history.redo(project, error), error.toRawUTF8());

    studio::Track versionTrack;
    versionTrack.name = "v1";
    versionTrack.parentTrackId = extraTrackId;
    versionTrack.versionNumber = 1;
    const auto versionTrackId = versionTrack.id;
    expect(history.perform(std::make_unique<studio::AddTrackCommand>(versionTrack),
                           project,
                           error),
           error.toRawUTF8());
    const auto parentIterator = std::find_if(project.tracks.cbegin(),
                                             project.tracks.cend(),
                                             [&extraTrackId](const auto& track)
    {
        return track.id == extraTrackId;
    });
    expect(parentIterator + 1 != project.tracks.cend()
               && (parentIterator + 1)->id == versionTrackId,
           "Version lanes are inserted directly below their parent.");
    expect(history.perform(std::make_unique<studio::RemoveTrackCommand>(extraTrackId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(extraTrackId) == nullptr
               && project.findTrack(versionTrackId) == nullptr,
           "Removing a parent track removes its grouped versions.");
    expect(history.undo(project), "Removing a track group can be undone.");
    expect(project.findTrack(extraTrackId) != nullptr
               && project.findTrack(versionTrackId) != nullptr,
           "Undo restores the parent and version lanes.");

    auto* restoredParent = project.findTrack(extraTrackId);
    auto* restoredVersion = project.findTrack(versionTrackId);
    restoredParent->volumeDecibels = -4.5f;
    restoredParent->pan = -0.35f;
    restoredParent->inputChannel = 2;
    restoredParent->activeTakeTrackId = versionTrackId;
    studio::AudioClip versionClip;
    versionClip.name = "Version audio";
    const auto sourceVersionClipId = versionClip.id;
    restoredVersion->clips.push_back(versionClip);
    restoredParent->compRegions.push_back({
        juce::Uuid().toString(),
        versionTrackId,
        0.0,
        2.0
    });
    studio::PluginInsert duplicatedInsert;
    duplicatedInsert.pluginIdentifier = "VST3-duplicate-test";
    duplicatedInsert.name = "Duplicate Test";
    restoredParent->inserts.push_back(duplicatedInsert);
    studio::ReampRoute duplicatedRoute;
    duplicatedRoute.name = "Duplicated send";
    duplicatedRoute.type = studio::TonePathType::hardware;
    duplicatedRoute.sourceTrackId = extraTrackId;
    duplicatedRoute.returnTrackId = project.tracks.front().id;
    duplicatedRoute.outputChannel = 2;
    duplicatedRoute.inputChannel = 1;
    project.reampRoutes.push_back(duplicatedRoute);

    auto duplicate = std::make_unique<studio::DuplicateTrackCommand>(extraTrackId);
    auto* duplicatePointer = duplicate.get();
    expect(history.perform(std::move(duplicate), project, error), error.toRawUTF8());
    const auto duplicateId = duplicatePointer->duplicatedTrackId();
    expect(project.findTrack(duplicateId) != nullptr, "Duplicate track command creates a track.");
    expect(project.findTrack(duplicateId)->id != extraTrackId,
           "Duplicated tracks receive a stable independent ID.");
    const auto duplicatedVersion = std::find_if(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [&duplicateId](const auto& track)
        {
            return track.parentTrackId == duplicateId;
        });
    expect(duplicatedVersion != project.tracks.cend()
               && duplicatedVersion->clips.size() == 1
               && duplicatedVersion->clips.front().id != sourceVersionClipId,
           "Duplicating a parent copies every take and audio clip with new IDs.");
    const auto duplicatedVersionId = duplicatedVersion != project.tracks.cend()
        ? duplicatedVersion->id
        : juce::String();
    expect(duplicatedVersionId.isNotEmpty()
               && project.findTrack(duplicateId)->activeTakeTrackId
                      == duplicatedVersionId
               && project.findTrack(duplicateId)->compRegions.size() == 1
               && project.findTrack(duplicateId)->compRegions.front().sourceTrackId
                      == duplicatedVersionId,
           "Duplicating a track remaps playlist and comp references.");
    expect(std::abs(project.findTrack(duplicateId)->volumeDecibels + 4.5f) < 0.0001f
               && std::abs(project.findTrack(duplicateId)->pan + 0.35f) < 0.0001f
               && project.findTrack(duplicateId)->inputChannel == 2
               && project.findTrack(duplicateId)->inserts.size() == 1,
           "Duplicating a track preserves its mix, input, and insert settings.");
    const auto clonedRoute = std::find_if(
        project.reampRoutes.cbegin(),
        project.reampRoutes.cend(),
        [&duplicateId](const auto& route)
        {
            return route.sourceTrackId == duplicateId;
        });
    expect(clonedRoute != project.reampRoutes.cend()
               && clonedRoute->returnTrackId != duplicatedRoute.returnTrackId,
           "Duplicating a track copies connected sends and their return track.");
    const auto sendReturn = clonedRoute != project.reampRoutes.cend()
        ? project.findTrack(clonedRoute->returnTrackId)
        : nullptr;
    expect(sendReturn != nullptr
               && sendReturn->clips.empty()
               && sendReturn->inserts.size()
                      == project.findTrack(duplicatedRoute.returnTrackId)->inserts.size(),
           "Hardware sends copy return settings without unrelated return audio.");
    const auto clonedReturnTrackId = clonedRoute != project.reampRoutes.cend()
        ? clonedRoute->returnTrackId
        : juce::String();

    expect(history.perform(std::make_unique<studio::RemoveTrackCommand>(duplicateId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(duplicateId) == nullptr, "Remove track command deletes a track.");
    expect(clonedReturnTrackId.isEmpty()
               || project.findTrack(clonedReturnTrackId) == nullptr,
           "Deleting a duplicate removes return tracks owned by its copied sends.");
    expect(history.undo(project), "Remove track command can be undone.");
    expect(project.findTrack(duplicateId) != nullptr, "Undo restores the removed track.");

    studio::AudioClip clip;
    clip.name = "Take 1";
    clip.durationSeconds = 8.0;
    clip.sourceLengthSeconds = 8.0;
    const auto clipId = clip.id;

    expect(history.perform(std::make_unique<studio::AddClipCommand>(project.tracks.front().id, clip),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findClip(clipId) != nullptr, "Add clip command inserts a clip.");

    expect(history.perform(std::make_unique<studio::MoveClipCommand>(clipId, 2.0), project, error),
           error.toRawUTF8());
    expect(project.findClip(clipId)->startSeconds == 2.0, "Move clip command updates its start.");

    const auto destinationTrackId = project.tracks[1].id;
    expect(history.perform(std::make_unique<studio::MoveClipCommand>(clipId,
                                                                    2.5,
                                                                    destinationTrackId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrackContainingClip(clipId)->id == destinationTrackId,
           "Move clip command transfers audio to another track.");
    expect(history.undo(project), "Cross-track clip move can be undone.");
    expect(project.findTrackContainingClip(clipId)->id == project.tracks.front().id
               && project.findClip(clipId)->startSeconds == 2.0,
           "Undo restores the original track and position.");

    expect(history.perform(std::make_unique<studio::TrimClipCommand>(clipId, 3.0, 1.0, 7.0),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findClip(clipId)->startSeconds == 3.0
               && project.findClip(clipId)->sourceOffsetSeconds == 1.0
               && project.findClip(clipId)->durationSeconds == 7.0,
           "Trim clip command preserves a non-destructive source offset.");
    expect(history.undo(project), "Trim clip command can be undone.");
    expect(project.findClip(clipId)->startSeconds == 2.0
               && project.findClip(clipId)->sourceOffsetSeconds == 0.0
               && project.findClip(clipId)->durationSeconds == 8.0,
           "Undo restores the original clip boundaries.");

    expect(history.perform(std::make_unique<studio::SplitClipCommand>(clipId, 5.0), project, error),
           error.toRawUTF8());
    expect(project.tracks.front().clips.size() == 2, "Split clip command creates two clips.");

    expect(history.undo(project), "Split clip command can be undone.");
    expect(project.tracks.front().clips.size() == 1, "Undo restores the original clip.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.tracks.front().clips.size() == 2, "Redo repeats the split.");

    studio::PluginInsert insert;
    insert.pluginIdentifier = "VST3-command-test";
    insert.name = "Command Test";
    insert.format = "VST3";
    const auto insertId = insert.id;

    expect(history.perform(std::make_unique<studio::AddPluginInsertCommand>(
                               project.tracks.front().id,
                               insert),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.tracks.front().inserts.size() == 1, "Add insert command creates a slot.");

    expect(history.perform(std::make_unique<studio::SetPluginBypassCommand>(
                               project.tracks.front().id,
                               insertId,
                               true),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.tracks.front().inserts.front().bypassed, "Bypass command updates the insert.");

    expect(history.perform(std::make_unique<studio::RemovePluginInsertCommand>(
                               project.tracks.front().id,
                               insertId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.tracks.front().inserts.empty(), "Remove insert command removes the slot.");
    expect(history.undo(project), "Remove insert command can be undone.");
    expect(project.tracks.front().inserts.size() == 1, "Undo restores the plugin insert.");
}

void splitClipBoundaries()
{
    auto project = studio::Project::createDefault();
    project.tracks.front().clips.clear();

    studio::AudioClip clip;
    clip.name = "Recorded take";
    clip.startSeconds = 2.0;
    clip.durationSeconds = 8.0;
    clip.sourceLengthSeconds = 8.0;
    clip.sourceRangeStartSeconds = 0.0;
    clip.sourceRangeEndSeconds = 8.0;
    clip.warpMarkers = { { 6.0, 6.0 } };
    const auto leftId = clip.id;
    project.tracks.front().clips.push_back(clip);

    studio::CommandStack history;
    juce::String error;
    expect(history.perform(std::make_unique<studio::SplitClipCommand>(leftId, 5.0),
                           project,
                           error),
           error.toRawUTF8());

    const auto& clips = project.tracks.front().clips;
    expect(clips.size() == 2, "Split creates left and right clip halves.");
    const auto rightId = clips[1].id;
    expect(std::abs(clips[0].sourceRangeStartSeconds - 0.0) < 0.0001
               && std::abs(clips[0].sourceRangeEndSeconds - 3.0) < 0.0001,
           "The left half cannot expand past the split source position.");
    expect(std::abs(clips[1].sourceRangeStartSeconds - 3.0) < 0.0001
               && std::abs(clips[1].sourceRangeEndSeconds - 8.0) < 0.0001,
           "The right half cannot expand before the split source position.");
    expect(clips[1].warpMarkers.size() == 1
               && std::abs(clips[1].warpMarkers.front().timelineOffsetSeconds - 3.0) < 0.0001,
           "Split shifts right-side warp markers once.");
    expect(history.undo(project), "A warped split can be undone.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.tracks.front().clips[1].warpMarkers.size() == 1
               && std::abs(project.tracks.front().clips[1]
                               .warpMarkers.front().timelineOffsetSeconds
                           - 3.0)
                      < 0.0001,
           "Redo preserves right-side warp marker positions.");

    error.clear();
    expect(!history.perform(std::make_unique<studio::TrimClipCommand>(leftId, 2.0, 0.0, 3.25),
                            project,
                            error),
           "The left half rejects expansion across the split.");
    error.clear();
    expect(!history.perform(std::make_unique<studio::TrimClipCommand>(rightId,
                                                                      4.75,
                                                                      2.75,
                                                                      5.25),
                            project,
                            error),
           "The right half rejects expansion across the split.");

    error.clear();
    expect(history.perform(std::make_unique<studio::TrimClipCommand>(leftId, 2.0, 0.0, 2.0),
                           project,
                           error),
           error.toRawUTF8());
    error.clear();
    expect(history.perform(std::make_unique<studio::TrimClipCommand>(rightId, 6.0, 4.0, 4.0),
                           project,
                           error),
           error.toRawUTF8());

    const auto* shortenedLeft = project.findClip(leftId);
    const auto* shortenedRight = project.findClip(rightId);
    expect(shortenedLeft != nullptr
               && std::abs(shortenedLeft->recoverableEndSeconds() - 5.0) < 0.0001,
           "A shortened left half retains a recoverable tail up to the split.");
    expect(shortenedRight != nullptr
               && std::abs(shortenedRight->recoverableStartSeconds() - 5.0) < 0.0001,
           "A shortened right half retains a recoverable head back to the split.");

    error.clear();
    expect(history.perform(std::make_unique<studio::TrimClipCommand>(leftId, 2.0, 0.0, 3.0),
                           project,
                           error),
           error.toRawUTF8());
    error.clear();
    expect(history.perform(std::make_unique<studio::TrimClipCommand>(rightId, 5.0, 3.0, 5.0),
                           project,
                           error),
           error.toRawUTF8());
    expect(std::abs(project.findClip(leftId)->endSeconds() - 5.0) < 0.0001
               && std::abs(project.findClip(rightId)->startSeconds - 5.0) < 0.0001,
           "Both split halves can expand back to their shared boundary.");
}

void busRouting()
{
    auto project = studio::Project::createDefault();
    studio::CommandStack history;
    juce::String error;

    studio::Track guitarBus;
    guitarBus.name = "Guitars";
    guitarBus.type = studio::TrackType::bus;
    const auto guitarBusId = guitarBus.id;
    expect(history.perform(std::make_unique<studio::AddTrackCommand>(guitarBus),
                           project,
                           error),
           error.toRawUTF8());

    studio::Track mixBus;
    mixBus.name = "Mix Bus";
    mixBus.type = studio::TrackType::bus;
    const auto mixBusId = mixBus.id;
    expect(history.perform(std::make_unique<studio::AddTrackCommand>(mixBus),
                           project,
                           error),
           error.toRawUTF8());

    const auto audioTrackId = project.tracks.front().id;
    expect(history.perform(std::make_unique<studio::SetTrackOutputCommand>(
                               audioTrackId,
                               guitarBusId),
                           project,
                           error),
           error.toRawUTF8());
    expect(history.perform(std::make_unique<studio::SetTrackOutputCommand>(
                               guitarBusId,
                               mixBusId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(audioTrackId)->outputTrackId == guitarBusId
               && project.findTrack(guitarBusId)->outputTrackId == mixBusId,
           "Tracks can route through nested buses.");

    const auto order = project.routingOrder(error);
    const auto audioPosition = order.has_value()
        ? std::find(order->cbegin(), order->cend(), audioTrackId)
        : std::vector<juce::String>::const_iterator {};
    const auto guitarPosition = order.has_value()
        ? std::find(order->cbegin(), order->cend(), guitarBusId)
        : std::vector<juce::String>::const_iterator {};
    const auto mixPosition = order.has_value()
        ? std::find(order->cbegin(), order->cend(), mixBusId)
        : std::vector<juce::String>::const_iterator {};
    expect(order.has_value()
               && audioPosition < guitarPosition
               && guitarPosition < mixPosition,
           "Routing order processes sources before destination buses.");

    error.clear();
    expect(!history.perform(std::make_unique<studio::SetTrackOutputCommand>(
                                mixBusId,
                                guitarBusId),
                            project,
                            error)
               && error.containsIgnoreCase("cycle"),
           "Routing rejects cycles.");

    const auto unrelatedAudioId = project.tracks[1].id;
    error.clear();
    expect(!history.perform(std::make_unique<studio::SetTrackOutputCommand>(
                                guitarBusId,
                                unrelatedAudioId),
                            project,
                            error),
           "Routing rejects audio-track destinations.");

    expect(history.undo(project), "Track output changes can be undone.");
    expect(project.findTrack(guitarBusId)->outputTrackId.isEmpty(),
           "Undo restores the prior master output.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.findTrack(guitarBusId)->outputTrackId == mixBusId,
           "Redo restores a bus output.");

    expect(history.perform(std::make_unique<studio::RemoveTrackCommand>(guitarBusId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(audioTrackId)->outputTrackId.isEmpty(),
           "Deleting a bus reroutes its sources to the master.");
    expect(history.undo(project), "Bus deletion can be undone.");
    expect(project.findTrack(guitarBusId) != nullptr
               && project.findTrack(audioTrackId)->outputTrackId == guitarBusId,
           "Undo restores a deleted bus and its incoming routes.");
}

void transportMaps()
{
    auto project = studio::Project::createDefault();
    project.tempoChanges = {
        { 0.0, 120.0, true },
        { 4.0, 180.0, false },
        { 8.0, 90.0, false }
    };
    project.meterChanges = {
        { 0.0, 4, 4 },
        { 6.0, 7, 8 }
    };

    expect(std::abs(project.tempoAt(2.0) - 150.0) < 0.0001,
           "Tempo ramps interpolate between map points.");
    expect(std::abs(project.tempoAt(5.0) - 180.0) < 0.0001,
           "Abrupt tempo segments hold their value.");
    expect(std::abs(project.beatsAt(4.0) - 10.0) < 0.0001,
           "Beat conversion integrates a tempo ramp.");
    expect(std::abs(project.secondsAtBeat(10.0) - 4.0) < 0.0001,
           "Beat conversion inverts tempo-map integration.");
    expect(project.meterAt(7.0).numerator == 7
               && project.meterAt(7.0).denominator == 8,
           "Meter changes apply at their timeline position.");
    const auto musicalPosition = project.musicalPositionAt(7.0);
    expect(musicalPosition.bar == 5
               && musicalPosition.beat == 7
               && musicalPosition.ticks == 0,
           "Musical positions count bars and denominator beats across meter changes.");

    project.punchEnabled = true;
    project.punchInSeconds = 4.0;
    project.punchOutSeconds = 8.0;
    project.countInBars = 1;
    project.preRollSeconds = 0.5;
    project.postRollSeconds = 1.0;
    const auto plan = project.recordingPlan(6.0);
    expect(std::abs(plan.captureStartSeconds - 4.0) < 0.0001
               && std::abs(plan.captureEndSeconds - 8.0) < 0.0001
               && std::abs(plan.transportEndSeconds - 9.0) < 0.0001
               && plan.transportStartSeconds < plan.captureStartSeconds,
           "Punch recording plans include count-in, pre-roll, and post-roll.");

    project.punchEnabled = false;
    project.loopEnabled = true;
    project.loopStartSeconds = 2.0;
    project.loopEndSeconds = 6.0;
    const auto loopPlan = project.recordingPlan(5.0);
    expect(loopPlan.loopEnabled
               && std::abs(loopPlan.captureStartSeconds - 2.0) < 0.0001
               && std::abs(loopPlan.loopEndSeconds - 6.0) < 0.0001,
           "Loop recording plans begin at the loop boundary.");
}

void playlistsAndComping()
{
    auto project = studio::Project::createDefault();
    auto& parent = project.tracks.front();
    const auto parentId = parent.id;

    studio::Track firstTake;
    firstTake.name = "v1";
    firstTake.parentTrackId = parentId;
    firstTake.versionNumber = 1;
    const auto firstTakeId = firstTake.id;
    studio::Track secondTake;
    secondTake.name = "v2";
    secondTake.parentTrackId = parentId;
    secondTake.versionNumber = 2;
    const auto secondTakeId = secondTake.id;
    project.tracks.insert(project.tracks.begin() + 1, firstTake);
    project.tracks.insert(project.tracks.begin() + 2, secondTake);

    expect(project.activeTakeTrackId(parentId) == secondTakeId,
           "The newest version is the default active playlist.");

    studio::CommandStack history;
    juce::String error;
    expect(history.perform(std::make_unique<studio::SetActiveTakeCommand>(parentId,
                                                                          firstTakeId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.activeTakeTrackId(parentId) == firstTakeId,
           "An explicit playlist selection becomes active.");

    std::vector<studio::CompRegion> original {
        { juce::Uuid().toString(), firstTakeId, 0.0, 4.0 }
    };
    auto replacement = studio::replaceCompRegion(
        original,
        { juce::Uuid().toString(), secondTakeId, 1.0, 2.0 });
    expect(replacement.size() == 3
               && replacement[0].sourceTrackId == firstTakeId
               && std::abs(replacement[0].durationSeconds - 1.0) < 0.0001
               && replacement[1].sourceTrackId == secondTakeId
               && replacement[2].sourceTrackId == firstTakeId,
           "A comp selection replaces only its overlapping timeline range.");

    expect(history.perform(std::make_unique<studio::SetCompRegionsCommand>(
                               parentId,
                               project.findTrack(parentId)->compRegions,
                               replacement),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(parentId)->compRegions.size() == 3,
           "Comp regions are applied to the parent playlist.");
    expect(history.undo(project), "Comp changes can be undone.");
    expect(project.findTrack(parentId)->compRegions.empty(),
           "Undo restores the previous comp.");

    studio::RecordingPlan loopPlan;
    loopPlan.loopEnabled = true;
    loopPlan.loopStartSeconds = 2.0;
    loopPlan.loopEndSeconds = 6.0;
    loopPlan.captureStartSeconds = 2.0;
    const auto passes = studio::recordingPasses(9.5, loopPlan);
    expect(passes.size() == 3
               && std::abs(passes[0].timelineStartSeconds - 2.0) < 0.0001
               && std::abs(passes[0].durationSeconds - 4.0) < 0.0001
               && std::abs(passes[1].sourceOffsetSeconds - 4.0) < 0.0001
               && std::abs(passes[2].durationSeconds - 1.5) < 0.0001,
           "Loop recordings split continuous capture into alternate takes.");
}

void linkedMultitrackEditing()
{
    auto project = studio::Project::createDefault();
    project.tracks[0].clips.clear();
    project.tracks[1].clips.clear();

    studio::AudioClip first;
    first.name = "Mic 1";
    first.startSeconds = 2.0;
    first.durationSeconds = 6.0;
    first.sourceLengthSeconds = 6.0;
    first.sourceRangeEndSeconds = 6.0;
    const auto firstId = first.id;
    project.tracks[0].clips.push_back(first);

    studio::AudioClip second = first;
    second.id = juce::Uuid().toString();
    second.name = "Mic 2";
    const auto secondId = second.id;
    project.tracks[1].clips.push_back(second);

    studio::EditGroup group;
    group.name = "Drums";
    group.trackIds = { project.tracks[0].id, project.tracks[1].id };
    group.timingReferenceTrackId = project.tracks[0].id;
    group.quantizeStrength = 0.5;
    group.protectedAnchorsSeconds = { 4.0 };

    studio::CommandStack history;
    juce::String error;
    expect(history.perform(std::make_unique<studio::SetEditGroupsCommand>(
                               project.editGroups,
                               std::vector<studio::EditGroup> { group }),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.editGroupForTrack(project.tracks[1].id) != nullptr,
           "Linked tracks resolve to their edit group.");

    std::vector<std::unique_ptr<studio::ProjectCommand>> splitCommands;
    splitCommands.push_back(std::make_unique<studio::SplitClipCommand>(firstId, 5.0));
    splitCommands.push_back(std::make_unique<studio::SplitClipCommand>(secondId, 5.0));
    expect(history.perform(std::make_unique<studio::BatchProjectCommand>(
                               "Split linked clips",
                               std::move(splitCommands)),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.tracks[0].clips.size() == 2
               && project.tracks[1].clips.size() == 2,
           "A linked split preserves phase across every group track.");
    expect(history.undo(project), "A linked edit can be undone in one step.");
    expect(project.tracks[0].clips.size() == 1
               && project.tracks[1].clips.size() == 1,
           "Undo restores every clip in a linked edit.");
}

void audioProcessingTools()
{
    studio::AudioClip clip;
    clip.durationSeconds = 4.0;
    clip.sourceLengthSeconds = 8.0;
    clip.sourceRangeEndSeconds = 8.0;
    clip.playbackRate = 2.0;
    expect(std::abs(clip.sourceSecondsAt(1.0) - 2.0) < 0.0001,
           "Playback rate maps timeline time into source time.");

    clip.warpMarkers = { { 1.0, 3.0 } };
    expect(std::abs(clip.sourceSecondsAt(0.5) - 1.5) < 0.0001
               && std::abs(clip.sourceSecondsAt(2.0) - 4.6666667) < 0.0002,
           "Warp markers piecewise-map timeline positions into source audio.");
    expect(std::abs(clip.timelineOffsetForSourceSeconds(3.0) - 1.0) < 0.0001,
           "Source markers map back to their warped timeline position.");

    clip.fadeInSeconds = 1.0;
    clip.fadeOutSeconds = 1.0;
    expect(std::abs(clip.envelopeGainAt(0.5) - 0.5f) < 0.0001f
               && std::abs(clip.envelopeGainAt(3.5) - 0.5f) < 0.0001f,
           "Clip fades expose deterministic envelope gain.");

    juce::AudioBuffer<float> buffer(2, 48000);
    buffer.clear();
    buffer.setSample(0, 4800, 1.0f);
    buffer.setSample(1, 4800, 0.8f);
    buffer.setSample(0, 24000, 0.9f);
    buffer.setSample(1, 24000, 0.7f);
    const auto transients = studio::AudioAnalysis::detectTransients(buffer, 48000.0);
    expect(transients.size() == 2
               && std::abs(transients[0] - 0.1) < 0.01
               && std::abs(transients[1] - 0.5) < 0.01,
           "Transient analysis deterministically finds separated attacks.");

    auto project = studio::Project::createDefault();
    project.tracks.front().clips = { clip };
    const auto before = project.tracks.front().clips.front();
    auto after = before;
    after.fadeInSeconds = 0.25;
    after.transientSourceSeconds = transients;
    studio::CommandStack history;
    juce::String error;
    expect(history.perform(std::make_unique<studio::SetClipStateCommand>(
                               project.tracks.front().id,
                               before,
                               after,
                               "Process clip"),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.tracks.front().clips.front().transientSourceSeconds.size() == 2,
           "Clip processing changes are applied atomically.");
    expect(history.undo(project), "Clip processing changes can be undone.");
    expect(project.tracks.front().clips.front().transientSourceSeconds.empty(),
           "Undo restores the original clip processing state.");
}

void reampWorkflow()
{
    auto project = studio::Project::createDefault();
    studio::ReampRoute route;
    route.name = "Bass reamp";
    route.sourceTrackId = project.tracks[0].id;
    route.returnTrackId = project.tracks[1].id;
    route.type = studio::TonePathType::hardware;
    route.outputChannel = 2;
    route.inputChannel = 1;

    studio::CommandStack history;
    juce::String error;
    expect(history.perform(std::make_unique<studio::SetReampRoutesCommand>(
                               project.reampRoutes,
                               std::vector<studio::ReampRoute> { route }),
                           project,
                           error),
           error.toRawUTF8());
    const auto* stored = project.reampRouteForReturn(project.tracks[1].id);
    expect(stored != nullptr
               && stored->sourceTrackId == project.tracks[0].id
               && stored->outputChannel == 2,
           "A hardware tone path links DI, send, and return tracks.");
    expect(history.undo(project), "Reamp route creation can be undone.");
    expect(project.reampRoutes.empty(), "Undo removes the reamp relationship.");
}

void packagePersistence()
{
    auto project = studio::Project::createDefault();
    project.name = "Persistence";

    const auto package = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("StudioDuoModelTests", ".studioduo", false);

    const auto saveResult = studio::ProjectFile::save(project, package);
    expect(saveResult.wasOk(), saveResult.getErrorMessage().toRawUTF8());

    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value(), error.toRawUTF8());
    expect(loaded.has_value() && loaded->id == project.id, "Saved package restores the project ID.");
    expect(package.getChildFile("manifest.json").existsAsFile(), "Package contains a manifest.");
    expect(package.getChildFile("recovery/latest.json").existsAsFile(), "Package contains a recovery point.");

    package.deleteRecursively();
}

void rejectsInvalidBaseMeter()
{
    auto project = studio::Project::createDefault();
    auto value = project.toVar();
    value.getDynamicObject()->setProperty("timeSignatureDenominator", 0);
    juce::String error;
    expect(!studio::Project::fromVar(value, error).has_value(),
           "Project loading rejects an invalid base meter denominator.");
}

void pluginAwareExportGuard()
{
    auto project = studio::Project::createDefault();
    studio::PluginInsert insert;
    insert.pluginIdentifier = "VST3-export-test";
    insert.name = "Export Test";
    insert.format = "VST3";
    project.tracks.front().inserts.push_back(insert);

    expect(project.hasActivePluginInserts(), "Project reports active plugin inserts.");
    project.tracks.front().inserts.front().bypassed = true;
    expect(!project.hasActivePluginInserts(), "Bypassed plugins do not block fast export.");
}

void pluginCatalogFiltering()
{
    studio::PluginCatalogEntry entry;
    entry.name = "Scream Forge";
    entry.manufacturer = "Studio Duo";
    entry.category = "Pitch";
    entry.format = "VST3";

    expect(studio::PluginCatalog::matchesQuery(entry, "scream vst3"),
           "Plugin filter matches across name and format.");
    expect(studio::PluginCatalog::matchesQuery(entry, "studio pitch"),
           "Plugin filter matches manufacturer and category.");
    expect(!studio::PluginCatalog::matchesQuery(entry, "compressor"),
           "Plugin filter rejects unrelated terms.");
}

void pluginBridgeProtocol()
{
    studio::PluginBridgeSharedState state;
    state.numChannels.store(2);
    state.numSamples.store(4);
    state.input[0][0] = 0.25f;
    state.input[0][1] = -0.5f;
    state.input[1][0] = 0.75f;
    state.input[1][1] = -1.0f;
    state.hostSequence.store(1, std::memory_order_release);

    expect(studio::PluginBridgeProtocol::isValid(state), "Bridge protocol header is valid.");
    expect(studio::PluginBridgeProtocol::processAvailableBlock(state),
           "Bridge worker processes a published block.");
    expect(state.workerSequence.load(std::memory_order_acquire) == 1,
           "Bridge worker acknowledges the host sequence.");
    expect(std::abs(state.output[0][0] - 0.25f) < 0.0001f
               && std::abs(state.output[1][1] + 1.0f) < 0.0001f,
           "Bridge transport preserves stereo samples.");
    expect(!studio::PluginBridgeProtocol::processAvailableBlock(state),
           "Bridge worker does not process the same block twice.");
}

void liveRecordingWaveform()
{
    studio::RecordingWaveform waveform;
    waveform.reset();

    std::array<float, studio::RecordingWaveform::bucketSamples> left {};
    std::array<float, studio::RecordingWaveform::bucketSamples> right {};
    left[100] = 0.4f;
    right[300] = -0.8f;
    const float* channels[] { left.data(), right.data() };
    waveform.push(channels,
                  2,
                  0,
                  2,
                  studio::RecordingWaveform::bucketSamples);

    const auto peaks = waveform.snapshot();
    expect(peaks.size() == 1, "Recording waveform creates one completed peak bucket.");
    expect(peaks.size() == 1 && std::abs(peaks.front() - 0.8f) < 0.0001f,
           "Recording waveform captures the loudest selected input sample.");
}

void synchronizedRecordingCapture()
{
    const std::array freeSamples { 512, 256, 384 };
    expect(studio::synchronizedCaptureSamples(512, freeSamples) == 256,
           "Multitrack capture uses the smallest recorder capacity.");
    expect(studio::synchronizedCaptureSamples(128, freeSamples) == 128,
           "Multitrack capture never exceeds the callback block.");
}

void multitrackRecordingTargets()
{
    auto project = studio::Project::createDefault();
    auto& first = project.tracks.front();
    first.name = "Guitar left";
    first.armed = true;
    const auto firstId = first.id;

    studio::Track firstVersion;
    firstVersion.name = "v1";
    firstVersion.parentTrackId = firstId;
    firstVersion.versionNumber = 1;
    firstVersion.armed = true;
    project.tracks.insert(project.tracks.begin() + 1, firstVersion);

    studio::Track second;
    second.name = "Guitar right";
    second.armed = true;
    const auto secondId = second.id;
    project.tracks.insert(project.tracks.end() - 1, second);

    const auto targetIds = project.armedAudioParentTrackIds();
    expect(targetIds.size() == 2, "Armed recording targets contain each parent once.");
    expect(targetIds.size() == 2
               && targetIds[0] == firstId
               && targetIds[1] == secondId,
           "Armed recording targets preserve project track order.");
}

void multitrackRecordingCommand()
{
    auto project = studio::Project::createDefault();
    project.tracks.front().versionsCollapsed = true;
    const auto firstParentId = project.tracks.front().id;

    studio::Track secondParent;
    secondParent.name = "Second mic";
    secondParent.versionsCollapsed = true;
    const auto secondParentId = secondParent.id;
    project.tracks.insert(project.tracks.end() - 1, secondParent);

    studio::Track firstTake;
    firstTake.name = "v1";
    firstTake.parentTrackId = firstParentId;
    firstTake.versionNumber = 1;
    studio::AudioClip firstClip;
    firstClip.name = "First take";
    firstClip.startSeconds = 3.5;
    firstTake.clips.push_back(firstClip);
    const auto firstTakeId = firstTake.id;

    studio::Track secondTake;
    secondTake.name = "v1";
    secondTake.parentTrackId = secondParentId;
    secondTake.versionNumber = 1;
    studio::AudioClip secondClip;
    secondClip.name = "Second take";
    secondClip.startSeconds = 3.5;
    secondTake.clips.push_back(secondClip);
    const auto secondTakeId = secondTake.id;

    studio::CommandStack history;
    juce::String error;
    std::vector<studio::Track> takes;
    takes.push_back(firstTake);
    takes.push_back(secondTake);
    expect(history.perform(std::make_unique<studio::AddRecordingTakeCommand>(std::move(takes)),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(firstTakeId) != nullptr
               && project.findTrack(secondTakeId) != nullptr,
           "A multitrack recording adds every captured take.");
    expect(!project.findTrack(firstParentId)->versionsCollapsed
               && !project.findTrack(secondParentId)->versionsCollapsed,
           "Recording expands every captured parent track.");
    expect(history.undo(project), "A multitrack recording can be undone in one step.");
    expect(project.findTrack(firstTakeId) == nullptr
               && project.findTrack(secondTakeId) == nullptr,
           "Undo removes every take from a multitrack recording.");
    expect(project.findTrack(firstParentId)->versionsCollapsed
               && project.findTrack(secondParentId)->versionsCollapsed,
           "Undo restores parent lane collapse states.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.findTrack(firstTakeId) != nullptr
               && project.findTrack(secondTakeId) != nullptr,
           "Redo restores every take from a multitrack recording.");
}
}

int main()
{
    serializationRoundTrip();
    legacyProjectMigration();
    commandHistory();
    splitClipBoundaries();
    busRouting();
    transportMaps();
    playlistsAndComping();
    linkedMultitrackEditing();
    audioProcessingTools();
    reampWorkflow();
    packagePersistence();
    rejectsInvalidBaseMeter();
    pluginAwareExportGuard();
    pluginCatalogFiltering();
    pluginBridgeProtocol();
    liveRecordingWaveform();
    synchronizedRecordingCapture();
    multitrackRecordingTargets();
    multitrackRecordingCommand();
    routingModelTests();
    routingEngineTests();

    if (failures == 0)
    {
        std::cout << "All Studio Duo model tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
