#pragma once

#include "audio/StudioAudioEngine.h"
#include "model/ProjectCommands.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class AutomationPanel final : public juce::Component
{
public:
    AutomationPanel();

    void setProject(Project* value);
    void setTrack(const juce::String& value);
    void setPositionSeconds(double value);
    void setRuntimeStatuses(
        std::vector<StudioAudioEngine::PluginRuntimeStatus> value);
    void refresh();

    std::function<void(const juce::String&, AutomationMode, bool)> onModeChanged;
    std::function<void(AutomationLane)> onAddLane;
    std::function<void(AutomationLane, AutomationLane)> onUpdateLane;
    std::function<void(const juce::String&)> onRemoveLane;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    struct TargetItem
    {
        juce::String label;
        AutomationTarget target;
    };

    [[nodiscard]] AutomationLane* selectedLane() const;
    [[nodiscard]] double lanePosition(
        const AutomationLane& lane) const;
    void addLane();
    void addPoint();
    void removeNearestPoint();
    void removeLane();

    Project* project = nullptr;
    juce::String trackId;
    double positionSeconds = 0.0;
    std::vector<StudioAudioEngine::PluginRuntimeStatus> runtimeStatuses;
    std::vector<TargetItem> targets;
    std::vector<juce::String> laneIds;
    bool rebuilding = false;

    juce::ComboBox mode;
    juce::ToggleButton armed { "WRITE ARM" };
    juce::ComboBox lanes;
    juce::ComboBox target;
    juce::ToggleButton beatTime { "BEAT TIME" };
    juce::ToggleButton stepInterpolation { "STEP" };
    juce::Slider value;
    juce::TextButton addLaneButton { "ADD LANE" };
    juce::TextButton addPointButton { "ADD POINT" };
    juce::TextButton removePointButton { "REMOVE NEAR" };
    juce::TextButton removeLaneButton { "DELETE LANE" };
};
}
