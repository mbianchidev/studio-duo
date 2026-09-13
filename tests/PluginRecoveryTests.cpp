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
    database.noteValidation(record, "pass");
    expect(database.save(error), error.toRawUTF8());

    studio::PluginCompatibilityDatabase restoredDatabase(
        root.getChildFile("compatibility.json"));
    expect(restoredDatabase.load(error), error.toRawUTF8());
    const auto restoredRecord = restoredDatabase.find("fixture");
    expect(restoredRecord.has_value()
               && restoredRecord->runtimeCrashCount == 1
               && restoredRecord->timeoutCount == 1
               && restoredRecord->validationStatus == "pass"
               && restoredRecord->lastFailure
                      == studio::PluginFailureKind::timeout,
           "Compatibility records persist crash and timeout diagnostics.");

    const auto compatibilityFile = root.getChildFile("compatibility.json");
    auto forwardValue = juce::JSON::parse(
        compatibilityFile.loadFileAsString());
    auto* forwardRecords =
        forwardValue.getDynamicObject()->getProperty("records").getArray();
    auto future = std::make_unique<juce::DynamicObject>();
    future->setProperty("pluginIdentifier", "future");
    future->setProperty("name", "Future");
    future->setProperty("format", "CLAP");
    future->setProperty("preferredMode", "futureMode");
    future->setProperty("lastFailure", "none");
    forwardRecords->add(juce::var(future.release()));
    compatibilityFile.replaceWithText(
        juce::JSON::toString(forwardValue, true));
    studio::PluginCompatibilityDatabase forwardDatabase(
        compatibilityFile);
    error.clear();
    expect(forwardDatabase.load(error)
               && error.containsIgnoreCase("skipped")
               && forwardDatabase.find("fixture").has_value(),
           "Unknown compatibility records do not discard valid records.");
    juce::String saveError;
    expect(!forwardDatabase.save(saveError)
               && juce::JSON::parse(
                      compatibilityFile.loadFileAsString())
                      .getDynamicObject()
                      ->getProperty("records")
                      .getArray()
                      ->size()
                      == 2,
           "A forward-incompatible database is not truncated on save.");

    auto project = studio::Project::createDefault();
    studio::PluginInsert missing;
    missing.pluginIdentifier = "missing";
    missing.name = "Missing";
    missing.format = "VST3";
    missing.stateFile = first->relativePath;
    missing.stateHash = first->hash;
    missing.missing = true;
    project.tracks.front().inserts.push_back(missing);

    studio::RoutingConnection missingSidechain;
    missingSidechain.name = "Missing sidechain";
    missingSidechain.kind = studio::RouteKind::sidechain;
    missingSidechain.sourceTrackId = project.tracks[1].id;
    missingSidechain.destination.type =
        studio::RouteEndpointType::pluginSidechain;
    missingSidechain.destination.trackId =
        project.tracks.front().id;
    missingSidechain.destination.insertId = missing.id;
    project.routingConnections.push_back(missingSidechain);
    studio::AutomationLane missingAutomation;
    missingAutomation.name = "Missing gain";
    missingAutomation.target.type =
        studio::AutomationTargetType::pluginParameter;
    missingAutomation.target.trackId =
        project.tracks.front().id;
    missingAutomation.target.insertId = missing.id;
    missingAutomation.target.parameterId = "gain";
    missingAutomation.target.parameterIndex = 0;
    missingAutomation.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.5
    });
    project.automationLanes.push_back(missingAutomation);
    expect(studio::ProjectFile::save(project, root).wasOk(),
           "Projects with missing plugins can be saved.");
    const auto restoredProject = studio::ProjectFile::load(
        root,
        error);
    const auto* restoredTrack = restoredProject.has_value()
        ? restoredProject->findTrack(project.tracks.front().id)
        : nullptr;
    juce::MemoryBlock persistedState;
    expect(restoredTrack != nullptr
               && restoredTrack->inserts.size() == 1
               && restoredTrack->inserts.front().id == missing.id
               && restoredTrack->inserts.front().missing
               && restoredTrack->inserts.front().stateFile
                      == missing.stateFile
               && restoredProject->routingConnections.back()
                      .destination.insertId
                      == missing.id
               && restoredProject->automationLanes.front()
                      .target.insertId
                      == missing.id
               && studio::PluginStateStore::load(
                   root,
                   {
                       restoredTrack->inserts.front().stateFile,
                       restoredTrack->inserts.front().stateHash
                   },
                   persistedState,
                   error)
               && persistedState == state,
           "Missing plugins preserve IDs, routing, automation, and opaque state across save and reopen.");

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
