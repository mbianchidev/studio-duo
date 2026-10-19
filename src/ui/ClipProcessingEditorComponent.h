#pragma once

#include "model/ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class ClipProcessingEditorComponent final : public juce::Component
{
public:
    enum class Mode { gain, fadeIn, fadeOut };
    ClipProcessingEditorComponent(const AudioClip& clip, Mode mode);
    juce::Result applyChanges();
    void focusValue();
    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;

    std::function<juce::Result(double, float)> onApply;
    std::function<void()> onClose;

private:
    juce::Result showError(juce::String message, juce::TextEditor& field);
    Mode mode;
    double duration;
    juce::Label valueLabel, curveLabel, status;
    juce::TextEditor value, curve;
    juce::TextButton applyButton { "APPLY" }, cancelButton { "CANCEL" };
};
}
