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

    std::function<void(const juce::String&, int, float)> onValueChanged;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void selectParameter();

    juce::String name;
    juce::String id;
    std::vector<PluginParameterDescriptor> descriptors;
    bool rebuilding = false;
    juce::ComboBox parameter;
    juce::Slider value;
    juce::Label valueLabel;
};
}
