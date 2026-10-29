#include "TestHarness.h"
#include "TestSuites.h"

#include "workflow/WorkspaceCommands.h"
#include "update/StudioPreferences.h"
#include "ui/WorkflowSettingsComponent.h"
#include "ReleaseFixtures.h"

namespace
{
class CommandTarget final : public juce::ApplicationCommandTarget
{
public:
    explicit CommandTarget(const studio::WorkspaceSettings& settings) : configuration(settings) {}
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& result) override
    {
        for (const auto& descriptor : studio::workspaceCommands())
            result.add(static_cast<juce::CommandID>(descriptor.command));
        for (const auto& macro : configuration.macros)
            result.add(macro.commandId);
    }
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override
    {
        if (const auto* descriptor = studio::workspaceCommand(id))
        {
            info.setInfo(descriptor->name, descriptor->name, descriptor->category, 0);
            for (const auto& key : descriptor->defaults)
                info.addDefaultKeypress(key.getKeyCode(), key.getModifiers());
        }
        else
            info.setInfo("Synthetic macro", "Synthetic macro", "Macros", 0);
    }
    bool perform(const InvocationInfo& info) override { invoked = info.commandID; return true; }
    juce::CommandID invoked = 0;
private:
    const studio::WorkspaceSettings& configuration;
};

void nativeMappingsAndMacroEditor(const studio::WorkflowMacro& macro)
{
    release_fixture::Directory directory;
    const auto file = directory.file.getChildFile("preferences.json");
    studio::StudioPreferences preferences(file, false);
    studio::WorkspaceSettings settings;
    settings.macros = { macro };
    expect(preferences.setWorkspaceSettings(settings).wasOk(), "Synthetic workflow preferences can be written.");
    CommandTarget target(preferences.workspaceSettings());
    juce::ApplicationCommandManager manager;
    manager.setFirstCommandTarget(&target);
    manager.registerAllCommandsForTarget(&target);
    auto& mappings = *manager.getKeyMappings();
    mappings.resetToDefaultMappings();
    const auto automationId = static_cast<juce::CommandID>(studio::WorkspaceCommand::toggleAutomation);
    expect(mappings.findCommandForKeyPress(juce::KeyPress('A')) == automationId,
           "The actual JUCE command manager resolves default timeline automation.");
    mappings.removeKeyPress(juce::KeyPress('A'));
    mappings.addKeyPress(automationId, juce::KeyPress('U'));
    mappings.addKeyPress(macro.commandId, juce::KeyPress('K', juce::ModifierKeys::commandModifier, 0));
    settings.keyMappingsXml = mappings.createXml(true)->toString();
    expect(preferences.setWorkspaceSettings(settings).wasOk(), "Custom native shortcuts and macro bindings are persisted.");
    studio::StudioPreferences reopened(file);
    CommandTarget reopenedTarget(reopened.workspaceSettings());
    juce::ApplicationCommandManager restored;
    restored.setFirstCommandTarget(&reopenedTarget);
    restored.registerAllCommandsForTarget(&reopenedTarget);
    const auto xml = juce::parseXML(reopened.workspaceSettings().keyMappingsXml);
    expect(xml != nullptr && restored.getKeyMappings()->restoreFromXml(*xml)
               && restored.getKeyMappings()->findCommandForKeyPress(juce::KeyPress('A')) == 0
               && restored.getKeyMappings()->findCommandForKeyPress(juce::KeyPress('U')) == automationId
               && restored.getKeyMappings()->findCommandForKeyPress(
                   juce::KeyPress('K', juce::ModifierKeys::commandModifier, 0)) == macro.commandId
               && restored.invokeDirectly(macro.commandId, false) && reopenedTarget.invoked == macro.commandId,
           "Restart preserves rebinding, explicitly unbound defaults and command-based macro shortcuts.");
    const auto previous = reopened.workspaceSettings().keyMappingsXml;
    auto invalid = reopened.workspaceSettings();
    invalid.keyMappingsXml = "<not-native/>";
    expect(reopened.setWorkspaceSettings(invalid).failed()
               && reopened.workspaceSettings().keyMappingsXml == previous,
           "Invalid mappings fail without changing the saved or in-memory settings.");

    int saved = 0;
    studio::WorkflowSettingsComponent editor(restored, reopened, [&] { ++saved; });
    editor.setSize(680, 486);
    auto* tabs = dynamic_cast<juce::TabbedComponent*>(editor.getChildComponent(0));
    expect(tabs != nullptr && tabs->getNumTabs() == 2, "Native workflow settings expose shortcut and macro tabs.");
    if (tabs == nullptr)
        return;
    tabs->setCurrentTabIndex(1);
    auto* page = tabs->getTabContentComponent(1);
    auto* name = dynamic_cast<juce::TextEditor*>(page->findChildWithID("workflow.macro-name"));
    juce::ComboBox* choice = nullptr;
    juce::Button* add = nullptr;
    juce::Button* save = nullptr;
    for (auto* child : page->getChildren())
    {
        if (child->getTitle() == "Command to add to macro")
            choice = dynamic_cast<juce::ComboBox*>(child);
        if (auto* button = dynamic_cast<juce::Button*>(child))
        {
            if (button->getButtonText() == "ADD") add = button;
            if (button->getButtonText() == "SAVE") save = button;
        }
    }
    expect(name != nullptr && choice != nullptr && add != nullptr && save != nullptr,
           "The real macro editor has named native controls for names, steps and saving.");
    if (name != nullptr && choice != nullptr && add != nullptr && save != nullptr)
    {
        name->setText("Synthetic revised workflow", false);
        choice->setSelectedId(automationId, juce::dontSendNotification);
        add->onClick();
        save->onClick();
        const auto& edited = reopened.workspaceSettings().macros.front();
        expect(saved == 1 && edited.name == "Synthetic revised workflow"
                   && edited.steps.size() == macro.steps.size() + 1 && edited.steps.back() == automationId
                   && studio::StudioPreferences(file).workspaceSettings().macros.front().name == edited.name,
               "The native macro editor saves real command sequences through the persistent preferences path.");
    }
}
}

void workspaceCommandTests()
{
    using Command = studio::WorkspaceCommand;
    const auto* automation = studio::workspaceCommand(static_cast<juce::CommandID>(Command::toggleAutomation));
    expect(automation != nullptr && automation->defaults.size() == 1
               && automation->defaults.front() == juce::KeyPress('A'),
           "Automation visibility has a discoverable, customizable A binding.");
    studio::WorkflowMacro macro;
    macro.name = "Synthetic edit workflow";
    macro.steps = { static_cast<juce::CommandID>(Command::split),
                    static_cast<juce::CommandID>(Command::nextClip),
                    static_cast<juce::CommandID>(Command::gainDown) };
    std::vector<juce::CommandID> executed;
    auto result = studio::runWorkflowMacro(macro, [&](auto id) { executed.push_back(id); return true; });
    expect(result.wasOk() && executed == macro.steps, "A macro executes the chosen Studio Duo commands in their exact order.");
    executed.clear();
    result = studio::runWorkflowMacro(macro, [&](auto id)
    {
        executed.push_back(id);
        return executed.size() < 2;
    });
    expect(result.failed() && executed.size() == 2 && result.getErrorMessage().contains("2"),
           "A failed macro step stops the sequence and reports its one-based step.");
    auto recursive = macro;
    recursive.steps = { studio::firstMacroCommandId };
    expect(recursive.validate().failed(), "Macros cannot recursively call themselves or other macros.");
    recursive.steps = { static_cast<juce::CommandID>(Command::open) };
    expect(recursive.validate().failed(), "Macros cannot continue through asynchronous file dialogs or external applications.");
    studio::WorkspaceSettings settings;
    settings.macros = { macro };
    settings.keyMappingsXml = "<KEYMAPPINGS basedOnDefaults=\"1\"/>";
    juce::String error;
    const auto restored = studio::WorkspaceSettings::fromVar(settings.toVar(), error);
    expect(restored && restored->macros.size() == 1
               && restored->macros.front().steps == macro.steps
               && restored->keyMappingsXml == settings.keyMappingsXml,
           "Macro commands and native key mappings survive a strict settings round-trip.");
    nativeMappingsAndMacroEditor(macro);
    const auto automationId = static_cast<juce::CommandID>(Command::toggleAutomation);
    expect(!studio::workspaceShortcutAllowed(automationId, juce::KeyPress('A'), { true, false, false })
               && !studio::workspaceShortcutAllowed(automationId, juce::KeyPress('A'), { false, true, false })
               && studio::workspaceShortcutAllowed(automationId, juce::KeyPress('A'), { false, false, true }),
           "Automation shortcuts never steal text entry or drum-pad keys from focused editors.");
    const auto focusId = static_cast<juce::CommandID>(Command::focusTimeline);
    expect(studio::workspaceShortcutAllowed(focusId, juce::KeyPress(juce::KeyPress::F6Key), { true, false, false })
               && !studio::workspaceShortcutAllowed(focusId, juce::KeyPress('A'), { true, false, false }),
           "F6 can leave a text field, but remapping focus to a letter cannot steal typed text.");
}
