#include "TestHarness.h"
#include "TestSuites.h"

#include "model/ProjectModel.h"
#include "model/ProjectCommands.h"
#include "model/PluginRack.h"
#include "dawproject_io/DawProjectIO.h"
#include "project_io/ProjectFile.h"
#include "plugin_host/PluginStateStore.h"

#include <map>
#include <cmath>
#include <array>

void pluginRackTests()
{
    studio::PluginInsert plugin;
    plugin.pluginIdentifier = "fixture";
    plugin.name = "Serial rack";
    auto value = plugin.toVar();
    auto* object = value.getDynamicObject();
    object->setProperty("kind", "serialRack");
    object->setProperty("containerId", "parent-rack");
    object->setProperty("gainDecibels", -6.0);
    object->setProperty("pluginIdentifier", "");

    juce::String error;
    const auto restored = studio::PluginInsert::fromVar(value, error);
    expect(restored.has_value(),
           "A serial rack is a native container, not an unavailable external plugin.");
    if (restored.has_value())
    {
        const auto roundTrip = restored->toVar();
        expect(roundTrip["kind"].toString() == "serialRack"
                   && roundTrip["containerId"].toString() == "parent-rack"
                   && std::abs(static_cast<double>(roundTrip["gainDecibels"]) + 6.0) < 0.000001,
               "Rack kind, parent and output level survive serialization.");
    }

    auto project = studio::Project::createDefault();
    studio::PluginInsert firstRack;
    firstRack.kind = studio::PluginInsertKind::serialRack;
    firstRack.name = "First rack";
    studio::PluginInsert secondRack = firstRack;
    secondRack.id = juce::Uuid().toString();
    secondRack.name = "Second rack";
    firstRack.containerId = secondRack.id;
    secondRack.containerId = firstRack.id;
    project.tracks.front().inserts = { firstRack, secondRack };
    error.clear();
    expect(!project.validateRoutingGraph(error) && error.isNotEmpty(),
           "Rack containment cycles are rejected before reaching the audio callback.");

    firstRack.containerId.clear();
    firstRack.bypassed = true;
    plugin.containerId = firstRack.id;
    project.tracks.front().inserts = { firstRack, plugin };
    expect(!project.hasActivePluginInserts(),
           "Bypassing a rack bypasses its contained plugins, not just its header.");
    project.tracks.front().inserts = { firstRack };
    project.tracks.front().inserts.front().bypassed = false;
    expect(!project.hasActivePluginInserts(),
           "An empty native rack does not trigger the missing-plugin export guard.");

    plugin.containerId = "unavailable-rack";
    studio::AddPluginInsertCommand add(project.tracks.front().id, plugin);
    const auto beforeAdd = juce::JSON::toString(project.toVar(), false);
    error.clear();
    expect(!add.perform(project, error) && error.isNotEmpty()
               && juce::JSON::toString(project.toVar(), false) == beforeAdd,
           "Adding to a deleted rack reports an error and leaves the chain unchanged.");

    plugin.containerId = firstRack.id;
    project.tracks.front().inserts = { firstRack, plugin };
    studio::RoutingConnection sidechain;
    sidechain.name = "Nested sidechain";
    sidechain.kind = studio::RouteKind::sidechain;
    sidechain.sourceTrackId = project.tracks[1].id;
    sidechain.destination.type = studio::RouteEndpointType::pluginSidechain;
    sidechain.destination.trackId = project.tracks.front().id;
    sidechain.destination.insertId = plugin.id;
    project.routingConnections.push_back(sidechain);
    studio::AutomationLane lane;
    lane.name = "Nested parameter";
    lane.target.trackId = project.tracks.front().id;
    lane.target.insertId = plugin.id;
    lane.target.type = studio::AutomationTargetType::pluginParameter;
    lane.target.parameterId = "gain";
    project.automationLanes.push_back(lane);
    const auto beforeRemove = juce::JSON::toString(project.toVar(), false);
    studio::RemovePluginInsertCommand remove(project.tracks.front().id, firstRack.id);
    error.clear();
    expect(remove.perform(project, error), error.toRawUTF8());
    expect(project.tracks.front().inserts.empty()
               && project.automationLanes.empty()
               && project.findRoutingConnection(sidechain.id) == nullptr,
           "Removing a rack removes its descendants and their routing and automation.");
    remove.undo(project);
    expect(juce::JSON::toString(project.toVar(), false) == beforeRemove,
           "Undo restores rack children, order, routing, automation and plugin state references.");

    auto reordered = project.tracks.front().inserts;
    reordered.back().containerId.clear();
    reordered.front().gainDecibels = -3.0f;
    studio::SetPluginChainCommand move(project.tracks.front().id, reordered, "Move out of rack");
    error.clear();
    expect(move.perform(project, error), error.toRawUTF8());
    expect(project.tracks.front().inserts.back().containerId.isEmpty()
               && project.tracks.front().inserts.back().id == plugin.id
               && project.findRoutingConnection(sidechain.id) != nullptr,
           "Rack edits preserve plugin IDs and existing parameter/sidechain references.");
    move.undo(project);
    expect(juce::JSON::toString(project.toVar(), false) == beforeRemove,
           "Undo restores rack placement and output levels.");
    reordered.front().containerId = plugin.id;
    studio::SetPluginChainCommand invalidMove(project.tracks.front().id, reordered, "Invalid move");
    error.clear();
    expect(!invalidMove.perform(project, error) && error.isNotEmpty()
               && juce::JSON::toString(project.toVar(), false) == beforeRemove,
           "Moving a rack into a plugin is rejected without changing the project.");

    firstRack.bypassed = false;
    auto splitter = firstRack;
    splitter.id = juce::Uuid().toString();
    splitter.name = "Parallel";
    splitter.kind = studio::PluginInsertKind::parallelSplitter;
    splitter.containerId = firstRack.id;
    auto branchA = firstRack;
    branchA.id = juce::Uuid().toString();
    branchA.name = "Branch A";
    branchA.containerId = splitter.id;
    branchA.gainDecibels = -6.0206f;
    auto branchB = branchA;
    branchB.id = juce::Uuid().toString();
    branchB.name = "Branch B";
    auto prefix = plugin;
    prefix.id = juce::Uuid().toString();
    prefix.containerId = firstRack.id;
    prefix.latencySamples = 16;
    prefix.tailSeconds = 0.25;
    auto effectA = plugin;
    effectA.id = juce::Uuid().toString();
    effectA.containerId = branchA.id;
    effectA.latencySamples = 64;
    effectA.tailSeconds = 1.0;
    auto effectB = effectA;
    effectB.id = juce::Uuid().toString();
    effectB.containerId = branchB.id;
    effectB.latencySamples = 128;
    effectB.tailSeconds = 2.0;
    auto suffix = prefix;
    suffix.id = juce::Uuid().toString();
    suffix.latencySamples = 8;
    suffix.tailSeconds = 0.5;
    project.tracks.front().inserts = {
        firstRack, prefix, splitter, branchA, effectA, branchB, effectB, suffix
    };
    project.routingConnections.pop_back();
    project.automationLanes.clear();
    std::map<juce::String, std::int64_t> inputLatencies;
    const auto timing = studio::PluginRack::measure(
        project.tracks.front().inserts, {}, &inputLatencies);
    expect(timing.latencySamples == 152 && timing.tailSeconds == 2.75
               && inputLatencies[effectA.id] == 16
               && inputLatencies[effectB.id] == 16
               && inputLatencies[suffix.id] == 144,
           "Serial timing sums latencies/tails; parallel timing takes the longest branch.");
    error.clear();
    const auto nested = studio::Project::fromVar(project.toVar(), error);
    expect(nested.has_value()
               && nested->tracks.front().inserts[6].containerId == branchB.id,
           "Nested serial/parallel rack hierarchy survives a format-12 project round-trip.");

    studio::DuplicateTrackCommand duplicate(project.tracks.front().id);
    error.clear();
    expect(duplicate.perform(project, error), error.toRawUTF8());
    const auto* copied = project.findTrack(duplicate.duplicatedTrackId());
    expect(copied != nullptr && copied->inserts.size() == 8
               && copied->inserts[1].containerId == copied->inserts.front().id
               && copied->inserts[6].containerId == copied->inserts[5].id
               && copied->inserts[6].id != effectB.id,
           "Duplicating a track regenerates rack IDs and every nested parent reference.");

    sidechain.destination.insertId = firstRack.id;
    project.routingConnections.push_back(sidechain);
    error.clear();
    expect(!project.validateRoutingGraph(error),
           "Sidechains target contained processors, never a native rack header.");
    project.routingConnections.pop_back();
    const auto archive = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("studio-duo-rack-export", ".dawproject");
    expect(archive.replaceWithText("previous export"), "The synthetic export destination is writable.");
    const auto exported = studio::DawProjectIO::exportProject(project, {}, archive);
    expect(exported.result.failed() && archive.loadFileAsString() == "previous export"
               && std::any_of(exported.report.issues.cbegin(), exported.report.issues.cend(),
                   [](const auto& issue) { return issue.code == "unsupported.plugin-rack"; }),
           "DAWproject export reports unrepresentable rack topology explicitly and never silently serializes parallel effects.");
    expect(archive.deleteFile(), "The synthetic rack export fixture is removed.");

    const auto package = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("studio-duo-rack-package", ".studioduo");
    expect(package.createDirectory().wasOk(), "A disposable native rack package is created.");
    const std::array<std::uint8_t, 4> stateBytes { 1, 2, 3, 4 };
    const juce::MemoryBlock state(stateBytes.data(), stateBytes.size());
    const auto reference = studio::PluginStateStore::store(package, state, error);
    expect(reference.has_value(), "Synthetic contained plugin state is stored content-addressably.");
    if (reference.has_value())
    {
        auto& child = project.tracks.front().inserts[4];
        child.stateFile = reference->relativePath;
        child.stateHash = reference->hash;
        expect(studio::ProjectFile::save(project, package).wasOk(), "The native rack project saves transactionally.");
        const auto reopened = studio::ProjectFile::load(package, error);
        juce::MemoryBlock restoredState;
        expect(reopened.has_value()
                   && reopened->tracks.front().inserts[4].containerId == branchA.id
                   && studio::PluginStateStore::load(package,
                        { reopened->tracks.front().inserts[4].stateFile,
                          reopened->tracks.front().inserts[4].stateHash }, restoredState, error)
                   && restoredState == state,
               "Native package reopening preserves nested plugin placement and its exact state bytes.");
        const auto manifest = juce::JSON::parse(package.getChildFile("manifest.json").loadFileAsString());
        const auto capabilities = manifest["requiredCapabilities"];
        expect(capabilities.isArray()
                   && std::any_of(capabilities.getArray()->begin(), capabilities.getArray()->end(),
                       [](const auto& capability) { return capability.toString() == "pluginRacksV1"; }),
               "Native packages declare their nested rack capability to other readers.");
    }
    expect(package.deleteRecursively(), "The disposable native rack package is removed.");

    auto stemProject = studio::Project::createDefault();
    studio::Track stem;
    stem.name = "Mock stem";
    stem.type = studio::TrackType::aux;
    stemProject.tracks.insert(stemProject.tracks.end() - 1, stem);
    auto stemRack = firstRack;
    stemRack.containerId.clear();
    auto stemPlugin = plugin;
    stemPlugin.bundledDevice = true;
    stemPlugin.containerId = stemRack.id;
    stemProject.tracks.front().inserts = { stemRack, stemPlugin };
    studio::RoutingConnection outputRoute;
    outputRoute.name = "Nested processor output";
    outputRoute.kind = studio::RouteKind::send;
    outputRoute.tap = studio::RouteTap::preFader;
    outputRoute.sourceTrackId = stemProject.tracks.front().id;
    outputRoute.sourceInsertId = stemPlugin.id;
    outputRoute.sourceBusIndex = 1;
    outputRoute.destination.trackId = stem.id;
    stemProject.routingConnections.push_back(outputRoute);
    error.clear();
    expect(stemProject.validateRoutingGraph(error), "Final bundled processor outputs remain routable inside serial racks.");
    stemRack.kind = studio::PluginInsertKind::parallelSplitter;
    branchA.containerId = branchB.containerId = stemRack.id;
    stemPlugin.containerId = branchA.id;
    stemProject.tracks.front().inserts = { stemRack, branchA, branchB, stemPlugin };
    error.clear();
    expect(!stemProject.validateRoutingGraph(error),
           "Per-processor auxiliary output taps cannot bypass a parallel rack's branch summing and compensation.");
}
