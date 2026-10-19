#pragma once

#include "midi/ControlSurface.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace studio
{
class ControlSurfaceSettingsComponent final : public juce::Component, private juce::Timer
{
public:
    ControlSurfaceSettingsComponent(ControlSurfaceSession&, const ControlSurfaceSettings&,
        std::function<juce::Result(const ControlSurfaceSettings&)> apply);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshPorts(const ControlSurfaceSettings&);
    [[nodiscard]] ControlSurfaceSettings selectedSettings() const;
    void refreshStatus();
    ControlSurfaceSession& session;
    std::function<juce::Result(const ControlSurfaceSettings&)> apply;
    juce::StringArray inputIds, outputIds;
    juce::String localError;
    juce::Label title, help, protocolLabel, inputLabel, outputLabel, connectionStatus;
    juce::ComboBox protocol, input, output;
    juce::TextButton refresh { "REFRESH PORTS" }, reconnect { "RECONNECT" }, save { "APPLY" };
};
}
