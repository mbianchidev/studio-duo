#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"

#include <cmath>
#include <iostream>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void serializationRoundTrip()
{
    auto project = studio::Project::createDefault();
    project.name = "Serialization";
    project.tempo = 168.0;

    studio::AudioClip clip;
    clip.name = "DI";
    clip.startSeconds = 1.5;
    clip.durationSeconds = 3.25;
    project.tracks.front().clips.push_back(clip);

    juce::String error;
    const auto decoded = studio::Project::fromVar(project.toVar(), error);

    expect(decoded.has_value(), error.toRawUTF8());
    expect(decoded.has_value() && decoded->name == project.name, "Project name survives serialization.");
    expect(decoded.has_value() && std::abs(decoded->tempo - project.tempo) < 0.0001,
           "Tempo survives serialization.");
    expect(decoded.has_value() && decoded->tracks.front().clips.size() == 1,
           "Clips survive serialization.");
}

void commandHistory()
{
    auto project = studio::Project::createDefault();
    studio::CommandStack history;
    juce::String error;

    studio::AudioClip clip;
    clip.name = "Take 1";
    clip.durationSeconds = 8.0;
    const auto clipId = clip.id;

    expect(history.perform(std::make_unique<studio::AddClipCommand>(project.tracks.front().id, clip),
                           project,
                           error),
           error.toRawUTF8());
    expect(project.findClip(clipId) != nullptr, "Add clip command inserts a clip.");

    expect(history.perform(std::make_unique<studio::MoveClipCommand>(clipId, 2.0), project, error),
           error.toRawUTF8());
    expect(project.findClip(clipId)->startSeconds == 2.0, "Move clip command updates its start.");

    expect(history.perform(std::make_unique<studio::SplitClipCommand>(clipId, 5.0), project, error),
           error.toRawUTF8());
    expect(project.tracks.front().clips.size() == 2, "Split clip command creates two clips.");

    expect(history.undo(project), "Split clip command can be undone.");
    expect(project.tracks.front().clips.size() == 1, "Undo restores the original clip.");
    expect(history.redo(project, error), error.toRawUTF8());
    expect(project.tracks.front().clips.size() == 2, "Redo repeats the split.");
}

void packagePersistence()
{
    auto project = studio::Project::createDefault();
    project.name = "Persistence";

    const auto package = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("StudioDuoModelTests", ".studioduo", false);

    const auto saveResult = studio::ProjectFile::save(project, package);
    expect(saveResult.wasOk(), saveResult.getErrorMessage().toRawUTF8());

    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value(), error.toRawUTF8());
    expect(loaded.has_value() && loaded->id == project.id, "Saved package restores the project ID.");
    expect(package.getChildFile("manifest.json").existsAsFile(), "Package contains a manifest.");
    expect(package.getChildFile("recovery/latest.json").existsAsFile(), "Package contains a recovery point.");

    package.deleteRecursively();
}
}

int main()
{
    serializationRoundTrip();
    commandHistory();
    packagePersistence();

    if (failures == 0)
    {
        std::cout << "All Studio Duo model tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
