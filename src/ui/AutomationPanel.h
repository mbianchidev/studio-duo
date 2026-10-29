#pragma once

#include "audio/StudioAudioEngine.h"
#include "model/ProjectCommands.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace studio
{
class AutomationPanel final : public juce::Component
{
public:
    explicit AutomationPanel(double playbackSampleRate = 48000.0);

    void setProject(Project* value);
    void setTrack(const juce::String& value);
    void setPositionSeconds(double value);
    void setRuntimeStatuses(
        std::vector<StudioAudioEngine::PluginRuntimeStatus> value);
    void refresh();

    std::function<void(const juce::String&, AutomationMode, bool)> onModeChanged;
    std::function<void(AutomationLane)> onAddLane;
    std::function<juce::Result(AutomationLane, AutomationLane)> onUpdateLane;
    std::function<void(const juce::String&)> onRemoveLane;
    std::function<void()> onCommitPreview;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    struct TargetItem
    {
        juce::String label;
        AutomationTarget target;
    };

    [[nodiscard]] AutomationLane* selectedLane() const;
    [[nodiscard]] const AutomationPoint* selectedPoint() const;
    [[nodiscard]] double lanePosition(
        const AutomationLane& lane) const;
    void refreshPoints();
    void refreshPointValues();
    [[nodiscard]] std::optional<double> editedValue();
    juce::Result showError(juce::String message, juce::Component& field);
    juce::Result updateLane(AutomationLane before, AutomationLane after,
                            juce::String pointToSelect = {});
    void addLane();
    void addPoint();
    void updateSelectedPoint();
    void removeSelectedPoint();
    void removeNearestPoint();
    void removeLane();

    const double sampleRate;
    Project* project = nullptr;
    juce::String trackId;
    double positionSeconds = 0.0;
    std::vector<StudioAudioEngine::PluginRuntimeStatus> runtimeStatuses;
    std::vector<TargetItem> targets;
    std::vector<juce::String> laneIds;
    std::vector<juce::String> pointIds;
    juce::String selectedPointId;
    bool rebuilding = false;

    juce::ComboBox mode;
    juce::ToggleButton armed { "WRITE ARM" };
    juce::ComboBox lanes;
    juce::ComboBox target;
    juce::ComboBox points;
    juce::ToggleButton beatTime { "BEAT TIME" };
    juce::ToggleButton stepInterpolation { "STEP" };
    juce::Slider value;
    juce::TextEditor pointPosition;
    juce::TextEditor pointValue;
    juce::Label positionLabel;
    juce::Label valueLabel;
    juce::Label status;
    juce::TextButton updatePointButton { "APPLY POINT" };
    juce::TextButton removeSelectedPointButton { "REMOVE SELECTED" };
    juce::TextButton addLaneButton { "ADD LANE" };
    juce::TextButton addPointButton { "ADD POINT" };
    juce::TextButton removePointButton { "REMOVE NEAR" };
    juce::TextButton removeLaneButton { "DELETE LANE" };
    juce::TextButton commitPreviewButton { "COMMIT PREVIEW" };
};
}
