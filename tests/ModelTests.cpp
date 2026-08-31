#include "model/ProjectCommands.h"
#include "audio/RecordingWaveform.h"
#include "plugin_host/PluginCatalog.h"
#include "plugin_host/PluginBridgeProtocol.h"
#include "project_io/ProjectFile.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void serializationRoundTrip()
{
    auto project = studio::Project::createDefault();
    project.name = "Serialization";
    project.tempo = 168.0;
    project.tracks.front().inputChannel = 1;
    project.tracks.front().stereoInput = true;
    project.tracks.front().inputMonitoring = true;

    studio::AudioClip clip;
    clip.name = "DI";
    clip.startSeconds = 1.5;
    clip.durationSeconds = 3.25;
    clip.sourceLengthSeconds = 3.25;
    project.tracks.front().clips.push_back(clip);

    studio::PluginInsert insert;
    insert.pluginIdentifier = "VST3-test-id";
    insert.name = "Test Gate";
    insert.manufacturer = "Studio Duo";
    insert.format = "VST3";
    project.tracks.front().inserts.push_back(insert);

    juce::String error;
    const auto decoded = studio::Project::fromVar(project.toVar(), error);

    expect(decoded.has_value(), error.toRawUTF8());
    expect(decoded.has_value() && decoded->name == project.name, "Project name survives serialization.");
    expect(decoded.has_value() && std::abs(decoded->tempo - project.tempo) < 0.0001,
           "Tempo survives serialization.");
    expect(decoded.has_value() && decoded->tracks.front().clips.size() == 1,
           "Clips survive serialization.");
    expect(decoded.has_value() && decoded->tracks.front().inserts.size() == 1,
           "Plugin inserts survive serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().inputChannel == 1
               && decoded->tracks.front().stereoInput
               && decoded->tracks.front().inputMonitoring,
           "Track input routing survives serialization.");
    expect(decoded.has_value()
               && decoded->tracks.front().inserts.front().bridgeMode
                      == studio::PluginBridgeMode::sandboxed,
           "Plugin bridge mode survives serialization.");
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

    auto duplicate = std::make_unique<studio::DuplicateTrackCommand>(extraTrackId);
    auto* duplicatePointer = duplicate.get();
    expect(history.perform(std::move(duplicate), project, error), error.toRawUTF8());
    const auto duplicateId = duplicatePointer->duplicatedTrackId();
    expect(project.findTrack(duplicateId) != nullptr, "Duplicate track command creates a track.");
    expect(project.findTrack(duplicateId)->id != extraTrackId,
           "Duplicated tracks receive a stable independent ID.");

    expect(history.perform(std::make_unique<studio::RemoveTrackCommand>(duplicateId),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findTrack(duplicateId) == nullptr, "Remove track command deletes a track.");
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
}

int main()
{
    serializationRoundTrip();
    commandHistory();
    packagePersistence();
    pluginCatalogFiltering();
    pluginBridgeProtocol();
    liveRecordingWaveform();

    if (failures == 0)
    {
        std::cout << "All Studio Duo model tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
