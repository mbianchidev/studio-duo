#include "TestHarness.h"
#include "TestSuites.h"

#include "plugin_host/PluginCompatibilityDatabase.h"
#include "plugin_host/PluginStateStore.h"
#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"

void pluginRecoveryTests()
{
    const auto root = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoPluginRecovery",
                              ".studioduo",
                              false);
    root.createDirectory();

    juce::MemoryBlock state("state-data", 10);
    juce::String error;
    const auto first = studio::PluginStateStore::store(root, state, error);
    expect(first.has_value(), error.toRawUTF8());
    const auto second = studio::PluginStateStore::store(root, state, error);
    expect(second.has_value()
               && first->hash == second->hash
               && first->relativePath == second->relativePath,
           "Plugin states are stored once by content hash.");
    juce::MemoryBlock restored;
    expect(first.has_value()
               && studio::PluginStateStore::load(
                      root,
                      *first,
                      restored,
                      error)
               && restored == state,
           "Content-addressed plugin state round-trips.");

    studio::PluginCompatibilityDatabase database(
        root.getChildFile("compatibility.json"));
    studio::PluginCompatibilityRecord record;
    record.pluginIdentifier = "fixture";
    record.name = "Fixture";
    record.format = "CLAP";
    record.vendor = "Studio Duo";
    record.version = "1.0";
    record.architecture = "arm64";
    database.noteFailure(
        record,
        studio::PluginFailureKind::runtimeCrash,
        "worker disconnected");
    database.noteFailure(
        record,
        studio::PluginFailureKind::timeout,
        "state request timed out");
    expect(database.save(error), error.toRawUTF8());

    studio::PluginCompatibilityDatabase restoredDatabase(
        root.getChildFile("compatibility.json"));
    expect(restoredDatabase.load(error), error.toRawUTF8());
    const auto restoredRecord = restoredDatabase.find("fixture");
    expect(restoredRecord.has_value()
               && restoredRecord->runtimeCrashCount == 1
               && restoredRecord->timeoutCount == 1
               && restoredRecord->lastFailure
                      == studio::PluginFailureKind::timeout,
           "Compatibility records persist crash and timeout diagnostics.");

    auto project = studio::Project::createDefault();
    studio::PluginInsert missing;
    missing.pluginIdentifier = "missing";
    missing.name = "Missing";
    missing.format = "VST3";
    missing.stateFile = first->relativePath;
    missing.stateHash = first->hash;
    missing.missing = true;
    project.tracks.front().inserts.push_back(missing);

    studio::PluginInsert replacement;
    replacement.pluginIdentifier = "replacement";
    replacement.name = "Replacement";
    replacement.format = "CLAP";
    studio::CommandStack history;
    error.clear();
    expect(history.perform(
               std::make_unique<studio::ReplacePluginInsertCommand>(
                   project.tracks.front().id,
                   missing.id,
                   replacement),
               project,
               error),
           error.toRawUTF8());
    const auto& replaced = project.tracks.front().inserts.front();
    expect(replaced.id == missing.id
               && replaced.pluginIdentifier == "replacement"
               && replaced.stateFile == missing.stateFile
               && replaced.stateHash == missing.stateHash
               && !replaced.missing,
           "Replacing a missing plugin preserves stable state and target IDs.");
    expect(history.undo(project)
               && project.tracks.front().inserts.front().pluginIdentifier
                      == "missing",
           "Missing plugin replacement is undoable.");

    expect(studio::ProjectFile::writeReducedIsolationMarker(
               root,
               { missing.id })
               .wasOk(),
           "Reduced-isolation activation marker can be written.");
    const auto marked = studio::ProjectFile::reducedIsolationMarker(root);
    expect(marked.size() == 1 && marked.front() == missing.id,
           "Reduced-isolation activation marker identifies active inserts.");
    expect(studio::ProjectFile::clearReducedIsolationMarker(root).wasOk()
               && studio::ProjectFile::reducedIsolationMarker(root).empty(),
           "Clean shutdown clears the reduced-isolation marker.");

    root.deleteRecursively();
}
