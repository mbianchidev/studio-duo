#include "MainComponent.h"

#include "logging/StudioLogger.h"

#include <algorithm>
#include <set>

namespace studio
{
juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget() { return nullptr; }

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    for (const auto& descriptor : workspaceCommands())
        commands.add(static_cast<juce::CommandID>(descriptor.command));
    for (const auto& macro : preferences.workspaceSettings().macros)
        commands.add(macro.commandId);
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    if (const auto* descriptor = workspaceCommand(id))
    {
        info.setInfo(descriptor->name, descriptor->name, descriptor->category, 0);
        for (const auto& key : descriptor->defaults)
            info.addDefaultKeypress(key.getKeyCode(), key.getModifiers());
        if (descriptor->command == WorkspaceCommand::toggleAutomation)
            info.setTicked(timeline.isAutomationVisible());
        if (descriptor->command == WorkspaceCommand::toggleStretch)
            info.setTicked(timeline.isStretchToolEnabled());
    }
    else
    {
        const auto& macros = preferences.workspaceSettings().macros;
        const auto found = std::find_if(macros.begin(), macros.end(),
            [&](const auto& macro) { return macro.commandId == id; });
        if (found != macros.end())
            info.setInfo(found->name, "Run this ordered Studio Duo command sequence", "Workflow macros", 0);
    }
    info.setActive(workspaceCommandEnabled(id));
}

bool MainComponent::workspaceCommandEnabled(juce::CommandID id) const
{
    using Command = WorkspaceCommand;
    if (!startupServicesAvailable || exportInProgress || appShutdownPrepared || recordingFinalizationInProgress)
        return false;
    const auto* descriptor = workspaceCommand(id);
    if (descriptor == nullptr)
        return id >= firstMacroCommandId && !startupHub.isVisible() && !macroRunning
            && !hasActiveRecordingTargets() && !audioEngine.isRecording();
    const auto command = descriptor->command;
    if (startupHub.isVisible())
        return command == Command::newProject || command == Command::open || command == Command::settings;
    if (hasActiveRecordingTargets() || audioEngine.isRecording())
        return command == Command::stop || command == Command::record || command == Command::playPause
            || command == Command::toggleAutomation || command == Command::focusTimeline
            || command == Command::zoomOut || command == Command::zoomIn || command == Command::zoomReset
            || command == Command::previousTrack || command == Command::nextTrack
            || command == Command::previousClip || command == Command::nextClip;
    const auto* track = project.findTrack(selectedTrackId);
    const auto* clip = project.findClip(selectedClipId);
    const auto* midi = project.findMidiClip(selectedClipId);
    const auto editable = track != nullptr && !project.isTrackFrozen(track->id);
    const auto audio = editable && clip != nullptr;
    const auto anyClip = editable && (clip != nullptr || midi != nullptr);
    switch (command)
    {
        case Command::undo: return commandStack.canUndo();
        case Command::redo: return commandStack.canRedo();
        case Command::split:
        case Command::trimStart:
        case Command::trimEnd:
        case Command::clipEffects:
        case Command::gainUp:
        case Command::gainDown:
        case Command::fineGainUp:
        case Command::fineGainDown:
        case Command::slipEditor:
        case Command::stretchEditor:
        case Command::clipPolarity:
        case Command::clipReverse:
        case Command::clipMute:
        case Command::crossfade:
        case Command::fadeInToPlayhead:
        case Command::fadeOutFromPlayhead:
        case Command::consolidateClip:
        case Command::detectTransients: return audio;
        case Command::copy:
        case Command::duplicateClip:
        case Command::bounce:
        case Command::bounceInPlace:
        case Command::moveClipLeft:
        case Command::moveClipRight:
        case Command::moveClipUp:
        case Command::moveClipDown: return anyClip;
        case Command::paste: return copiedClipId.isNotEmpty();
        case Command::deleteSelection: return anyClip || (editable && track->type != TrackType::master);
        case Command::groupFolder:
        case Command::groupBusFolder: return editable && track->type != TrackType::master && project.tracks.size() < Project::maximumTracks;
        case Command::freeze: return editable && project.isTrackActive(track->id)
            && (track->type == TrackType::audio || track->type == TrackType::instrument
                || track->type == TrackType::bus || track->type == TrackType::aux);
        case Command::unfreeze: return track != nullptr && project.isTrackFrozen(track->id);
        case Command::toggleTrackActive: return editable && track->type != TrackType::master;
        case Command::hideTrack: return track != nullptr && track->type != TrackType::master;
        case Command::toggleMute:
        case Command::toggleSolo: return track != nullptr && track->type != TrackType::master
            && project.isTrackMixEditable(track->id);
        case Command::toggleArm: return editable && project.isTrackActive(track->id)
            && (track->type == TrackType::audio || track->type == TrackType::midi || track->type == TrackType::instrument);
        case Command::addAudioTrack: return project.tracks.size() < Project::maximumTracks;
        case Command::newMidiClip:
        case Command::captureMidi: return editable && project.isTrackActive(track->id)
            && (track->type == TrackType::midi || track->type == TrackType::instrument);
        case Command::clearSelection: return selectedClipId.isNotEmpty();
        case Command::activateSelection:
        case Command::contextMenu: return track != nullptr;
        case Command::newProject:
        case Command::open:
        case Command::save:
        case Command::saveAs:
        case Command::importAudio:
        case Command::exportMix:
        case Command::playPause:
        case Command::stop:
        case Command::record:
        case Command::focusTimeline:
        case Command::selectAll:
        case Command::toggleAutomation:
        case Command::toggleStretch:
        case Command::showHiddenTracks:
        case Command::zoomOut:
        case Command::zoomIn:
        case Command::zoomReset:
        case Command::toggleSnap:
        case Command::toggleLoop:
        case Command::toggleMetronome:
        case Command::settings:
        case Command::automationPanel:
        case Command::previousTrack:
        case Command::nextTrack:
        case Command::previousClip:
        case Command::nextClip:
        case Command::seekLeft:
        case Command::seekRight:
        case Command::seekStart:
        case Command::seekEnd: return true;
    }
    return false;
}

bool MainComponent::perform(const juce::ApplicationCommandTarget::InvocationInfo& invocation)
{
    return executeWorkspaceCommand(invocation.commandID);
}

bool MainComponent::executeWorkspaceCommand(juce::CommandID id)
{
    if (!workspaceCommandEnabled(id))
    {
        const auto* descriptor = workspaceCommand(id);
        setStatus(juce::String(descriptor != nullptr ? descriptor->name : "Workflow macro")
                  + " is unavailable for the current selection or transport state.", true);
        return false;
    }
    if (id >= firstMacroCommandId)
    {
        const auto& macros = preferences.workspaceSettings().macros;
        const auto found = std::find_if(macros.begin(), macros.end(),
            [&](const auto& macro) { return macro.commandId == id; });
        if (found == macros.end())
        {
            setStatus("The selected workflow macro no longer exists.", true);
            return false;
        }
        const auto macro = *found;
        const juce::ScopedValueSetter<bool> running(macroRunning, true);
        const auto result = runWorkflowMacro(macro, [this](auto command) { return executeWorkspaceCommand(command); });
        setStatus(result.wasOk() ? "Ran macro: " + macro.name : result.getErrorMessage(), result.failed());
        return result.wasOk();
    }
    const auto* descriptor = workspaceCommand(id);
    if (descriptor == nullptr)
        return false;
    lastWorkspaceCommandFailed = false;
    if (descriptor->navigationKey)
        return timeline.dispatchNavigationKey(*descriptor->navigationKey) && !lastWorkspaceCommandFailed;
    using Command = WorkspaceCommand;
    switch (descriptor->command)
    {
        case Command::newProject: createNewProject(); break;
        case Command::open: beginOpenProject(); break;
        case Command::save: beginSaveProject(); break;
        case Command::saveAs: beginSaveProject(true); break;
        case Command::importAudio: beginImportAudio(); break;
        case Command::exportMix: beginExportMix(); break;
        case Command::undo: undo(); break;
        case Command::redo: redo(); break;
        case Command::playPause: togglePlayback(); break;
        case Command::stop: stopTransportAndRecording(); break;
        case Command::record: toggleRecording(); break;
        case Command::focusTimeline: timeline.grabKeyboardFocus(); break;
        case Command::copy: copySelectedClip(); break;
        case Command::paste: pasteCopiedClip(); break;
        case Command::duplicateClip: duplicateSelectedClip(); break;
        case Command::split: splitSelectedClip(); break;
        case Command::trimStart: trimSelectedClipStartToPlayhead(); break;
        case Command::trimEnd: trimSelectedClipEndToPlayhead(); break;
        case Command::deleteSelection: deleteWorkspaceSelection(); break;
        case Command::selectAll:
        {
            std::vector<juce::String> tracks, clips;
            for (const auto* track : ArrangementEditing::visibleTracks(project))
            {
                if (track->type == TrackType::master)
                    continue;
                tracks.push_back(track->id);
                for (const auto& clip : track->clips)
                    clips.push_back(clip.id);
                for (const auto& clip : track->midiClips)
                    clips.push_back(clip.id);
            }
            timeline.setMultiSelection(std::move(tracks), std::move(clips));
            break;
        }
        case Command::addAudioTrack: addAudioTrack(); break;
        case Command::newMidiClip: createMidiClip(selectedTrackId, audioEngine.positionSeconds()); break;
        case Command::captureMidi: captureRetrospectiveMidi(); break;
        case Command::groupFolder: groupSelectedTracks(false); break;
        case Command::groupBusFolder: groupSelectedTracks(true); break;
        case Command::toggleAutomation: timeline.setAutomationVisible(!timeline.isAutomationVisible()); break;
        case Command::toggleStretch:
            timeline.setStretchToolEnabled(!timeline.isStretchToolEnabled());
            stretchToolButton.setToggleState(timeline.isStretchToolEnabled(), juce::dontSendNotification);
            break;
        case Command::clipEffects: showClipEffects(selectedClipId); break;
        case Command::bounce: bounceSelectedClips(false); break;
        case Command::bounceInPlace: bounceSelectedClips(true); break;
        case Command::freeze: freezeTrack(selectedTrackId); break;
        case Command::unfreeze: unfreezeTrack(selectedTrackId); break;
        case Command::toggleTrackActive: changeWorkspaceTrackAvailability(false); break;
        case Command::hideTrack: changeWorkspaceTrackAvailability(true); break;
        case Command::showHiddenTracks: showHiddenTracks(); break;
        case Command::zoomOut: zoomTimeline(1.0 / 1.25); break;
        case Command::zoomIn: zoomTimeline(1.25); break;
        case Command::zoomReset: zoomTimeline(1.0, true); break;
        case Command::toggleSnap:
            snapButton.setToggleState(!snapButton.getToggleState(), juce::dontSendNotification);
            timeline.setSnapEnabled(snapButton.getToggleState());
            break;
        case Command::toggleLoop: changeTransportState([](auto& state) { state.loopEnabled = !state.loopEnabled; }); break;
        case Command::toggleMetronome: changeTransportState([](auto& state) { state.metronomeEnabled = !state.metronomeEnabled; }); break;
        case Command::settings: showSettings(); break;
        case Command::automationPanel: showAutomationPanel(); break;
        case Command::slipEditor: timeline.editSelectedClipProcessing(ClipProcessingEditorComponent::Mode::slip); break;
        case Command::stretchEditor: timeline.editSelectedClipProcessing(ClipProcessingEditorComponent::Mode::stretch); break;
        case Command::clipPolarity: toggleClipPolarity(selectedClipId); break;
        case Command::clipReverse: toggleClipReverse(selectedClipId); break;
        case Command::clipMute: toggleClipMute(selectedClipId); break;
        case Command::crossfade: createClipCrossfade(selectedClipId); break;
        case Command::fadeInToPlayhead: setClipFade(selectedClipId, audioEngine.positionSeconds(), true); break;
        case Command::fadeOutFromPlayhead: setClipFade(selectedClipId, audioEngine.positionSeconds(), false); break;
        case Command::consolidateClip: consolidateClip(selectedClipId); break;
        case Command::detectTransients: analyseClipTransients(selectedClipId); break;
        case Command::previousTrack:
        case Command::nextTrack:
        case Command::previousClip:
        case Command::nextClip:
        case Command::moveClipLeft:
        case Command::moveClipRight:
        case Command::moveClipUp:
        case Command::moveClipDown:
        case Command::seekLeft:
        case Command::seekRight:
        case Command::seekStart:
        case Command::seekEnd:
        case Command::toggleMute:
        case Command::toggleSolo:
        case Command::toggleArm:
        case Command::gainUp:
        case Command::gainDown:
        case Command::fineGainUp:
        case Command::fineGainDown:
        case Command::clearSelection:
        case Command::activateSelection:
        case Command::contextMenu: return false;
    }
    commandManager.commandStatusChanged();
    return !lastWorkspaceCommandFailed;
}

bool MainComponent::handleWorkspaceKey(const juce::KeyPress& key)
{
    if (!startupServicesAvailable)
        return false;
    if (key == juce::KeyPress::spaceKey)
        if (auto* button = dynamic_cast<juce::Button*>(juce::Component::getCurrentlyFocusedComponent());
            button != nullptr && isParentOf(button))
        {
            if (button->isEnabled())
                button->triggerClick();
            return true;
        }
    const auto id = commandManager.getKeyMappings()->findCommandForKeyPress(key);
    if (id == 0)
        return false;
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (!workspaceShortcutAllowed(id, key, {
            dynamic_cast<juce::TextEditor*>(focused) != nullptr, midiEditor.hasKeyboardFocus(true),
            timeline.hasKeyboardFocus(false) }))
        return false;
    if (!workspaceCommandEnabled(id))
        juce::ignoreUnused(executeWorkspaceCommand(id));
    else
        juce::ignoreUnused(commandManager.invokeDirectly(id, false));
    return true;
}

void MainComponent::refreshWorkspaceCommands()
{
    const juce::ScopedValueSetter<bool> updating(updatingWorkspaceCommands, true);
    const auto previous = preferences.workspaceSettings().keyMappingsXml;
    const auto& macros = preferences.workspaceSettings().macros;
    for (int slot = 0; slot < maximumWorkflowMacros; ++slot)
    {
        const auto id = firstMacroCommandId + slot;
        if (std::none_of(macros.begin(), macros.end(), [&](const auto& macro) { return macro.commandId == id; }))
            commandManager.removeCommand(id);
    }
    commandManager.setFirstCommandTarget(this);
    commandManager.registerAllCommandsForTarget(this);
    commandManager.getKeyMappings()->resetToDefaultMappings();
    if (previous.isNotEmpty())
    {
        const auto xml = juce::parseXML(previous);
        if (xml == nullptr || !commandManager.getKeyMappings()->restoreFromXml(*xml))
            setStatus("Saved shortcut mappings could not be restored; default shortcuts remain active.", true);
    }
    for (int slot = 0; slot < maximumWorkflowMacros; ++slot)
        if (std::none_of(macros.begin(), macros.end(),
            [&](const auto& macro) { return macro.commandId == firstMacroCommandId + slot; }))
            commandManager.getKeyMappings()->clearAllKeyPresses(firstMacroCommandId + slot);
    const auto selected = preferences.workspaceSettings().selectedMacroId;
    macroSelector.clear(juce::dontSendNotification);
    for (const auto& macro : macros)
        macroSelector.addItem(macro.name, macro.commandId);
    macroSelector.setSelectedId(selected != 0 ? selected : macros.empty() ? 0 : macros.front().commandId,
                                juce::dontSendNotification);
    runMacroButton.setEnabled(!macros.empty());
    if (previous.isNotEmpty())
    {
        auto settings = preferences.workspaceSettings();
        settings.keyMappingsXml = commandManager.getKeyMappings()->createXml(true)->toString();
        if (settings.keyMappingsXml != previous)
            if (const auto saved = preferences.setWorkspaceSettings(settings); saved.failed())
                setStatus(saved.getErrorMessage(), true);
    }
    updateWorkspaceShortcutLabels();
    commandManager.getKeyMappings()->dispatchPendingMessages();
    commandManager.commandStatusChanged();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* broadcaster)
{
    if (broadcaster != commandManager.getKeyMappings() || updatingWorkspaceCommands)
        return;
    auto settings = preferences.workspaceSettings();
    const auto xml = commandManager.getKeyMappings()->createXml(true);
    if (xml == nullptr)
    {
        setStatus("Could not serialize the shortcut mappings.", true);
        return;
    }
    const auto mappings = xml->toString();
    if (settings.keyMappingsXml == mappings)
        return;
    settings.keyMappingsXml = mappings;
    if (const auto result = preferences.setWorkspaceSettings(settings); result.failed())
    {
        setStatus(result.getErrorMessage(), true);
        refreshWorkspaceCommands();
    }
    else
        updateWorkspaceShortcutLabels();
}

juce::String MainComponent::workspaceShortcutDescription(juce::CommandID id) const
{
    juce::StringArray keys;
    for (const auto& key : commandManager.getKeyMappings()->getKeyPressesAssignedToCommand(id))
        keys.add(key.getTextDescription());
    return keys.isEmpty() ? "Unassigned" : keys.joinIntoString(" / ");
}

void MainComponent::updateWorkspaceShortcutLabels()
{
    using Command = WorkspaceCommand;
    for (const auto& [button, id] : {
            std::pair<juce::Button*, Command> { &saveButton, Command::save },
            { &undoButton, Command::undo }, { &redoButton, Command::redo },
            { &splitClipButton, Command::split }, { &deleteClipButton, Command::deleteSelection },
            { &trimClipStartButton, Command::trimStart }, { &trimClipEndButton, Command::trimEnd },
            { &automationViewButton, Command::toggleAutomation }, { &stretchToolButton, Command::toggleStretch } })
        if (const auto* descriptor = workspaceCommand(static_cast<juce::CommandID>(id)))
            button->setTooltip(juce::String(descriptor->name) + " ("
                + workspaceShortcutDescription(static_cast<juce::CommandID>(id)) + ")");
    runMacroButton.setTooltip("Run the selected workflow macro ("
        + workspaceShortcutDescription(macroSelector.getSelectedId()) + ")");
}

void MainComponent::deleteWorkspaceSelection()
{
    const auto clips = timeline.selectedClipIds();
    const auto tracks = timeline.selectedTrackIds();
    if (clips.empty() && tracks.size() > 1)
    {
        std::vector<std::unique_ptr<ProjectCommand>> commands;
        for (const auto& id : tracks)
        {
            const auto* track = project.findTrack(id);
            if (track != nullptr && track->parentTrackId.isNotEmpty()
                && std::find(tracks.begin(), tracks.end(), track->parentTrackId) != tracks.end())
                continue;
            commands.push_back(std::make_unique<RemoveTrackCommand>(id));
        }
        perform(std::make_unique<BatchProjectCommand>("Delete selected tracks", std::move(commands)));
        return;
    }
    if (clips.size() <= 1)
    {
        selectedClipId.isNotEmpty() ? deleteSelectedClip() : deleteSelectedTrack();
        return;
    }
    std::set<juce::String> ids;
    for (const auto& id : clips)
    {
        if (const auto* clip = project.findClip(id))
        {
            const auto linked = linkedClipsAt(id, clip->startSeconds + clip->durationSeconds * 0.5);
            if (!linked.isComplete())
            {
                setStatus("Every linked track needs an active clip before deleting this selection.", true);
                return;
            }
            ids.insert(linked.clipIds.begin(), linked.clipIds.end());
        }
        else
            ids.insert(id);
    }
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& id : ids)
        if (project.findClip(id) != nullptr)
            commands.push_back(std::make_unique<DeleteClipCommand>(id));
        else
            commands.push_back(std::make_unique<DeleteMidiClipCommand>(id));
    if (perform(std::make_unique<BatchProjectCommand>("Delete selected clips", std::move(commands))))
        selectTrack(selectedTrackId);
}

void MainComponent::changeWorkspaceTrackAvailability(bool hide)
{
    auto ids = timeline.selectedTrackIds();
    if (ids.empty())
        ids.push_back(selectedTrackId);
    const auto deactivate = !hide && std::any_of(ids.begin(), ids.end(), [&](const auto& id)
    {
        const auto* track = project.findTrack(id);
        return track != nullptr && track->active;
    });
    if (deactivate)
    {
        auto active = ArrangementEditing::trackSubtreeIds(project, ids);
        std::erase_if(active, [this](const auto& id) { return !project.isTrackActive(id); });
        juce::String error;
        if (!captureCurrentPluginStates(active, error))
        {
            setStatus(error, true);
            return;
        }
    }
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& id : ids)
    {
        const auto* track = project.findTrack(id);
        if (track == nullptr)
        {
            setStatus("A selected track is no longer available.", true);
            return;
        }
        const auto before = TrackLifecycleState::fromTrack(*track);
        auto after = before;
        if (hide)
            after.hidden = true;
        else
            after.active = !deactivate;
        commands.push_back(std::make_unique<SetTrackLifecycleCommand>(id, before, after, "Change track availability"));
    }
    perform(std::make_unique<BatchProjectCommand>(hide ? "Hide selected tracks" : "Change selected track activation",
                                                 std::move(commands)));
}
}
