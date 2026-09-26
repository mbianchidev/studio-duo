#include "TestHarness.h"
#include "TestSuites.h"
#include "model/ProjectTemplates.h"

#include <algorithm>
#include <cmath>

namespace
{
void expectProjectStructure(
    const studio::Project& project,
    const juce::String& expectedName)
{
    expect(project.id.isNotEmpty(),
           "Project templates assign a project identifier.");
    expect(project.name == expectedName,
           "Project templates preserve their advertised name.");
    expect(std::isfinite(project.tempo)
               && project.tempo >= 20.0
               && project.tempo <= 400.0,
           "Project templates use a practical finite tempo.");

    const auto masterCount = std::count_if(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [](const auto& track)
        {
            return track.type
                == studio::TrackType::master;
        });
    expect(masterCount == 1,
           "Project templates contain exactly one master track.");

    const auto master = std::find_if(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [](const auto& track)
        {
            return track.type
                == studio::TrackType::master;
        });
    if (master == project.tracks.cend())
        return;

    juce::StringArray trackIds;
    for (const auto& track : project.tracks)
    {
        expect(track.id.isNotEmpty()
                   && !trackIds.contains(track.id),
               "Project template track identifiers are non-empty and unique.");
        trackIds.add(track.id);
        expect(track.name.isNotEmpty(),
               "Project template tracks have display names.");
        const auto recordable =
            track.type == studio::TrackType::audio
            || track.type
                   == studio::TrackType::instrument
            || track.type == studio::TrackType::midi;
        expect(!track.armed
                   || recordable,
               "Only recordable tracks start armed.");

        if (track.type == studio::TrackType::master)
            continue;

        const auto routeCount = std::count_if(
            project.routingConnections.cbegin(),
            project.routingConnections.cend(),
            [&track, &master](const auto& route)
            {
                return route.kind
                           == studio::RouteKind::mainOutput
                    && route.sourceTrackId == track.id
                    && route.destination.type
                           == studio::RouteEndpointType::track
                    && route.destination.trackId
                           == master->id;
            });
        expect(routeCount == 1,
               "Every non-master template track routes to the master exactly once.");
    }

    expect(project.routingConnections.size()
               == project.tracks.size() - 1,
           "Project templates contain only the required master routes.");
    juce::String error;
    expect(project.validateRoutingGraph(error),
           ("Project template routing is valid"
            + (error.isNotEmpty()
                   ? ": " + error
                   : juce::String()))
               .toRawUTF8());
}
}

void projectTemplateTests()
{
    const auto blank =
        studio::ProjectTemplates::createBlankSong();
    expectProjectStructure(blank, "Untitled");
    expect(blank.tracks.size() == 1,
           "A blank song starts with only its master track.");
    const auto drums = studio::ProjectTemplates::createDrumPerformanceTrack(blank, 2.0);
    expect(drums.type == studio::TrackType::instrument && drums.armed
               && drums.inserts.size() == 1
               && drums.inserts.front().pluginIdentifier == "studio.device.drum-composer"
               && drums.inserts.front().bundledDevice
               && drums.midiClips.size() == 1
               && drums.midiClips.front().editorMode == studio::MidiEditorMode::drums
               && std::abs(drums.midiClips.front().startBeats - blank.beatsAt(2.0)) < 0.0000001
               && drums.midiClips.front().drumPadBindings.size() == studio::drumPadCount,
           "The drum performance preset creates an armed, playable bundled instrument with an editable mapped clip at the playhead.");

    const auto& descriptors =
        studio::ProjectTemplates::descriptors();
    expect(descriptors.size() == 3,
           "Three curated song templates are available.");
    juce::StringArray descriptorIds;
    for (const auto& descriptor : descriptors)
    {
        expect(descriptor.id.isNotEmpty()
                   && !descriptorIds.contains(
                       descriptor.id),
               "Project template identifiers are non-empty and unique.");
        descriptorIds.add(descriptor.id);
        expect(descriptor.name.isNotEmpty()
                   && descriptor.description.isNotEmpty(),
               "Project templates have user-facing metadata.");

        const auto firstProject =
            studio::ProjectTemplates::create(
                descriptor.id);
        const auto secondProject =
            studio::ProjectTemplates::create(
                descriptor.id);
        expect(firstProject.has_value()
                   && secondProject.has_value(),
               ("Project template can be created: "
                + descriptor.name)
                   .toRawUTF8());
        if (!firstProject.has_value()
            || !secondProject.has_value())
            continue;

        expectProjectStructure(
            *firstProject,
            descriptor.name);
        expect(firstProject->tracks.size() > 1,
               "Curated project templates contain working tracks.");
        expect(firstProject->id != secondProject->id,
               "Each project template creation receives a fresh project identifier.");
        expect(firstProject->tracks.front().id
                   != secondProject->tracks.front().id,
               "Each project template creation receives fresh track identifiers.");
    }
    expect(!studio::ProjectTemplates::create(
                "missing-template")
                .has_value(),
           "Unknown project templates are rejected.");
}
