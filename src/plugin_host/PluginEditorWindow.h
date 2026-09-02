#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace studio
{
class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    static std::unique_ptr<PluginEditorWindow> create(
        juce::AudioProcessor& processor,
        const juce::String& title,
        juce::String& error);

    void showEditor();
    void hideEditor();
    void focusEditor();
    bool resizeEditor(int width, int height);
    void closeButtonPressed() override;

private:
    PluginEditorWindow(const juce::String& title,
                       juce::AudioProcessorEditor* editor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};
}
