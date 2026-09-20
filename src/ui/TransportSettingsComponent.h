#pragma once

#include "model/TransportEditing.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <optional>

namespace studio
{
class LoopSettingsComponent final : public juce::Component
{
public:
    LoopSettingsComponent(
        Project project,
        double sampleRate,
        std::optional<juce::Range<double>> selectedRange = {});
    ~LoopSettingsComponent() override;

    [[nodiscard]] std::optional<LoopRangeSettings> settings(juce::String& error) const;
    std::function<void(LoopRangeSettings)> onApply;

    static void show(
        const Project& project,
        double sampleRate,
        std::optional<juce::Range<double>> selectedRange,
        juce::Component& centreAround,
        std::function<void(LoopRangeSettings)> handler);

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusOfChildComponentChanged(FocusChangeType) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopSettingsComponent)
};

class SectionSettingsComponent final : public juce::Component
{
public:
    SectionSettingsComponent(Project project, juce::String sectionId);
    ~SectionSettingsComponent() override;

    [[nodiscard]] std::optional<SectionTransportSettings> settings(juce::String& error) const;
    std::function<void(SectionTransportSettings)> onApply;

    static void show(
        const Project& project,
        const juce::String& sectionId,
        juce::Component& centreAround,
        std::function<void(SectionTransportSettings)> handler);

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusOfChildComponentChanged(FocusChangeType) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionSettingsComponent)
};
}
