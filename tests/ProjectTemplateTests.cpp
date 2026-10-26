#include "ReleaseFixtures.h"
#include "TestSuites.h"
#include "model/ProjectCommands.h"
#include "model/ProjectTemplates.h"
#include "project_io/ProjectFile.h"
#include "ui/ProjectNamingComponents.h"

#include <algorithm>
#include <cmath>

namespace
{
void projectNamingControls()
{
    auto project = studio::ProjectTemplates::createBlankSong();
    studio::CommandStack commands;
    studio::ProjectNameLabel label;
    label.setSize(320, 32);
    label.setProjectName(project.name, false);
    juce::String error;
    label.onRename = [&](const juce::String& name)
    {
        error.clear();
        if (commands.perform(std::make_unique<studio::RenameProjectCommand>(name), project, error))
            label.setProjectName(project.name, true);
    };
    const auto now = juce::Time::getCurrentTime();
    juce::Component& control = label;
    control.mouseDoubleClick({
        juce::Desktop::getInstance().getMainMouseSource(), { 5.0f, 5.0f }, {},
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &label, &label, now, { 5.0f, 5.0f }, now, 2, false
    });
    auto* editor = label.getCurrentTextEditor();
    expect(label.isEditableOnDoubleClick() && label.getWantsKeyboardFocus()
               && editor != nullptr && editor->getText() == "Untitled",
           "Double-clicking a blank song's top-bar name opens a named native editor.");
    if (editor == nullptr)
        return;
    editor->setText("  Synthetic song *  ", false);
    label.setProjectName(project.name, true);
    expect(label.isBeingEdited() && editor->getText() == "  Synthetic song *  ",
           "Dirty-state refreshes do not dismiss or overwrite an in-progress name edit.");
    label.hideEditor(false);
    expect(project.name == "Synthetic song *" && label.getText() == "Synthetic song * *"
               && commands.undo(project) && project.name == "Untitled"
               && commands.redo(project, error) && project.name == "Synthetic song *",
           "Renaming trims whitespace, preserves literal stars, and supports ordinary undo and redo.");

    label.showEditor();
    editor = label.getCurrentTextEditor();
    if (editor != nullptr)
        editor->setText("   ", false);
    label.hideEditor(false);
    expect(error.isNotEmpty() && project.name == "Synthetic song *"
               && label.getText() == "Synthetic song * *",
           "An empty top-bar edit reports an error and restores the previous song name.");

    for (const auto& descriptor : studio::ProjectTemplates::descriptors())
    {
        label.setProjectName(descriptor.name, true);
        expect(label.keyPressed(juce::KeyPress(juce::KeyPress::returnKey))
                   && label.getCurrentTextEditor() != nullptr
                   && label.getCurrentTextEditor()->getText() == descriptor.name,
               "Every template song remains keyboard-editable without leaking its unsaved marker into the name.");
        label.hideEditor(true);
    }
    label.setProjectName("Opened song", false);
    expect(label.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)) && label.isBeingEdited(),
           "Saved and imported song names have the same rename action as newly created songs.");
    label.hideEditor(true);

    const auto finishKeyboardEdit = [&](int keyCode)
    {
        if (auto* activeEditor = label.getCurrentTextEditor())
            activeEditor->keyPressed(juce::KeyPress(keyCode));
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
        while (label.isBeingEdited() && juce::Time::getMillisecondCounterHiRes() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        return !label.isBeingEdited();
    };
    label.setProjectName(project.name, true);
    label.showEditor();
    if (auto* activeEditor = label.getCurrentTextEditor())
        activeEditor->setText("Return edit", false);
    expect(finishKeyboardEdit(juce::KeyPress::returnKey) && project.name == "Return edit"
               && label.getText() == "Return edit *",
           "Return commits the actual native text editor without restoring the old name or storing its dirty marker.");
    label.showEditor();
    if (auto* activeEditor = label.getCurrentTextEditor())
        activeEditor->setText("Cancelled edit", false);
    expect(finishKeyboardEdit(juce::KeyPress::escapeKey) && project.name == "Return edit"
               && label.getText() == "Return edit *",
           "Escape closes the native editor without renaming the song.");
    expect(!commands.perform(std::make_unique<studio::RenameProjectCommand>("///"), project, error)
               && error.isNotEmpty() && project.name == "Return edit",
           "A name made entirely of filename separators fails without changing the current song.");
    release_fixture::Directory directory;
    const auto package = directory.file.getChildFile("different-filename.studioduo");
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "A renamed synthetic song can be saved to a differently named package.");
    error.clear();
    const auto restored = studio::ProjectFile::load(package, error);
    expect(restored.has_value() && restored->name == "Return edit" && restored->id == project.id,
           "Song renaming persists the chosen display name without changing project identity.");

    auto dialog = studio::createProjectNameDialog("Metal Tracking (2)", nullptr);
    auto* name = dialog->getTextEditor("name");
    auto* create = dialog->getButton("Create");
    expect(name != nullptr && name->getTitle() == "Song name"
               && name->getText() == "Metal Tracking (2)"
               && create != nullptr && create->isEnabled()
               && dialog->getButton("Cancel") != nullptr,
           "The creation prompt starts with the suggested name and exposes native Create and Cancel actions.");
    if (name != nullptr && create != nullptr)
    {
        const auto waitForCreateEnabled = [create](bool enabled)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
            while (create->isEnabled() != enabled
                   && juce::Time::getMillisecondCounterHiRes() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            return create->isEnabled() == enabled;
        };
        name->setText("   ");
        expect(waitForCreateEnabled(false), "The creation prompt cannot accept an empty song name.");
        name->setText("A custom song");
        expect(waitForCreateEnabled(true), "Typing a custom song name re-enables creation.");
        name->setText("///");
        expect(waitForCreateEnabled(false), "Names without any usable filename characters cannot create hidden packages.");
    }
}

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
    projectNamingControls();
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
