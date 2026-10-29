#pragma once

#include "PluginInsertPanel.h"
#include "plugin_host/PluginBrowserComponent.h"
#include "render/ArrangementRender.h"

#include <map>

namespace studio
{
class ClipEffectsComponent final : public juce::Component, private juce::Timer
{
public:
    ClipEffectsComponent(const Project&, const AudioClip&, PluginCatalog&, RenderEngine::RequestBuilder);
    ~ClipEffectsComponent() override;
    std::function<juce::Result(ClipEffectsSettings)> onApply;
    std::function<void()> onClose;
    void paint(juce::Graphics&) override;
    void resized() override;
    juce::Result applyChanges();
    [[nodiscard]] bool effectsReady() const;

private:
    void timerCallback() override;
    void choosePlugin(juce::String containerId, juce::String replacementId = {});
    void editParameters(const juce::String&);
    bool captureStates();
    void setChain(std::vector<PluginInsert>);
    void rebuildRuntime();
    std::vector<StudioAudioEngine::PluginRuntimeRequest> requests() const;
    void showStatus(const juce::String&, bool error = false);

    PluginCatalog& catalog;
    RenderEngine::RequestBuilder requestBuilder;
    Project effectsProject;
    std::unique_ptr<StudioAudioEngine> engine;
    std::map<juce::String, juce::MemoryBlock> states;
    double clipDuration;
    PluginInsertPanel rack;
    juce::Label title, rangeLabel, tailLabel, status;
    juce::TextEditor start, end, tail;
    juce::TextButton apply { "RENDER CLIP FX" }, cancel { "CANCEL" };
};
}
