#pragma once

#include "audio/StudioAudioEngine.h"
#include "model/ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class MixerPanel final : public juce::Component
{
public:
    MixerPanel();
    ~MixerPanel() override;

    void setProject(const Project* value);
    void setSelection(const juce::String& value);
    void setPeaks(float left, float right);
    void setMeters(std::vector<StudioAudioEngine::TrackMeterSnapshot> value);

    std::function<void(const juce::String&)> onTrackSelected;
    std::function<void(const juce::String&, juce::Rectangle<int>)> onEditTrack;
    std::function<void(const juce::String&, float)> onVolumeChanged;
    std::function<void(const juce::String&, float)> onPanChanged;
    std::function<void(const juce::String&, const juce::String&)> onPluginOpen;
    std::function<void(const juce::String&, const juce::String&, bool)>
        onPluginEnabledChanged;
    std::function<void(const juce::String&, const juce::String&)> onRouteOpen;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

private:
    class ItemList;

    struct Item
    {
        enum class Type
        {
            plugin,
            route
        };

        Type type = Type::plugin;
        juce::String trackId;
        juce::String objectId;
        juce::String title;
        juce::String detail;
        bool enabled = true;
    };

    [[nodiscard]] std::vector<const Track*> mixerTracks() const;
    [[nodiscard]] std::vector<Item> items() const;
    void refreshItems();

    const Project* project = nullptr;
    juce::String selectedTrack;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;
    std::vector<StudioAudioEngine::TrackMeterSnapshot> meters;
    juce::String draggingVolumeTrack;
    juce::String draggingPanTrack;
    float dragStartY = 0.0f;
    float dragStartVolume = 0.0f;
    float dragPreviewVolume = 0.0f;
    float dragStartPan = 0.0f;
    float dragPreviewPan = 0.0f;
    int dragFaderHeight = 1;
    juce::Viewport itemsViewport;
    std::unique_ptr<ItemList> itemList;
};
}
