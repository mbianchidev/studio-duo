#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace studio
{
struct StudioColours
{
    static constexpr std::uint32_t window = 0xff101214;
    static constexpr std::uint32_t panel = 0xff171a1d;
    static constexpr std::uint32_t raised = 0xff22262a;
    static constexpr std::uint32_t border = 0xff30353a;
    static constexpr std::uint32_t text = 0xffe7e4df;
    static constexpr std::uint32_t secondaryText = 0xff8f969c;
    static constexpr std::uint32_t orange = 0xffdd5b3f;
    static constexpr std::uint32_t amber = 0xffd99a42;
    static constexpr std::uint32_t green = 0xff78c6a3;
};

class StudioTheme final : public juce::LookAndFeel_V4
{
public:
    StudioTheme()
    {
        setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(StudioColours::window));
        setColour(juce::TextButton::buttonColourId, juce::Colour(StudioColours::raised));
        setColour(juce::TextButton::buttonOnColourId, juce::Colour(StudioColours::orange));
        setColour(juce::TextButton::textColourOffId, juce::Colour(StudioColours::text));
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::ToggleButton::textColourId, juce::Colour(StudioColours::text));
        setColour(juce::Label::textColourId, juce::Colour(StudioColours::text));
        setColour(juce::Slider::backgroundColourId, juce::Colour(StudioColours::window));
        setColour(juce::Slider::trackColourId, juce::Colour(StudioColours::orange));
        setColour(juce::Slider::thumbColourId, juce::Colour(StudioColours::text));
        setColour(juce::ScrollBar::thumbColourId, juce::Colour(StudioColours::border));
        setColour(juce::ScrollBar::trackColourId, juce::Colour(StudioColours::window));
        setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(StudioColours::raised));
        setColour(juce::TooltipWindow::textColourId, juce::Colour(StudioColours::text));
        setColour(juce::TooltipWindow::outlineColourId, juce::Colour(StudioColours::border));
    }

    void drawButtonBackground(juce::Graphics& graphics,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool highlighted,
                              bool down) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        auto colour = backgroundColour;
        if (highlighted)
            colour = colour.brighter(0.08f);
        if (down)
            colour = colour.darker(0.12f);

        graphics.setColour(colour);
        graphics.fillRoundedRectangle(bounds, 4.0f);
        graphics.setColour(juce::Colour(StudioColours::border));
        graphics.drawRoundedRectangle(bounds, 4.0f, 1.0f);
    }
};
}
