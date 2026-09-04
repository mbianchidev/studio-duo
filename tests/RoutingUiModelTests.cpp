#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/RoutingUiModel.h"

void routingUiModelTests()
{
    auto project = studio::Project::createDefault();
    const auto sourceId = project.tracks.front().id;

    studio::Track aux;
    aux.name = "Parallel";
    aux.type = studio::TrackType::aux;
    const auto auxId = aux.id;
    project.tracks.insert(project.tracks.end() - 1, aux);

    studio::Track bus;
    bus.name = "Guitars";
    bus.type = studio::TrackType::bus;
    const auto busId = bus.id;
    project.tracks.insert(project.tracks.end() - 1, bus);

    const auto destinations =
        studio::RoutingUiModel::sendDestinations(project, sourceId);
    expect(std::any_of(
               destinations.cbegin(),
               destinations.cend(),
               [&auxId](const auto& destination)
               {
                   return destination.trackId == auxId;
               })
               && std::any_of(
                   destinations.cbegin(),
                   destinations.cend(),
                   [&busId](const auto& destination)
                   {
                       return destination.trackId == busId;
                   }),
           "Routing UI offers valid aux and bus send destinations.");

    studio::RoutingConnection route;
    route.name = "Parallel send";
    route.kind = studio::RouteKind::send;
    route.tap = studio::RouteTap::preFader;
    route.sourceTrackId = sourceId;
    route.destination.type = studio::RouteEndpointType::track;
    route.destination.trackId = auxId;
    route.gainDecibels = -6.0f;
    expect(studio::RoutingUiModel::summary(project, route)
               .containsIgnoreCase("pre")
               && studio::RoutingUiModel::summary(project, route)
                      .containsIgnoreCase("Parallel"),
           "Routing UI summaries expose tap and destination.");
}
