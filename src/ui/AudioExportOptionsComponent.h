#pragma once

#include "render/RenderEngine.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <optional>

namespace studio
{
class AudioExportOptionsComponent final : public juce::Component
{
public:
    AudioExportOptionsComponent(
        std::optional<Project> projectForRanges,
        MixExportSettings initialSettings);
    ~AudioExportOptionsComponent() override;

    std::function<void(MixExportSettings)> onExport;

    [[nodiscard]] std::optional<MixExportSettings> settings(
        juce::String& error) const;

    static void show(
        const Project* projectForRanges,
        const MixExportSettings& initialSettings,
        juce::Component& centreAround,
        std::function<void(MixExportSettings)> onExport);

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusOfChildComponentChanged(FocusChangeType) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioExportOptionsComponent)
};
}
