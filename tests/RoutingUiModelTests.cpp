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

    studio::PluginInsert vendor;
    vendor.pluginIdentifier = "synthetic-vendor-multi-output";
    vendor.name = "Synthetic vendor instrument";
    vendor.outputBusNames = { "Main", "Scene A", "Scene B" };
    project.findTrack(sourceId)->inserts.push_back(vendor);
    auto processorRoute = route;
    processorRoute.sourceInsertId = vendor.id;
    processorRoute.sourceBusIndex = 1;
    expect(studio::RoutingUiModel::processorOutputBusNames(vendor) == vendor.outputBusNames
               && !studio::RoutingUiModel::processorOutputDestinations(project, sourceId, vendor.id, 1).empty()
               && studio::RoutingUiModel::summary(project, processorRoute).contains("Scene A"),
           "External processors expose their actual named auxiliary buses in the existing keyboard-native routing menu.");
    project.routingConnections.push_back(processorRoute);
    juce::String error;
    const auto decoded = studio::Project::fromVar(project.toVar(), error);
    expect(decoded.has_value() && decoded->findTrack(sourceId)->inserts.front().outputBusNames == vendor.outputBusNames,
           ("External bus names and stable per-bus routes survive a native project round trip: " + error).toRawUTF8());
    auto invalidNames = vendor.toVar();
    invalidNames.getDynamicObject()->setProperty("outputBusNames", juce::Array<juce::var> { "Main", 42 });
    expect(!studio::PluginInsert::fromVar(invalidNames, error).has_value(),
           "Native processor bus caches reject non-string entries rather than silently substituting names.");
    invalidNames.getDynamicObject()->setProperty("outputBusNames", juce::String("not-an-array"));
    expect(!studio::PluginInsert::fromVar(invalidNames, error).has_value(),
           "Native processor bus caches reject malformed list shapes.");
}
