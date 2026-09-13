#pragma once

#include "audio/StudioAudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class PluginParameterPanel final : public juce::Component
{
public:
    PluginParameterPanel(
        juce::String insertName,
        juce::String insertId,
        std::vector<PluginParameterDescriptor> parameters);

    using ParameterCallback = std::function<void(
        const juce::String&,
        const juce::String&,
        const juce::String&,
        int,
        float)>;

    ParameterCallback onValueChanged;
    ParameterCallback onGestureStarted;
    ParameterCallback onGestureEnded;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void selectParameter();
    void notify(const ParameterCallback& callback, float parameterValue);

    juce::String name;
    juce::String id;
    std::vector<PluginParameterDescriptor> descriptors;
    bool rebuilding = false;
    bool gestureActive = false;
    juce::ComboBox parameter;
    juce::Slider value;
    juce::Label valueLabel;
};
}
