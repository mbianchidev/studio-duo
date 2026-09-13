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

    studio::Track midi;
    midi.name = "MIDI";
    midi.type = studio::TrackType::midi;
    const auto midiId = midi.id;
    project.tracks.insert(project.tracks.end() - 1, midi);
    studio::Track instrument;
    instrument.name = "Instrument";
    instrument.type = studio::TrackType::instrument;
    const auto instrumentId = instrument.id;
    project.tracks.insert(project.tracks.end() - 1, instrument);

    const auto midiDestinations =
        studio::RoutingUiModel::midiDestinations(project, midiId);
    expect(std::any_of(
               midiDestinations.cbegin(),
               midiDestinations.cend(),
               [&instrumentId](const auto& destination)
               {
                   return destination.trackId == instrumentId;
               }),
           "Routing UI offers instrument tracks as MIDI destinations.");
}
