#pragma once

#include "StudioTheme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace studio
{
struct StudioPanGeometry
{
    juce::Rectangle<float> track;
    juce::Rectangle<float> fill;
    float centreX = 0.0f;
    float knobX = 0.0f;
};

inline StudioPanGeometry studioPanGeometry(
    juce::Rectangle<float> bounds,
    float pan)
{
    bounds = bounds.reduced(18.0f, 5.0f);
    const auto centreX = bounds.getCentreX();
    const auto knobX = juce::jmap(
        juce::jlimit(-1.0f, 1.0f, pan),
        -1.0f,
        1.0f,
        bounds.getX(),
        bounds.getRight());
    const auto fillX = juce::jmin(centreX, knobX);
    return {
        {
            bounds.getX(),
            bounds.getCentreY() - 2.5f,
            bounds.getWidth(),
            5.0f
        },
        {
            fillX,
            bounds.getCentreY() - 2.5f,
            std::abs(knobX - centreX),
            5.0f
        },
        centreX,
        knobX
    };
}

inline void drawStudioPanControl(
    juce::Graphics& graphics,
    juce::Rectangle<float> bounds,
    float pan,
    juce::Colour accent,
    bool enabled,
    bool showLabels = true)
{
    const auto geometry =
        studioPanGeometry(bounds, pan);
    graphics.setColour(
        juce::Colour(StudioColours::window));
    graphics.fillRoundedRectangle(
        geometry.track,
        2.5f);
    graphics.setColour(
        juce::Colour(StudioColours::border));
    graphics.drawRoundedRectangle(
        geometry.track,
        2.5f,
        1.0f);

    if (geometry.fill.getWidth() > 0.5f)
    {
        graphics.setColour(
            accent.withMultipliedAlpha(
                enabled ? 0.95f : 0.4f));
        graphics.fillRoundedRectangle(
            geometry.fill,
            2.5f);
    }

    graphics.setColour(
        juce::Colour(StudioColours::secondaryText));
    graphics.drawVerticalLine(
        static_cast<int>(std::round(geometry.centreX)),
        geometry.track.getY() - 4.0f,
        geometry.track.getBottom() + 4.0f);

    graphics.setColour(
        enabled
            ? juce::Colour(StudioColours::text)
            : juce::Colour(StudioColours::secondaryText)
                  .withMultipliedAlpha(0.5f));
    graphics.fillRoundedRectangle(
        geometry.knobX - 5.0f,
        geometry.track.getCentreY() - 7.0f,
        10.0f,
        14.0f,
        3.0f);

    if (showLabels)
    {
        graphics.setColour(
            juce::Colour(StudioColours::secondaryText));
        graphics.setFont(
            juce::Font(
                juce::FontOptions(8.0f,
                                  juce::Font::bold)));
        auto labelBounds = bounds.toNearestInt();
        graphics.drawText(
            "L",
            labelBounds.removeFromLeft(14),
            juce::Justification::centred);
        graphics.drawText(
            "R",
            labelBounds.removeFromRight(14),
            juce::Justification::centred);
    }
}

class StudioPanSlider final : public juce::Slider
{
public:
    StudioPanSlider()
    {
        setSliderStyle(juce::Slider::LinearHorizontal);
    }

    void setAccentColour(juce::Colour colour)
    {
        accent = colour;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        auto bounds = getLocalBounds().toFloat();
        if (getTextBoxPosition()
            == juce::Slider::TextBoxBelow)
            bounds.removeFromBottom(28.0f);
        drawStudioPanControl(
            graphics,
            bounds,
            static_cast<float>(getValue()),
            accent,
            isEnabled());
        if (hasKeyboardFocus(true))
        {
            graphics.setColour(
                juce::Colour(StudioColours::orange));
            graphics.drawRoundedRectangle(
                bounds.reduced(1.0f),
                3.0f,
                1.5f);
        }
    }

private:
    juce::Colour accent {
        StudioColours::orange
    };
};
}
