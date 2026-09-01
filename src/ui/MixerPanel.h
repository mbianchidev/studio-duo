#pragma once

#include "model/ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class MixerPanel final : public juce::Component
{
public:
    void setProject(const Project* value);
    void setSelection(const juce::String& value);
    void setPeaks(float left, float right);

    std::function<void(const juce::String&)> onTrackSelected;
    std::function<void(const juce::String&, juce::Rectangle<int>)> onEditTrack;
    std::function<void(const juce::String&, float)> onVolumeChanged;
    std::function<void(const juce::String&, float)> onPanChanged;

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

private:
    [[nodiscard]] std::vector<const Track*> mixerTracks() const;

    const Project* project = nullptr;
    juce::String selectedTrack;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;
    juce::String draggingVolumeTrack;
    juce::String draggingPanTrack;
    float dragStartY = 0.0f;
    float dragStartVolume = 0.0f;
    float dragPreviewVolume = 0.0f;
    float dragStartPan = 0.0f;
    float dragPreviewPan = 0.0f;
    int dragFaderHeight = 1;
};
}
