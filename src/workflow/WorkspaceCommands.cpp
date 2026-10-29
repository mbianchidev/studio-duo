#include "WorkspaceCommands.h"

#include <algorithm>
#include <set>

namespace studio
{
const std::vector<WorkspaceCommandDescriptor>& workspaceCommands()
{
    static const auto commands = []
    {
        using Command = WorkspaceCommand;
        const auto cmd = juce::ModifierKeys::commandModifier;
        const auto shift = juce::ModifierKeys::shiftModifier;
        const auto alt = juce::ModifierKeys::altModifier;
        const auto key = [](int code, int modifiers = 0) { return juce::KeyPress(code, modifiers, 0); };
        std::vector<WorkspaceCommandDescriptor> result;
        const auto add = [&](Command id, const char* name, const char* category,
                             std::vector<juce::KeyPress> keys = {}, bool macroSafe = false)
        {
            result.push_back({ id, name, category, std::move(keys), macroSafe });
        };
        add(Command::newProject, "New song", "File", { key('N', cmd) });
        add(Command::open, "Open project", "File", { key('O', cmd) });
        add(Command::save, "Save project", "File", { key('S', cmd) });
        add(Command::saveAs, "Save project as", "File", { key('S', cmd | shift) });
        add(Command::importAudio, "Import audio", "File", { key('I', cmd) });
        add(Command::exportMix, "Export mix", "File");
        add(Command::undo, "Undo", "Editing", { key('Z', cmd) }, true);
        add(Command::redo, "Redo", "Editing", { key('Z', cmd | shift) }, true);
        add(Command::playPause, "Play or pause", "Transport", { key(juce::KeyPress::spaceKey) }, true);
        add(Command::stop, "Stop", "Transport", {}, true);
        add(Command::record, "Record", "Transport");
        add(Command::focusTimeline, "Focus timeline", "Timeline", { key(juce::KeyPress::F6Key) }, true);
        add(Command::copy, "Copy selected clip", "Editing", { key('C', cmd) }, true);
        add(Command::paste, "Paste copied clip", "Editing", { key('V', cmd) }, true);
        add(Command::duplicateClip, "Duplicate selected clip", "Editing", { key('D', cmd) }, true);
        add(Command::split, "Split at playhead", "Editing", { key('S') }, true);
        add(Command::trimStart, "Trim start to playhead", "Editing", { key('[') }, true);
        add(Command::trimEnd, "Trim end to playhead", "Editing", { key(']') }, true);
        add(Command::deleteSelection, "Delete selected clips or tracks", "Editing",
            { key(juce::KeyPress::deleteKey), key(juce::KeyPress::backspaceKey) }, true);
        add(Command::selectAll, "Select all timeline clips", "Editing", { key('A', cmd) }, true);
        add(Command::addAudioTrack, "Add audio track", "Tracks", { key('T', cmd) }, true);
        add(Command::newMidiClip, "Create MIDI clip", "Editing", { key('N', cmd | shift) }, true);
        add(Command::captureMidi, "Capture recent MIDI", "Editing", { key('M', cmd | shift) }, true);
        add(Command::groupFolder, "Group selected tracks into folder", "Tracks", { key('F', cmd | shift) }, true);
        add(Command::groupBusFolder, "Group selected tracks into bus folder", "Tracks", { key('G', cmd | shift) }, true);
        add(Command::toggleAutomation, "Show or hide timeline automation", "Automation", { key('A') }, true);
        add(Command::toggleStretch, "Toggle stretch tool", "Editing", { key('T') }, true);
        add(Command::clipEffects, "AudioSuite clip effects", "Rendering");
        add(Command::bounce, "Bounce selected clips to new track", "Rendering", { key('B', cmd) });
        add(Command::bounceInPlace, "Bounce selected clips in place", "Rendering", { key('B', cmd | shift) });
        add(Command::freeze, "Freeze selected track", "Rendering");
        add(Command::unfreeze, "Unfreeze selected track", "Rendering");
        add(Command::toggleTrackActive, "Activate or deactivate selected tracks", "Tracks");
        add(Command::hideTrack, "Hide selected tracks", "Tracks", {}, true);
        add(Command::showHiddenTracks, "Show hidden tracks", "Tracks", {}, true);
        add(Command::zoomOut, "Zoom out", "Timeline", { key('-', cmd) }, true);
        add(Command::zoomIn, "Zoom in", "Timeline", { key('+', cmd), key('=', cmd) }, true);
        add(Command::zoomReset, "Reset zoom", "Timeline", { key('0', cmd) }, true);
        add(Command::toggleSnap, "Toggle edit snapping", "Timeline", {}, true);
        add(Command::toggleLoop, "Toggle loop", "Transport", {}, true);
        add(Command::toggleMetronome, "Toggle metronome", "Transport", {}, true);
        add(Command::settings, "Open settings", "File", { key(',', cmd) });
        add(Command::automationPanel, "Open automation editor", "Automation");
        const auto navigation = [&](Command id, const char* name, juce::KeyPress shortcut)
        {
            add(id, name, "Timeline navigation", { shortcut }, true);
            result.back().navigationKey = shortcut;
        };
        navigation(Command::previousTrack, "Select previous track", key(juce::KeyPress::upKey));
        navigation(Command::nextTrack, "Select next track", key(juce::KeyPress::downKey));
        navigation(Command::previousClip, "Select previous clip", key(juce::KeyPress::leftKey));
        navigation(Command::nextClip, "Select next clip", key(juce::KeyPress::rightKey));
        navigation(Command::moveClipLeft, "Nudge clip left", key(juce::KeyPress::leftKey, shift));
        navigation(Command::moveClipRight, "Nudge clip right", key(juce::KeyPress::rightKey, shift));
        navigation(Command::moveClipUp, "Move clip to previous track", key(juce::KeyPress::upKey, shift));
        navigation(Command::moveClipDown, "Move clip to next track", key(juce::KeyPress::downKey, shift));
        navigation(Command::seekLeft, "Seek left by edit grid", key(juce::KeyPress::leftKey, alt));
        navigation(Command::seekRight, "Seek right by edit grid", key(juce::KeyPress::rightKey, alt));
        navigation(Command::seekStart, "Seek project start", key(juce::KeyPress::homeKey));
        navigation(Command::seekEnd, "Seek project end", key(juce::KeyPress::endKey));
        navigation(Command::toggleMute, "Mute or unmute track", key('M', alt));
        navigation(Command::toggleSolo, "Solo or unsolo track", key('S', alt));
        navigation(Command::toggleArm, "Arm or disarm track", key('R', alt));
        navigation(Command::gainUp, "Increase clip gain by 1 dB", key(juce::KeyPress::upKey, alt));
        navigation(Command::gainDown, "Decrease clip gain by 1 dB", key(juce::KeyPress::downKey, alt));
        navigation(Command::fineGainUp, "Increase clip gain by 0.1 dB", key(juce::KeyPress::upKey, alt | shift));
        navigation(Command::fineGainDown, "Decrease clip gain by 0.1 dB", key(juce::KeyPress::downKey, alt | shift));
        navigation(Command::clearSelection, "Clear clip selection", key(juce::KeyPress::escapeKey));
        navigation(Command::activateSelection, "Seek clip start or edit track", key(juce::KeyPress::returnKey));
        result.back().macroSafe = false;
        navigation(Command::contextMenu, "Open selection actions", key(juce::KeyPress::F10Key, shift));
        result.back().macroSafe = false;
        return result;
    }();
    return commands;
}

const WorkspaceCommandDescriptor* workspaceCommand(juce::CommandID id)
{
    const auto& commands = workspaceCommands();
    const auto found = std::find_if(commands.begin(), commands.end(),
        [&](const auto& descriptor) { return static_cast<juce::CommandID>(descriptor.command) == id; });
    return found == commands.end() ? nullptr : &*found;
}

bool workspaceShortcutAllowed(juce::CommandID id, const juce::KeyPress& key, WorkspaceShortcutContext context)
{
    const auto* descriptor = workspaceCommand(id);
    const auto commandModifier = key.getModifiers().isCommandDown();
    const auto focus = descriptor != nullptr && descriptor->command == WorkspaceCommand::focusTimeline;
    const auto functionFocus = focus && (commandModifier
        || (key.getKeyCode() >= juce::KeyPress::F1Key && key.getKeyCode() <= juce::KeyPress::F16Key));
    if (context.textEditing)
    {
        const auto fileCommand = descriptor != nullptr && commandModifier
            && (descriptor->command == WorkspaceCommand::save || descriptor->command == WorkspaceCommand::saveAs
                || descriptor->command == WorkspaceCommand::open || descriptor->command == WorkspaceCommand::settings);
        if (!functionFocus && !fileCommand)
            return false;
    }
    if (context.midiEditing)
    {
        if (commandModifier && key.getKeyCode() == 'A')
            return false;
        if (!commandModifier && key != juce::KeyPress::spaceKey && !functionFocus)
            return false;
    }
    return descriptor == nullptr || !descriptor->navigationKey || context.timelineFocused;
}

juce::Result WorkflowMacro::validate() const
{
    if (commandId < firstMacroCommandId || commandId >= firstMacroCommandId + maximumWorkflowMacros
        || name.trim().isEmpty() || name.length() > 80 || name.containsAnyOf("\r\n\t")
        || steps.empty() || steps.size() > static_cast<std::size_t>(maximumMacroSteps))
        return juce::Result::fail("A macro needs a unique slot, a name of at most 80 characters and 1 to 64 steps.");
    for (const auto id : steps)
        if (const auto* descriptor = workspaceCommand(id); descriptor == nullptr || !descriptor->macroSafe)
            return juce::Result::fail("Macros accept only synchronous Studio Duo commands, not dialogs, renders or nested macros.");
    return juce::Result::ok();
}

juce::Result WorkspaceSettings::validate() const
{
    if (macros.size() > static_cast<std::size_t>(maximumWorkflowMacros))
        return juce::Result::fail("At most 32 workflow macros can be saved.");
    if (keyMappingsXml.length() > 512 * 1024)
        return juce::Result::fail("The shortcut mapping is too large.");
    if (keyMappingsXml.isNotEmpty())
    {
        const auto xml = juce::parseXML(keyMappingsXml);
        if (xml == nullptr || !xml->hasTagName("KEYMAPPINGS"))
            return juce::Result::fail("Shortcut mappings must be valid native KEYMAPPINGS XML.");
    }
    std::set<juce::CommandID> ids;
    std::set<juce::String> names;
    for (const auto& macro : macros)
    {
        if (const auto valid = macro.validate(); valid.failed())
            return valid;
        if (!ids.insert(macro.commandId).second || !names.insert(macro.name.trim().toLowerCase()).second)
            return juce::Result::fail("Macro slots and names must be unique.");
    }
    if (selectedMacroId != 0 && !ids.contains(selectedMacroId))
        return juce::Result::fail("The selected workflow macro is unavailable.");
    return juce::Result::ok();
}

juce::var WorkspaceSettings::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("keyMappingsXml", keyMappingsXml);
    object->setProperty("selectedMacroId", selectedMacroId);
    juce::Array<juce::var> values;
    for (const auto& macro : macros)
    {
        auto value = std::make_unique<juce::DynamicObject>();
        value->setProperty("commandId", macro.commandId);
        value->setProperty("name", macro.name);
        juce::Array<juce::var> steps;
        for (const auto id : macro.steps)
            steps.add(id);
        value->setProperty("steps", juce::var(steps));
        values.add(juce::var(value.release()));
    }
    object->setProperty("macros", juce::var(values));
    return juce::var(object.release());
}

std::optional<WorkspaceSettings> WorkspaceSettings::fromVar(const juce::var& value, juce::String& error)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr || !value["keyMappingsXml"].isString() || !value["macros"].isArray()
        || value["macros"].getArray()->size() > maximumWorkflowMacros)
    {
        error = "Workspace settings require bounded shortcut and macro fields.";
        return std::nullopt;
    }
    WorkspaceSettings result;
    result.keyMappingsXml = value["keyMappingsXml"].toString();
    if (!value["selectedMacroId"].isVoid())
    {
        if (!value["selectedMacroId"].isInt())
        {
            error = "The selected workflow macro must be a command ID.";
            return std::nullopt;
        }
        result.selectedMacroId = static_cast<int>(value["selectedMacroId"]);
    }
    for (const auto& item : *value["macros"].getArray())
    {
        if (item.getDynamicObject() == nullptr || !item["commandId"].isInt() || !item["name"].isString()
            || !item["steps"].isArray() || item["steps"].getArray()->size() > maximumMacroSteps)
        {
            error = "A macro contains invalid or unbounded command fields.";
            return std::nullopt;
        }
        WorkflowMacro macro;
        macro.commandId = static_cast<int>(item["commandId"]);
        macro.name = item["name"].toString();
        for (const auto& step : *item["steps"].getArray())
        {
            if (!step.isInt())
            {
                error = "Macro steps must be Studio Duo command IDs.";
                return std::nullopt;
            }
            macro.steps.push_back(static_cast<int>(step));
        }
        result.macros.push_back(std::move(macro));
    }
    if (const auto valid = result.validate(); valid.failed())
    {
        error = valid.getErrorMessage();
        return std::nullopt;
    }
    return result;
}

juce::Result runWorkflowMacro(const WorkflowMacro& macro,
                              const std::function<bool(juce::CommandID)>& execute)
{
    if (const auto valid = macro.validate(); valid.failed())
        return valid;
    if (!execute)
        return juce::Result::fail("The workspace command handler is unavailable.");
    for (std::size_t index = 0; index < macro.steps.size(); ++index)
        if (!execute(macro.steps[index]))
            return juce::Result::fail("Macro " + macro.name + " stopped at step "
                + juce::String(static_cast<int>(index + 1)) + " ("
                + workspaceCommand(macro.steps[index])->name + "). Earlier edits remain undoable.");
    return juce::Result::ok();
}
}
