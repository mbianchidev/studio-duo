#pragma once

#include "audio/StudioAudioEngine.h"
#include "model/ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class PluginInsertPanel final : public juce::Component
{
public:
    void setProject(const Project* value);
    void setTrack(const juce::String& value);
    void setRuntimeStatuses(std::vector<StudioAudioEngine::PluginRuntimeStatus> value,
                            std::uint64_t lateBlocks);

    std::function<void(const juce::String&, const juce::String&, bool)> onBypass;
    std::function<void(const juce::String&, const juce::String&)> onRemove;
    std::function<void(const juce::String&,
                       const juce::String&,
                       PluginBridgeMode)> onModeChange;
    std::function<void(const juce::String&, const juce::String&)> onReplace;
    std::function<void(const juce::String&, const juce::String&)> onEdit;
    std::function<void(const juce::String&, const juce::String&)>
        onOpenEditor;
    std::function<void(const juce::String&, const juce::String&)> onReload;

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

private:
    const Project* project = nullptr;
    juce::String trackId;
    std::vector<StudioAudioEngine::PluginRuntimeStatus> runtimeStatuses;
    std::uint64_t lateBlockCount = 0;
};
}
