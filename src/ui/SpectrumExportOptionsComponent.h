#pragma once

#include "render/RenderEngine.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace studio
{
class SpectrumExportOptionsComponent final : public juce::Component
{
public:
    SpectrumExportOptionsComponent(Project, SpectrumExportSettings);
    ~SpectrumExportOptionsComponent() override;

    std::function<void(SpectrumExportSettings)> onExport;
    std::optional<SpectrumExportSettings> settings(juce::String& error) const;
    static void show(const Project&, const SpectrumExportSettings&, juce::Component&,
                     std::function<void(SpectrumExportSettings)>);

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusOfChildComponentChanged(FocusChangeType) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumExportOptionsComponent)
};
}
