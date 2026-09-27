#include "ProjectTemplates.h"

#include <array>
#include <algorithm>

namespace studio
{
namespace
{
struct TrackTemplate
{
    const char* name;
    TrackType type;
    std::uint32_t colour;
    bool armed = false;
};

Project createProject(
    const juce::String& name,
    double tempo,
    const std::vector<TrackTemplate>& trackTemplates)
{
    auto project = Project::createDefault();
    project.id = juce::Uuid().toString();
    project.name = name;
    project.tempo = tempo;
    project.tracks.clear();
    project.routingConnections.clear();

    for (const auto& trackTemplate : trackTemplates)
    {
        Track track;
        track.name = trackTemplate.name;
        track.type = trackTemplate.type;
        track.colour =
            juce::Colour(trackTemplate.colour);
        track.armed = trackTemplate.armed;
        project.tracks.push_back(std::move(track));
    }

    Track master;
    master.name = "Master";
    master.type = TrackType::master;
    master.colour = juce::Colour(0xff78c6a3);
    project.tracks.push_back(std::move(master));
    const auto masterId = project.tracks.back().id;
    for (std::size_t index = 0;
         index + 1 < project.tracks.size();
         ++index)
    {
        RoutingConnection output;
        output.name = "Main output";
        output.kind = RouteKind::mainOutput;
        output.sourceTrackId = project.tracks[index].id;
        output.destination.type =
            RouteEndpointType::track;
        output.destination.trackId = masterId;
        project.routingConnections.push_back(
            std::move(output));
    }
    return project;
}
}

Project ProjectTemplates::createBlankSong()
{
    return createProject("Untitled", 120.0, {});
}

Track ProjectTemplates::createDrumPerformanceTrack(
    const Project& project, double startSeconds)
{
    Track track;
    track.name = "Drums " + juce::String(1 + static_cast<int>(std::count_if(
        project.tracks.cbegin(), project.tracks.cend(), [](const auto& existing)
        {
            return existing.name.startsWithIgnoreCase("Drums");
        })));
    track.type = TrackType::instrument;
    track.colour = juce::Colour(0xff5d7fa3);
    track.armed = true;
    PluginInsert instrument;
    instrument.pluginIdentifier = "studio.device.drum-composer";
    instrument.name = "Metal Drum Composer";
    instrument.manufacturer = "Studio Duo";
    instrument.format = "Studio Duo";
    instrument.bundledDevice = true;
    instrument.bridgeMode = PluginBridgeMode::trustedInProcess;
    track.inserts.push_back(std::move(instrument));

    const auto* map = project.drumMaps.empty() ? nullptr : &project.drumMaps.front();
    const auto start = std::max(0.0, startSeconds);
    const auto meter = project.meterAt(start);
    MidiClip clip;
    clip.name = track.name + " MIDI";
    clip.startBeats = project.beatsAt(start);
    clip.durationBeats = static_cast<double>(meter.numerator) * 4.0 / meter.denominator;
    clip.editorMode = MidiEditorMode::drums;
    clip.drumMapId = map != nullptr ? map->id : juce::String();
    clip.drumPadBindings = defaultDrumPadBindings(map);
    track.midiClips.push_back(std::move(clip));
    return track;
}

const std::vector<ProjectTemplateDescriptor>&
ProjectTemplates::descriptors()
{
    static const std::vector descriptors {
        ProjectTemplateDescriptor {
            "metal-tracking",
            "Metal Tracking",
            "Paired rhythm guitars, lead, bass DI, drums, and vocals."
        },
        ProjectTemplateDescriptor {
            "songwriting",
            "Songwriting",
            "A compact drums, bass, guitar, keys, and vocal writing setup."
        },
        ProjectTemplateDescriptor {
            "mix-session",
            "Mix Session",
            "Organized drum, bass, guitar, vocal, and effects stems for mixing."
        }
    };
    return descriptors;
}

std::optional<Project> ProjectTemplates::create(
    const juce::String& templateId)
{
    if (templateId == "metal-tracking")
    {
        return createProject(
            "Metal Tracking",
            140.0,
            {
                { "Rhythm L", TrackType::audio, 0xffdd5b3f, true },
                { "Rhythm R", TrackType::audio, 0xffd98f39, true },
                { "Lead Guitar", TrackType::audio, 0xffb47ac4, false },
                { "Bass DI", TrackType::audio, 0xff78c6a3, true },
                { "Drums", TrackType::instrument, 0xff5d7fa3, false },
                { "Vocals", TrackType::audio, 0xffd8798b, true }
            });
    }
    if (templateId == "songwriting")
    {
        return createProject(
            "Songwriting",
            120.0,
            {
                { "Drum Ideas", TrackType::instrument, 0xff5d7fa3, false },
                { "Bass", TrackType::audio, 0xff78c6a3, true },
                { "Guitar", TrackType::audio, 0xffdd5b3f, true },
                { "Keys", TrackType::instrument, 0xffb47ac4, false },
                { "Vocal", TrackType::audio, 0xffd8798b, true }
            });
    }
    if (templateId == "mix-session")
    {
        return createProject(
            "Mix Session",
            120.0,
            {
                { "Drums", TrackType::audio, 0xff5d7fa3, false },
                { "Bass", TrackType::audio, 0xff78c6a3, false },
                { "Guitars", TrackType::audio, 0xffdd5b3f, false },
                { "Vocals", TrackType::audio, 0xffd8798b, false },
                { "Effects", TrackType::aux, 0xffb47ac4, false }
            });
    }
    return std::nullopt;
}
}
