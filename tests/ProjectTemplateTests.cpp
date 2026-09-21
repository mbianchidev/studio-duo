#include "TestHarness.h"
#include "model/ProjectTemplates.h"

void projectTemplateTests()
{
    const auto blank =
        studio::ProjectTemplates::createBlankSong();
    expect(blank.tracks.size() == 1
               && blank.tracks.front().type
                   == studio::TrackType::master,
           "A blank song starts with only its master track.");

    const auto& descriptors =
        studio::ProjectTemplates::descriptors();
    expect(descriptors.size() == 3,
           "Three curated song templates are available.");
    for (const auto& descriptor : descriptors)
    {
        const auto project =
            studio::ProjectTemplates::create(
                descriptor.id);
        juce::String error;
        expect(project.has_value()
                   && project->tracks.size() > 1
                   && project->validateRoutingGraph(error),
               ("Project template is valid: "
                + descriptor.name
                + (error.isNotEmpty()
                       ? " (" + error + ")"
                       : juce::String()))
                   .toRawUTF8());
    }
    expect(!studio::ProjectTemplates::create(
                "missing-template")
                .has_value(),
           "Unknown project templates are rejected.");
}
