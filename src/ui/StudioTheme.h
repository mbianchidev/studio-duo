#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <optional>

namespace studio
{
struct StudioThemePalette
{
    std::uint32_t window = 0xff1d2024;
    std::uint32_t panel = 0xff25292e;
    std::uint32_t raised = 0xff30353b;
    std::uint32_t transport = 0xff202b3d;
    std::uint32_t transportRaised = 0xff2b3b57;
    std::uint32_t border = 0xff7b838c;
    std::uint32_t text = 0xfff0ede8;
    std::uint32_t secondaryText = 0xffabb2b9;
    std::uint32_t orange = 0xffeb7658;
    std::uint32_t amber = 0xffdda85a;
    std::uint32_t green = 0xff7fc9ac;
    std::uint32_t violet = 0xffbf8ccc;

    [[nodiscard]] bool operator==(
        const StudioThemePalette&) const noexcept = default;

    [[nodiscard]] bool isAccessible(
        juce::String& error) const
    {
        const auto luminance = [](std::uint32_t argb)
        {
            const auto channel = [](std::uint32_t value)
            {
                const auto normalized =
                    static_cast<double>(value) / 255.0;
                return normalized <= 0.04045
                    ? normalized / 12.92
                    : std::pow(
                          (normalized + 0.055) / 1.055,
                          2.4);
            };
            return 0.2126 * channel((argb >> 16) & 0xff)
                + 0.7152 * channel((argb >> 8) & 0xff)
                + 0.0722 * channel(argb & 0xff);
        };
        const auto contrast =
            [&luminance](std::uint32_t first,
                         std::uint32_t second)
        {
            const auto left = luminance(first);
            const auto right = luminance(second);
            return (juce::jmax(left, right) + 0.05)
                / (juce::jmin(left, right) + 0.05);
        };
        for (const auto surface :
             { window, panel, raised, transport })
        {
            if (contrast(text, surface) < 4.5)
            {
                error =
                    "Primary text must keep at least 4.5:1 contrast "
                    "against every core surface.";
                return false;
            }
        }
        if (contrast(secondaryText, panel) < 4.5)
        {
            error =
                "Secondary text must keep at least 4.5:1 contrast "
                "against panels.";
            return false;
        }
        if (contrast(border, panel) < 3.0
            || contrast(border, raised) < 3.0)
        {
            error =
                "Control borders must keep at least 3:1 contrast "
                "against adjacent surfaces.";
            return false;
        }
        if (contrast(orange, window) < 4.5
            || contrast(orange, panel) < 3.0
            || contrast(orange, raised) < 3.0)
        {
            error =
                "The accent must keep readable button text and a "
                "3:1 focus indicator against every core surface.";
            return false;
        }
        error.clear();
        return true;
    }
};

struct StudioThemePreset
{
    const char* id;
    const char* name;
    StudioThemePalette palette;
};

inline const std::array<StudioThemePreset, 4>&
studioThemePresets()
{
    static const std::array presets {
        StudioThemePreset {
            "studio-gray",
            "Studio Gray",
            {}
        },
        StudioThemePreset {
            "slate-blue",
            "Slate Blue",
            {
                0xff171c24,
                0xff202733,
                0xff2a3442,
                0xff1d2b40,
                0xff2b4161,
                0xff738197,
                0xffeef2f7,
                0xffaab7c8,
                0xff78aee0,
                0xffdfaa61,
                0xff80c6a6,
                0xffb695d6
            }
        },
        StudioThemePreset {
            "forest",
            "Forest",
            {
                0xff18201f,
                0xff222c2a,
                0xff2d3835,
                0xff1d302d,
                0xff2b4942,
                0xff74847e,
                0xfff0eee8,
                0xffadb9b4,
                0xff82cda4,
                0xffdfad62,
                0xff82cda4,
                0xffba96ca
            }
        },
        StudioThemePreset {
            "aubergine",
            "Aubergine",
            {
                0xff211b24,
                0xff2b2330,
                0xff372d3d,
                0xff30233a,
                0xff493457,
                0xff87738d,
                0xfff2ecef,
                0xffbcaeb9,
                0xffd78ecf,
                0xffe0aa63,
                0xff83c8ad,
                0xffd78ecf
            }
        }
    };
    return presets;
}

inline std::optional<StudioThemePalette>
studioThemePaletteForPreset(const juce::String& id)
{
    for (const auto& preset : studioThemePresets())
        if (id == preset.id)
            return preset.palette;
    return std::nullopt;
}

struct StudioColours
{
    inline static std::uint32_t window = 0xff1d2024;
    inline static std::uint32_t panel = 0xff25292e;
    inline static std::uint32_t raised = 0xff30353b;
    inline static std::uint32_t transport = 0xff202b3d;
    inline static std::uint32_t transportRaised = 0xff2b3b57;
    inline static std::uint32_t border = 0xff7b838c;
    inline static std::uint32_t text = 0xfff0ede8;
    inline static std::uint32_t secondaryText = 0xffabb2b9;
    inline static std::uint32_t orange = 0xffeb7658;
    inline static std::uint32_t amber = 0xffdda85a;
    inline static std::uint32_t green = 0xff7fc9ac;
    inline static std::uint32_t violet = 0xffbf8ccc;

    static void apply(const StudioThemePalette& palette)
    {
        window = palette.window;
        panel = palette.panel;
        raised = palette.raised;
        transport = palette.transport;
        transportRaised = palette.transportRaised;
        border = palette.border;
        text = palette.text;
        secondaryText = palette.secondaryText;
        orange = palette.orange;
        amber = palette.amber;
        green = palette.green;
        violet = palette.violet;
    }
};

class StudioTheme final : public juce::LookAndFeel_V4
{
public:
    explicit StudioTheme(
        StudioThemePalette initialPalette = {})
    {
        applyPalette(initialPalette);
    }

    [[nodiscard]] const StudioThemePalette& palette() const noexcept
    {
        return currentPalette;
    }

    void applyPalette(const StudioThemePalette& paletteToApply)
    {
        currentPalette = paletteToApply;
        StudioColours::apply(currentPalette);
        setColour(
            juce::ResizableWindow::backgroundColourId,
            juce::Colour(StudioColours::window));
        setColour(
            juce::TextButton::buttonColourId,
            juce::Colour(StudioColours::raised));
        setColour(
            juce::TextButton::buttonOnColourId,
            juce::Colour(StudioColours::orange));
        setColour(
            juce::TextButton::textColourOffId,
            juce::Colour(StudioColours::text));
        setColour(
            juce::TextButton::textColourOnId,
            juce::Colour(StudioColours::window));
        setColour(
            juce::ToggleButton::textColourId,
            juce::Colour(StudioColours::text));
        setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::text));
        setColour(
            juce::Slider::backgroundColourId,
            juce::Colour(StudioColours::window));
        setColour(
            juce::Slider::trackColourId,
            juce::Colour(StudioColours::orange));
        setColour(
            juce::Slider::thumbColourId,
            juce::Colour(StudioColours::text));
        setColour(
            juce::ScrollBar::thumbColourId,
            juce::Colour(StudioColours::border));
        setColour(
            juce::ScrollBar::trackColourId,
            juce::Colour(StudioColours::window));
        setColour(
            juce::TooltipWindow::backgroundColourId,
            juce::Colour(StudioColours::raised));
        setColour(
            juce::TooltipWindow::textColourId,
            juce::Colour(StudioColours::text));
        setColour(
            juce::TooltipWindow::outlineColourId,
            juce::Colour(StudioColours::border));
    }

    static void recolourComponentTree(
        juce::Component& component,
        const StudioThemePalette& previous,
        const StudioThemePalette& replacement)
    {
        static constexpr std::array<int, 26> colourIds {
            juce::TextButton::buttonColourId,
            juce::TextButton::buttonOnColourId,
            juce::TextButton::textColourOffId,
            juce::TextButton::textColourOnId,
            juce::ToggleButton::textColourId,
            juce::Label::backgroundColourId,
            juce::Label::textColourId,
            juce::Label::outlineColourId,
            juce::Slider::backgroundColourId,
            juce::Slider::trackColourId,
            juce::Slider::thumbColourId,
            juce::ScrollBar::thumbColourId,
            juce::ScrollBar::trackColourId,
            juce::TextEditor::backgroundColourId,
            juce::TextEditor::textColourId,
            juce::TextEditor::outlineColourId,
            juce::TextEditor::focusedOutlineColourId,
            juce::ComboBox::backgroundColourId,
            juce::ComboBox::textColourId,
            juce::ComboBox::outlineColourId,
            juce::ComboBox::focusedOutlineColourId,
            juce::ComboBox::arrowColourId,
            juce::ListBox::backgroundColourId,
            juce::ListBox::outlineColourId,
            juce::ProgressBar::backgroundColourId,
            juce::ProgressBar::foregroundColourId
        };
        const std::array previousColours {
            previous.window,
            previous.panel,
            previous.raised,
            previous.transport,
            previous.transportRaised,
            previous.border,
            previous.text,
            previous.secondaryText,
            previous.orange,
            previous.amber,
            previous.green,
            previous.violet
        };
        const std::array replacementColours {
            replacement.window,
            replacement.panel,
            replacement.raised,
            replacement.transport,
            replacement.transportRaised,
            replacement.border,
            replacement.text,
            replacement.secondaryText,
            replacement.orange,
            replacement.amber,
            replacement.green,
            replacement.violet
        };
        for (const auto colourId : colourIds)
        {
            if (!component.isColourSpecified(colourId))
                continue;
            const auto current =
                component.findColour(colourId).getARGB();
            const auto match = std::find(
                previousColours.cbegin(),
                previousColours.cend(),
                current);
            if (match == previousColours.cend())
                continue;
            const auto index = static_cast<std::size_t>(
                std::distance(previousColours.cbegin(), match));
            component.setColour(
                colourId,
                juce::Colour(replacementColours[index]));
        }
        for (int index = 0;
             index < component.getNumChildComponents();
             ++index)
        {
            if (auto* child =
                    component.getChildComponent(index))
            {
                recolourComponentTree(
                    *child,
                    previous,
                    replacement);
            }
        }
    }

    void drawButtonBackground(
        juce::Graphics& graphics,
        juce::Button& button,
        const juce::Colour& backgroundColour,
        bool highlighted,
        bool down) override
    {
        auto bounds =
            button.getLocalBounds().toFloat().reduced(0.5f);
        auto colour = backgroundColour;
        if (highlighted)
            colour = colour.brighter(0.08f);
        if (down)
            colour = colour.darker(0.12f);

        graphics.setColour(colour);
        graphics.fillRoundedRectangle(bounds, 4.0f);
        graphics.setColour(
            juce::Colour(StudioColours::border));
        graphics.drawRoundedRectangle(
            bounds,
            4.0f,
            1.0f);
        if (button.hasKeyboardFocus(true))
        {
            graphics.setColour(
                juce::Colour(StudioColours::orange));
            graphics.drawRoundedRectangle(
                bounds.reduced(1.5f),
                3.0f,
                2.0f);
        }
    }

private:
    StudioThemePalette currentPalette;
};
}
