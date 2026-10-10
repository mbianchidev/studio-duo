#pragma once

#include "devices/DrumDeviceProcessor.h"
#include "plugin_host/PluginBridgeProtocol.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace studio
{
class DrumKitTuningComponent final : public juce::Component,
                                     private juce::Timer
{
public:
    explicit DrumKitTuningComponent(
        std::vector<PluginParameterDescriptor> parameters, int drum = 0);
    ~DrumKitTuningComponent() override;

    void setParameters(std::vector<PluginParameterDescriptor> parameters);
    void setSelectedDrum(int drum);

    using GestureCallback = std::function<void(const PluginParameterDescriptor&, float)>;
    std::function<juce::Result(const PluginParameterDescriptor&, float)> onValueChanged;
    GestureCallback onGestureStarted;
    GestureCallback onGestureEnded;
    std::function<void(const PluginParameterDescriptor&)> onGestureCancelled;
    std::function<std::vector<PluginParameterDescriptor>()> onRefreshParameters;
    std::function<juce::Result(int)> onAudition;
    std::function<void(const juce::String&, bool)> onStatus;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshControls();
    void startGesture(int control);
    void finishGesture(bool cancelled);
    juce::Result changeValue(int control, double value);
    void resetSelectedDrum();
    void report(const juce::Result& result);
    const DrumDeviceProcessor::KitTuningParameter* definition(int control) const;
    const PluginParameterDescriptor* parameter(int control) const;

    std::vector<PluginParameterDescriptor> parameters;
    std::optional<PluginParameterDescriptor> activeGesture;
    bool gestureChanged = false;
    bool refreshing = false;
    int selectedDrum = 0;
    juce::ComboBox drumSelector;
    std::array<juce::Label, 4> labels;
    std::array<juce::Slider, 4> sliders;
    juce::TextButton tapButton { "TAP DRUM" };
    juce::TextButton resetButton { "RESET THIS DRUM" };
    juce::Label hint;
    juce::Label status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumKitTuningComponent)
};
}
