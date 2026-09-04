#include "PluginEditorWindow.h"

namespace studio
{
std::unique_ptr<PluginEditorWindow> PluginEditorWindow::create(
    juce::AudioProcessor& processor,
    const juce::String& title,
    juce::String& error)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        error = "Plugin editors must be opened on the message thread.";
        return {};
    }

    auto* editor = processor.hasEditor()
        ? processor.createEditorAndMakeActive()
        : nullptr;
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor(processor);
    if (editor == nullptr)
    {
        error = "The plugin did not provide an editor.";
        return {};
    }
    return std::unique_ptr<PluginEditorWindow>(
        new PluginEditorWindow(title, editor));
}

PluginEditorWindow::PluginEditorWindow(
    const juce::String& title,
    juce::AudioProcessorEditor* editor)
    : juce::DocumentWindow(
          title,
          juce::Colour(0xff181b1f),
          juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setContentOwned(editor, true);
    setResizable(editor->isResizable(), false);
    setResizeLimits(
        240,
        160,
        4096,
        4096);
    centreWithSize(
        std::max(240, editor->getWidth()),
        std::max(160, editor->getHeight()));
}

PluginEditorWindow::~PluginEditorWindow()
{
    if (auto* editor =
            dynamic_cast<juce::AudioProcessorEditor*>(
                getContentComponent()))
    {
        editor->getAudioProcessor()->editorBeingDeleted(editor);
    }
}

void PluginEditorWindow::showEditor()
{
    setVisible(true);
    toFront(true);
}

void PluginEditorWindow::hideEditor()
{
    setVisible(false);
}

void PluginEditorWindow::focusEditor()
{
    if (isVisible())
        toFront(true);
}

bool PluginEditorWindow::resizeEditor(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    setSize(
        juce::jlimit(240, 4096, width),
        juce::jlimit(160, 4096, height));
    return true;
}

void PluginEditorWindow::closeButtonPressed()
{
    hideEditor();
}
}
