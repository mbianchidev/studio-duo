#include "model/ProjectCommands.h"
#include "plugin_host/PluginCatalog.h"
#include "plugin_host/PluginBridgeProtocol.h"
#include "project_io/ProjectFile.h"

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

    studio::AudioClip clip;
    clip.name = "DI";
    clip.startSeconds = 1.5;
    clip.durationSeconds = 3.25;
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
               && decoded->tracks.front().inserts.front().bridgeMode
                      == studio::PluginBridgeMode::sandboxed,
           "Plugin bridge mode survives serialization.");
}

void commandHistory()
{
    auto project = studio::Project::createDefault();
    studio::CommandStack history;
    juce::String error;

    studio::AudioClip clip;
    clip.name = "Take 1";
    clip.durationSeconds = 8.0;
    const auto clipId = clip.id;

    expect(history.perform(std::make_unique<studio::AddClipCommand>(project.tracks.front().id, clip),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findClip(clipId) != nullptr, "Add clip command inserts a clip.");

    expect(history.perform(std::make_unique<studio::MoveClipCommand>(clipId, 2.0), project, error),
           error.toRawUTF8());
    expect(project.findClip(clipId)->startSeconds == 2.0, "Move clip command updates its start.");

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
}

int main()
{
    serializationRoundTrip();
    commandHistory();
    packagePersistence();
    pluginCatalogFiltering();
    pluginBridgeProtocol();

    if (failures == 0)
    {
        std::cout << "All Studio Duo model tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
