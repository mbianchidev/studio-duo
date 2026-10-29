#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace studio
{
enum class WorkspaceCommand : juce::CommandID
{
    newProject = 0x2000, open, save, saveAs, importAudio, exportMix,
    undo, redo, playPause, stop, record, focusTimeline, copy, paste, duplicateClip,
    split, trimStart, trimEnd, deleteSelection, selectAll, addAudioTrack,
    newMidiClip, captureMidi, groupFolder, groupBusFolder, toggleAutomation,
    toggleStretch, clipEffects, bounce, bounceInPlace, freeze, unfreeze,
    toggleTrackActive, hideTrack, showHiddenTracks, zoomOut, zoomIn, zoomReset,
    toggleSnap, toggleLoop, toggleMetronome, settings, automationPanel,
    previousTrack, nextTrack, previousClip, nextClip, moveClipLeft, moveClipRight,
    moveClipUp, moveClipDown, seekLeft, seekRight, seekStart, seekEnd,
    toggleMute, toggleSolo, toggleArm, gainUp, gainDown, fineGainUp, fineGainDown,
    clearSelection, activateSelection, contextMenu,
    slipEditor, stretchEditor, clipPolarity, clipReverse, clipMute, crossfade,
    fadeInToPlayhead, fadeOutFromPlayhead, consolidateClip, detectTransients
};

struct WorkspaceCommandDescriptor
{
    WorkspaceCommand command;
    const char* name;
    const char* category;
    std::vector<juce::KeyPress> defaults;
    bool macroSafe = false;
    std::optional<juce::KeyPress> navigationKey {};
};

constexpr juce::CommandID firstMacroCommandId = 0x4000;
constexpr int maximumWorkflowMacros = 32;
constexpr int maximumMacroSteps = 64;

const std::vector<WorkspaceCommandDescriptor>& workspaceCommands();
const WorkspaceCommandDescriptor* workspaceCommand(juce::CommandID);

struct WorkspaceShortcutContext
{
    bool textEditing = false;
    bool midiEditing = false;
    bool timelineFocused = false;
};

bool workspaceShortcutAllowed(juce::CommandID, const juce::KeyPress&, WorkspaceShortcutContext);

struct WorkflowMacro
{
    juce::CommandID commandId = firstMacroCommandId;
    juce::String name;
    std::vector<juce::CommandID> steps;
    juce::Result validate() const;
};

struct WorkspaceSettings
{
    juce::String keyMappingsXml;
    std::vector<WorkflowMacro> macros;
    juce::CommandID selectedMacroId = 0;
    juce::Result validate() const;
    juce::var toVar() const;
    static std::optional<WorkspaceSettings> fromVar(const juce::var&, juce::String& error);
};

juce::Result runWorkflowMacro(const WorkflowMacro&,
                              const std::function<bool(juce::CommandID)>& execute);
}
