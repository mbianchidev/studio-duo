#include "WorkflowSettingsComponent.h"

#include "StudioTheme.h"
#include "logging/StudioLogger.h"

namespace studio
{
namespace
{
class MacroEditor final : public juce::Component, private juce::ListBoxModel
{
public:
    MacroEditor(StudioPreferences& settings, std::function<void()> callback)
        : preferences(settings), changed(std::move(callback)), steps("Macro steps", this)
    {
        for (auto* component : std::array<juce::Component*, 12> {
                &saved, &name, &commands, &steps, &newButton, &saveButton, &deleteButton,
                &addButton, &removeButton, &upButton, &downButton, &status })
            addAndMakeVisible(*component);
        saved.setTitle("Saved workflow macro");
        name.setTitle("Workflow macro name");
        name.setTextToShowWhenEmpty("Macro name", juce::Colour(StudioColours::secondaryText));
        name.setComponentID("workflow.macro-name");
        commands.setTitle("Command to add to macro");
        steps.setTitle("Ordered macro command sequence");
        steps.setRowHeight(28);
        steps.setWantsKeyboardFocus(true);
        status.setTitle("Macro editor status");
        status.setJustificationType(juce::Justification::topLeft);
        for (const auto& descriptor : workspaceCommands())
            if (descriptor.macroSafe)
                commands.addItem(juce::String(descriptor.category) + ": " + descriptor.name,
                                 static_cast<juce::CommandID>(descriptor.command));
        commands.setSelectedItemIndex(0, juce::dontSendNotification);
        saved.onChange = [this] { load(); };
        newButton.onClick = [this] { createDraft(); };
        saveButton.onClick = [this] { save(); };
        deleteButton.onClick = [this] { removeMacro(); };
        addButton.onClick = [this]
        {
            if (draft.steps.size() >= static_cast<std::size_t>(maximumMacroSteps))
            {
                showStatus("A macro can contain at most 64 commands.", true);
                return;
            }
            const auto id = commands.getSelectedId();
            if (id == 0)
                return;
            const auto index = steps.getSelectedRow() < 0 ? draft.steps.size()
                : static_cast<std::size_t>(steps.getSelectedRow() + 1);
            draft.steps.insert(draft.steps.begin() + static_cast<std::ptrdiff_t>(index), id);
            steps.updateContent();
            steps.selectRow(static_cast<int>(index));
        };
        removeButton.onClick = [this]
        {
            const auto row = steps.getSelectedRow();
            if (row >= 0 && row < static_cast<int>(draft.steps.size()))
            {
                draft.steps.erase(draft.steps.begin() + row);
                steps.updateContent();
                steps.selectRow(std::min(row, static_cast<int>(draft.steps.size()) - 1));
                updateButtons();
            }
        };
        upButton.onClick = [this] { move(-1); };
        downButton.onClick = [this] { move(1); };
        refresh();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        auto top = bounds.removeFromTop(30);
        saved.setBounds(top.removeFromLeft(top.getWidth() - 208).reduced(2));
        newButton.setBounds(top.removeFromLeft(60).reduced(2));
        saveButton.setBounds(top.removeFromLeft(70).reduced(2));
        deleteButton.setBounds(top.reduced(2));
        name.setBounds(bounds.removeFromTop(30).reduced(2));
        auto choose = bounds.removeFromTop(32);
        commands.setBounds(choose.removeFromLeft(choose.getWidth() - 66).reduced(2));
        addButton.setBounds(choose.reduced(2));
        status.setBounds(bounds.removeFromBottom(52));
        auto buttons = bounds.removeFromBottom(30);
        removeButton.setBounds(buttons.removeFromLeft(150).reduced(2));
        upButton.setBounds(buttons.removeFromLeft(74).reduced(2));
        downButton.setBounds(buttons.removeFromLeft(74).reduced(2));
        steps.setBounds(bounds.reduced(2));
    }

private:
    int getNumRows() override { return static_cast<int>(draft.steps.size()); }
    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        graphics.fillAll(juce::Colour(selected ? StudioColours::raised : StudioColours::panel));
        graphics.setColour(juce::Colour(StudioColours::text));
        if (row >= 0 && row < getNumRows())
            if (const auto* descriptor = workspaceCommand(draft.steps[static_cast<std::size_t>(row)]))
                graphics.drawText(juce::String(row + 1) + ". " + descriptor->name,
                                  8, 0, width - 16, height, juce::Justification::centredLeft);
    }
    void selectedRowsChanged(int) override { updateButtons(); }
    void updateButtons()
    {
        const auto row = steps.getSelectedRow();
        removeButton.setEnabled(row >= 0 && row < getNumRows());
        upButton.setEnabled(row > 0 && row < getNumRows());
        downButton.setEnabled(row >= 0 && row + 1 < getNumRows());
        deleteButton.setEnabled(saved.getSelectedId() != 0);
    }
    void move(int direction)
    {
        const auto row = steps.getSelectedRow();
        if (row < 0 || row + direction < 0 || row + direction >= getNumRows())
            return;
        std::swap(draft.steps[static_cast<std::size_t>(row)], draft.steps[static_cast<std::size_t>(row + direction)]);
        steps.updateContent();
        steps.selectRow(row + direction);
        steps.repaint();
    }
    void showStatus(const juce::String& text, bool error = false)
    {
        status.setText((error ? "Error: " : "") + text, juce::dontSendNotification);
        if (error)
            logError("workflow.settings", text);
    }
    void load()
    {
        const auto& macros = preferences.workspaceSettings().macros;
        const auto found = std::find_if(macros.begin(), macros.end(),
            [&](const auto& macro) { return macro.commandId == saved.getSelectedId(); });
        if (found == macros.end())
            return;
        draft = *found;
        name.setText(draft.name, false);
        steps.updateContent();
        steps.deselectAllRows();
        steps.repaint();
        updateButtons();
    }
    void refresh(juce::CommandID selectedId = 0)
    {
        saved.clear(juce::dontSendNotification);
        for (const auto& macro : preferences.workspaceSettings().macros)
            saved.addItem(macro.name, macro.commandId);
        if (saved.getNumItems() > 0)
        {
            if (selectedId != 0)
                saved.setSelectedId(selectedId, juce::dontSendNotification);
            if (saved.getSelectedId() == 0)
                saved.setSelectedItemIndex(0, juce::dontSendNotification);
            load();
        }
        else
            createDraft();
        showStatus("Commands run in order inside Studio Duo and stop on failure. "
                   "Choose a saved macro in the toolbar and press Run, or bind it in Shortcuts.");
    }
    void createDraft()
    {
        const auto& macros = preferences.workspaceSettings().macros;
        for (int slot = 0; slot < maximumWorkflowMacros; ++slot)
        {
            const auto id = firstMacroCommandId + slot;
            if (std::none_of(macros.begin(), macros.end(), [&](const auto& macro) { return macro.commandId == id; }))
            {
                draft = WorkflowMacro {};
                draft.commandId = id;
                saved.setSelectedId(0, juce::dontSendNotification);
                name.clear();
                steps.updateContent();
                steps.deselectAllRows();
                steps.repaint();
                updateButtons();
                return;
            }
        }
        showStatus("All 32 macro slots are used. Edit or delete an existing macro.", true);
    }
    void save()
    {
        draft.name = name.getText().trim();
        auto settings = preferences.workspaceSettings();
        const auto found = std::find_if(settings.macros.begin(), settings.macros.end(),
            [&](const auto& macro) { return macro.commandId == draft.commandId; });
        if (found == settings.macros.end())
            settings.macros.push_back(draft);
        else
            *found = draft;
        if (settings.selectedMacroId == 0)
            settings.selectedMacroId = draft.commandId;
        if (const auto result = preferences.setWorkspaceSettings(settings); result.failed())
        {
            showStatus(result.getErrorMessage(), true);
            return;
        }
        if (changed)
            changed();
        refresh(draft.commandId);
    }
    void removeMacro()
    {
        auto settings = preferences.workspaceSettings();
        if (settings.selectedMacroId == saved.getSelectedId())
            settings.selectedMacroId = 0;
        std::erase_if(settings.macros, [&](const auto& macro) { return macro.commandId == saved.getSelectedId(); });
        if (const auto result = preferences.setWorkspaceSettings(settings); result.failed())
        {
            showStatus(result.getErrorMessage(), true);
            return;
        }
        if (changed)
            changed();
        refresh();
    }

    StudioPreferences& preferences;
    std::function<void()> changed;
    WorkflowMacro draft;
    juce::ComboBox saved, commands;
    juce::TextEditor name;
    juce::ListBox steps;
    juce::Label status;
    juce::TextButton newButton { "NEW" }, saveButton { "SAVE" }, deleteButton { "DELETE" },
        addButton { "ADD" }, removeButton { "REMOVE STEP" }, upButton { "UP" }, downButton { "DOWN" };
};
}

WorkflowSettingsComponent::WorkflowSettingsComponent(
    juce::ApplicationCommandManager& manager, StudioPreferences& preferences, std::function<void()> changed)
    : shortcuts(*manager.getKeyMappings(), true), macros(std::make_unique<MacroEditor>(preferences, std::move(changed)))
{
    shortcuts.setTitle("Customizable Studio Duo shortcuts");
    addAndMakeVisible(tabs);
    tabs.addTab("Shortcuts", juce::Colour(StudioColours::panel), &shortcuts, false);
    tabs.addTab("Macros", juce::Colour(StudioColours::panel), macros.get(), false);
}

WorkflowSettingsComponent::~WorkflowSettingsComponent() { tabs.clearTabs(); }
void WorkflowSettingsComponent::resized() { tabs.setBounds(getLocalBounds()); }
}
