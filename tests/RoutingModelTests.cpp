#include "TestHarness.h"
#include "TestSuites.h"

#include "model/ProjectModel.h"
#include "model/ProjectCommands.h"

void routingModelTests()
{
    auto project = studio::Project::createDefault();

    studio::Track aux;
    aux.name = "Parallel";
    aux.type = studio::TrackType::aux;
    project.tracks.insert(project.tracks.end() - 1, aux);
    studio::Track outputBus;
    outputBus.name = "Output Bus";
    outputBus.type = studio::TrackType::bus;
    project.tracks.insert(project.tracks.end() - 1, outputBus);

    studio::RoutingConnection send;
    send.name = "Parallel send";
    send.kind = studio::RouteKind::send;
    send.tap = studio::RouteTap::preFader;
    send.sourceTrackId = project.tracks.front().id;
    send.destination.type = studio::RouteEndpointType::track;
    send.destination.trackId = aux.id;
    send.gainDecibels = -6.0f;
    project.routingConnections.push_back(send);

    juce::String error;
    const auto decoded = studio::Project::fromVar(project.toVar(), error);
    expect(decoded.has_value(), error.toRawUTF8());
    expect(decoded.has_value()
               && decoded->routingConnections.size()
                      == project.routingConnections.size()
               && decoded->routingConnections.back().kind
                      == studio::RouteKind::send
               && decoded->routingConnections.back().tap
                      == studio::RouteTap::preFader
               && decoded->routingConnections.back().destination.trackId
                      == aux.id,
           "Version 3 projects persist typed routing connections.");

    studio::CommandStack outputHistory;
    error.clear();
    expect(outputHistory.perform(
               std::make_unique<studio::SetTrackOutputCommand>(
                   project.tracks.front().id,
                   outputBus.id),
               project,
               error),
           error.toRawUTF8());
    const auto mainOutput = std::find_if(
        project.routingConnections.cbegin(),
        project.routingConnections.cend(),
        [&project](const auto& connection)
        {
            return connection.kind == studio::RouteKind::mainOutput
                && connection.sourceTrackId == project.tracks.front().id;
        });
    expect(mainOutput != project.routingConnections.cend()
               && mainOutput->destination.trackId == outputBus.id,
           "The legacy output command updates the explicit main route.");
    expect(outputHistory.undo(project)
               && project.resolvedOutputTrackId(project.tracks.front())
                      == project.masterTrackId(),
           "Undo restores the prior explicit main output.");

    auto legacy = studio::Project::createDefault();
    studio::Track bus;
    bus.name = "Legacy Bus";
    bus.type = studio::TrackType::bus;
    legacy.tracks.insert(legacy.tracks.end() - 1, bus);
    legacy.tracks.front().outputTrackId = bus.id;
    auto legacyValue = legacy.toVar();
    auto* legacyObject = legacyValue.getDynamicObject();
    legacyObject->setProperty("formatVersion", 2);
    legacyObject->removeProperty("routingConnections");

    error.clear();
    const auto migrated = studio::Project::fromVar(legacyValue, error);
    const auto route = migrated.has_value()
        ? std::find_if(migrated->routingConnections.cbegin(),
                       migrated->routingConnections.cend(),
                       [&legacy](const auto& connection)
                       {
                           return connection.kind
                                   == studio::RouteKind::mainOutput
                               && connection.sourceTrackId
                                   == legacy.tracks.front().id;
                       })
        : std::vector<studio::RoutingConnection>::const_iterator {};
    expect(migrated.has_value()
               && route != migrated->routingConnections.cend()
               && route->destination.trackId == bus.id
               && migrated->resolvedOutputTrackId(migrated->tracks.front())
                      == bus.id,
           "Version 2 nested bus outputs migrate to explicit main routes.");

    auto commandProject = studio::Project::createDefault();
    studio::Track commandAux;
    commandAux.name = "Command Aux";
    commandAux.type = studio::TrackType::aux;
    commandProject.tracks.insert(commandProject.tracks.end() - 1, commandAux);
    studio::PluginInsert sidechainTarget;
    sidechainTarget.pluginIdentifier = "sidechain-test";
    sidechainTarget.name = "Sidechain target";
    commandProject.tracks.front().inserts.push_back(sidechainTarget);

    studio::RoutingConnection commandSend;
    commandSend.name = "Command send";
    commandSend.kind = studio::RouteKind::send;
    commandSend.sourceTrackId = commandProject.tracks.front().id;
    commandSend.destination.type = studio::RouteEndpointType::track;
    commandSend.destination.trackId = commandAux.id;

    studio::CommandStack history;
    error.clear();
    expect(history.perform(
               std::make_unique<studio::AddRoutingConnectionCommand>(commandSend),
               commandProject,
               error),
           error.toRawUTF8());
    expect(commandProject.findRoutingConnection(commandSend.id) != nullptr,
           "Adding a routing connection is typed and undoable.");

    auto updatedSend = commandSend;
    updatedSend.gainDecibels = -9.0f;
    expect(history.perform(
               std::make_unique<studio::UpdateRoutingConnectionCommand>(
                   commandSend,
                   updatedSend),
               commandProject,
               error),
           error.toRawUTF8());
    expect(commandProject.findRoutingConnection(commandSend.id) != nullptr
               && std::abs(commandProject.findRoutingConnection(commandSend.id)
                               ->gainDecibels
                           + 9.0f)
                      < 0.0001f,
           "Routing connection changes retain stable IDs.");

    studio::RoutingConnection feedback;
    feedback.name = "Feedback sidechain";
    feedback.kind = studio::RouteKind::sidechain;
    feedback.sourceTrackId = commandAux.id;
    feedback.destination.type = studio::RouteEndpointType::pluginSidechain;
    feedback.destination.trackId = commandProject.tracks.front().id;
    feedback.destination.insertId = sidechainTarget.id;
    error.clear();
    expect(!history.perform(
               std::make_unique<studio::AddRoutingConnectionCommand>(feedback),
               commandProject,
               error)
               && error.containsIgnoreCase("cycle"),
           "Routing validation rejects cycles across sends and sidechains.");

    error.clear();
    expect(history.perform(
               std::make_unique<studio::RemoveRoutingConnectionCommand>(
                   commandSend.id),
               commandProject,
               error),
           error.toRawUTF8());
    expect(commandProject.findRoutingConnection(commandSend.id) == nullptr,
           "Routing connections can be removed.");
    expect(history.undo(commandProject)
               && commandProject.findRoutingConnection(commandSend.id) != nullptr,
           "Undo restores a removed routing connection.");

    auto roleProject = studio::Project::createDefault();
    studio::Track folder;
    folder.name = "Guitars";
    folder.type = studio::TrackType::folder;
    const auto folderId = folder.id;
    roleProject.tracks.insert(roleProject.tracks.end() - 1, folder);

    studio::Track vca;
    vca.name = "Guitar VCA";
    vca.type = studio::TrackType::vca;
    vca.controlledTrackIds = { roleProject.tracks.front().id };
    roleProject.tracks.insert(roleProject.tracks.end() - 1, vca);

    studio::Track controlRoom;
    controlRoom.name = "Control Room";
    controlRoom.type = studio::TrackType::controlRoom;
    controlRoom.hardwareOutputChannel = 4;
    roleProject.tracks.insert(roleProject.tracks.end() - 1, controlRoom);

    const auto* audioTrack = roleProject.findTrack(roleProject.tracks.front().id);
    auto beforeRole = studio::TrackRoutingState::fromTrack(*audioTrack);
    auto afterRole = beforeRole;
    afterRole.folderTrackId = folderId;
    afterRole.channelLayout = studio::ChannelLayout::mono;
    afterRole.polarityInverted = true;
    afterRole.soloSafe = true;
    studio::CommandStack roleHistory;
    error.clear();
    expect(roleHistory.perform(
               std::make_unique<studio::SetTrackRoutingStateCommand>(
                   audioTrack->id,
                   beforeRole,
                   afterRole),
               roleProject,
               error),
           error.toRawUTF8());
    const auto* changedTrack = roleProject.findTrack(audioTrack->id);
    expect(changedTrack != nullptr
               && changedTrack->folderTrackId == folderId
               && changedTrack->channelLayout == studio::ChannelLayout::mono
               && changedTrack->polarityInverted
               && changedTrack->soloSafe,
           "Track routing roles are typed and undoable.");

    const auto roleRoundTrip = studio::Project::fromVar(roleProject.toVar(), error);
    expect(roleRoundTrip.has_value()
               && roleRoundTrip->findTrack(changedTrack->id)->folderTrackId
                      == folderId
               && roleRoundTrip->findTrack(vca.id)->controlledTrackIds.size()
                      == 1
               && roleRoundTrip->findTrack(controlRoom.id)
                      ->hardwareOutputChannel
                      == 4,
           "Folder, VCA, control-room, channel, polarity, and solo-safe state persist.");

    auto invalidFolder = studio::TrackRoutingState::fromTrack(
        *roleProject.findTrack(folderId));
    invalidFolder.folderTrackId = folderId;
    error.clear();
    expect(!roleHistory.perform(
               std::make_unique<studio::SetTrackRoutingStateCommand>(
                   folderId,
                   studio::TrackRoutingState::fromTrack(
                       *roleProject.findTrack(folderId)),
                   invalidFolder),
               roleProject,
               error),
           "Folder hierarchy rejects self-cycles.");

    auto lifecycleProject = studio::Project::createDefault();
    studio::CommandStack lifecycleHistory;
    studio::Track lifecycleBus;
    lifecycleBus.name = "Lifecycle Bus";
    lifecycleBus.type = studio::TrackType::bus;
    const auto lifecycleBusId = lifecycleBus.id;
    error.clear();
    expect(lifecycleHistory.perform(
               std::make_unique<studio::AddTrackCommand>(lifecycleBus),
               lifecycleProject,
               error),
           error.toRawUTF8());
    const auto busMain = std::find_if(
        lifecycleProject.routingConnections.cbegin(),
        lifecycleProject.routingConnections.cend(),
        [lifecycleBusId](const auto& connection)
        {
            return connection.kind == studio::RouteKind::mainOutput
                && connection.sourceTrackId == lifecycleBusId;
        });
    expect(busMain != lifecycleProject.routingConnections.cend()
               && busMain->destination.trackId
                      == lifecycleProject.masterTrackId(),
           "Adding a mix track creates an explicit main route.");

    const auto lifecycleAudioId = lifecycleProject.tracks.front().id;
    expect(lifecycleHistory.perform(
               std::make_unique<studio::SetTrackOutputCommand>(
                   lifecycleAudioId,
                   lifecycleBusId),
               lifecycleProject,
               error),
           error.toRawUTF8());
    expect(lifecycleHistory.perform(
               std::make_unique<studio::RemoveTrackCommand>(lifecycleBusId),
               lifecycleProject,
               error),
           error.toRawUTF8());
    expect(lifecycleProject.resolvedOutputTrackId(
               *lifecycleProject.findTrack(lifecycleAudioId))
               == lifecycleProject.masterTrackId()
               && lifecycleProject.validateRoutingGraph(error),
           "Deleting a destination repairs explicit main routes.");
    expect(lifecycleHistory.undo(lifecycleProject)
               && lifecycleProject.findTrack(lifecycleBusId) != nullptr
               && lifecycleProject.resolvedOutputTrackId(
                      *lifecycleProject.findTrack(lifecycleAudioId))
                      == lifecycleBusId,
           "Undo restores deleted tracks and their graph connections.");

    studio::RoutingConnection duplicateSend;
    duplicateSend.name = "Duplicate send";
    duplicateSend.kind = studio::RouteKind::send;
    duplicateSend.sourceTrackId = lifecycleAudioId;
    duplicateSend.destination.type = studio::RouteEndpointType::track;
    duplicateSend.destination.trackId = lifecycleBusId;
    expect(lifecycleHistory.perform(
               std::make_unique<studio::AddRoutingConnectionCommand>(
                   duplicateSend),
               lifecycleProject,
               error),
           error.toRawUTF8());
    auto duplicateCommand = std::make_unique<studio::DuplicateTrackCommand>(
        lifecycleAudioId);
    auto* duplicateCommandPointer = duplicateCommand.get();
    expect(lifecycleHistory.perform(
               std::move(duplicateCommand),
               lifecycleProject,
               error),
           error.toRawUTF8());
    const auto duplicatedId = duplicateCommandPointer->duplicatedTrackId();
    expect(std::any_of(
               lifecycleProject.routingConnections.cbegin(),
               lifecycleProject.routingConnections.cend(),
               [&duplicatedId, &lifecycleBusId](const auto& connection)
               {
                   return connection.kind == studio::RouteKind::send
                       && connection.sourceTrackId == duplicatedId
                       && connection.destination.trackId == lifecycleBusId;
               }),
           "Duplicating a track remaps its routing connections.");
}
