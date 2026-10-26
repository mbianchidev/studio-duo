#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace studio
{
class ProjectNameLabel final : public juce::Label
{
public:
    ProjectNameLabel()
    {
        setEditable(false, true, false);
        setWantsKeyboardFocus(true);
        setTitle("Song name");
        setTooltip("Double-click or press Return / F2 to rename this song");
    }

    void setProjectName(const juce::String& name, bool hasUnsavedChanges)
    {
        projectName = name;
        dirty = hasUnsavedChanges;
        refresh();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (isEnabled()
            && (key == juce::KeyPress::returnKey || key == juce::KeyPress::F2Key))
        {
            showEditor();
            return true;
        }
        return juce::Label::keyPressed(key);
    }

    std::function<void(const juce::String&)> onRename;

private:
    void refresh()
    {
        if (!isBeingEdited())
            setText(projectName + (dirty ? " *" : ""), juce::dontSendNotification);
    }

    void editorShown(juce::TextEditor* textEditor) override
    {
        textEditor->setTitle("Song name");
        textEditor->setText(projectName, false);
        textEditor->selectAll();
        juce::Label::editorShown(textEditor);
    }

    void editorAboutToBeHidden(juce::TextEditor* textEditor) override
    {
        // Return and focus loss notify edits on different sides of hiding the editor.
        pendingName = textEditor->getText().trim();
        juce::Label::editorAboutToBeHidden(textEditor);
        refresh();
    }

    void textWasEdited() override
    {
        if (onRename && pendingName != projectName)
            onRename(pendingName);
        refresh();
    }

    void paint(juce::Graphics& graphics) override
    {
        juce::Label::paint(graphics);
        if (hasKeyboardFocus(false) && !isBeingEdited())
        {
            graphics.setColour(findColour(juce::TextEditor::focusedOutlineColourId));
            graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f);
        }
    }

    juce::String projectName;
    juce::String pendingName;
    bool dirty = false;
};

inline std::unique_ptr<juce::AlertWindow> createProjectNameDialog(
    const juce::String& suggestedName, juce::Component* owner)
{
    auto dialog = std::make_unique<juce::AlertWindow>(
        "New song",
        "Choose a name for this song. You can rename it later in the top bar.",
        juce::MessageBoxIconType::NoIcon, owner);
    dialog->addTextEditor("name", suggestedName, "Song name");
    dialog->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    auto* editor = dialog->getTextEditor("name");
    editor->setTitle("Song name");
    editor->setSelectAllWhenFocused(true);
    editor->onTextChange = [safe = juce::Component::SafePointer<juce::AlertWindow>(dialog.get())]
    {
        if (safe != nullptr)
        {
            const auto name = safe->getTextEditorContents("name").trim();
            safe->getButton("Create")->setEnabled(
                name.isNotEmpty() && juce::File::createLegalFileName(name).isNotEmpty());
        }
    };
    editor->onTextChange();
    return dialog;
}
}
