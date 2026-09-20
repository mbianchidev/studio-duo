#include "StudioIconButton.h"

#include <algorithm>

namespace studio
{
namespace
{
void addChevron(juce::Path& path,
                float firstX,
                float firstY,
                float pointX,
                float pointY,
                float lastX,
                float lastY)
{
    path.startNewSubPath(firstX, firstY);
    path.lineTo(pointX, pointY);
    path.lineTo(lastX, lastY);
}

bool usesFill(StudioIcon icon)
{
    return icon == StudioIcon::play
        || icon == StudioIcon::pause
        || icon == StudioIcon::stop
        || icon == StudioIcon::record;
}
}

juce::Path createStudioIconPath(StudioIcon icon)
{
    juce::Path path;
    switch (icon)
    {
        case StudioIcon::newFile:
            path.startNewSubPath(6.0f, 2.0f);
            path.lineTo(15.0f, 2.0f);
            path.lineTo(20.0f, 7.0f);
            path.lineTo(20.0f, 22.0f);
            path.lineTo(6.0f, 22.0f);
            path.closeSubPath();
            path.startNewSubPath(15.0f, 2.0f);
            path.lineTo(15.0f, 7.0f);
            path.lineTo(20.0f, 7.0f);
            path.startNewSubPath(9.0f, 14.0f);
            path.lineTo(17.0f, 14.0f);
            path.startNewSubPath(13.0f, 10.0f);
            path.lineTo(13.0f, 18.0f);
            break;

        case StudioIcon::openFolder:
        case StudioIcon::folder:
            path.startNewSubPath(2.0f, 7.0f);
            path.lineTo(2.0f, 20.0f);
            path.lineTo(20.0f, 20.0f);
            path.lineTo(22.0f, 9.0f);
            path.lineTo(10.0f, 9.0f);
            path.lineTo(8.0f, 5.0f);
            path.lineTo(2.0f, 5.0f);
            path.closeSubPath();
            break;

        case StudioIcon::save:
            path.addRectangle(3.0f, 3.0f, 18.0f, 18.0f);
            path.addRectangle(7.0f, 3.0f, 9.0f, 6.0f);
            path.addRectangle(7.0f, 14.0f, 10.0f, 7.0f);
            break;

        case StudioIcon::exportFile:
            path.startNewSubPath(12.0f, 16.0f);
            path.lineTo(12.0f, 3.0f);
            addChevron(path, 7.0f, 8.0f, 12.0f, 3.0f, 17.0f, 8.0f);
            path.startNewSubPath(4.0f, 13.0f);
            path.lineTo(4.0f, 21.0f);
            path.lineTo(20.0f, 21.0f);
            path.lineTo(20.0f, 13.0f);
            break;

        case StudioIcon::settings:
            for (int index = 0; index < 24; ++index)
            {
                const auto angle = juce::MathConstants<float>::twoPi
                    * static_cast<float>(index) / 24.0f;
                const auto toothPhase = index % 3;
                const auto radius = toothPhase == 1 ? 10.0f : 8.0f;
                const auto point = juce::Point<float>(12.0f, 12.0f)
                    .getPointOnCircumference(radius, angle);
                if (index == 0)
                    path.startNewSubPath(point);
                else
                    path.lineTo(point);
            }
            path.closeSubPath();
            path.addEllipse(8.5f, 8.5f, 7.0f, 7.0f);
            break;

        case StudioIcon::undo:
            path.startNewSubPath(21.0f, 18.0f);
            path.cubicTo(18.0f, 8.0f, 10.0f, 7.0f, 4.0f, 12.0f);
            addChevron(path, 4.0f, 6.0f, 4.0f, 12.0f, 10.0f, 12.0f);
            break;

        case StudioIcon::redo:
            path.startNewSubPath(3.0f, 18.0f);
            path.cubicTo(6.0f, 8.0f, 14.0f, 7.0f, 20.0f, 12.0f);
            addChevron(path, 14.0f, 12.0f, 20.0f, 12.0f, 20.0f, 6.0f);
            break;

        case StudioIcon::play:
            path.addTriangle(6.0f, 3.0f, 21.0f, 12.0f, 6.0f, 21.0f);
            break;

        case StudioIcon::pause:
            path.addRectangle(5.0f, 3.0f, 5.0f, 18.0f);
            path.addRectangle(14.0f, 3.0f, 5.0f, 18.0f);
            break;

        case StudioIcon::stop:
            path.addRectangle(4.0f, 4.0f, 16.0f, 16.0f);
            break;

        case StudioIcon::record:
            path.addEllipse(3.0f, 3.0f, 18.0f, 18.0f);
            break;

        case StudioIcon::loop:
            path.startNewSubPath(5.0f, 8.0f);
            path.cubicTo(7.0f, 4.0f, 16.0f, 4.0f, 19.0f, 8.0f);
            addChevron(path, 15.0f, 8.0f, 19.0f, 8.0f, 19.0f, 4.0f);
            path.startNewSubPath(19.0f, 16.0f);
            path.cubicTo(17.0f, 20.0f, 8.0f, 20.0f, 5.0f, 16.0f);
            addChevron(path, 9.0f, 16.0f, 5.0f, 16.0f, 5.0f, 20.0f);
            break;

        case StudioIcon::loopRange:
            path.startNewSubPath(6.0f, 8.0f);
            path.cubicTo(8.0f, 4.0f, 15.0f, 4.0f, 18.0f, 8.0f);
            addChevron(path, 14.0f, 8.0f, 18.0f, 8.0f, 18.0f, 4.0f);
            path.startNewSubPath(18.0f, 16.0f);
            path.cubicTo(16.0f, 20.0f, 9.0f, 20.0f, 6.0f, 16.0f);
            addChevron(path, 10.0f, 16.0f, 6.0f, 16.0f, 6.0f, 20.0f);
            path.startNewSubPath(3.0f, 5.0f);
            path.lineTo(3.0f, 19.0f);
            path.startNewSubPath(21.0f, 5.0f);
            path.lineTo(21.0f, 19.0f);
            break;

        case StudioIcon::metronome:
            path.startNewSubPath(8.0f, 3.0f);
            path.lineTo(16.0f, 3.0f);
            path.lineTo(20.0f, 21.0f);
            path.lineTo(4.0f, 21.0f);
            path.closeSubPath();
            path.startNewSubPath(12.0f, 7.0f);
            path.lineTo(16.0f, 17.0f);
            path.addEllipse(14.5f, 15.5f, 3.0f, 3.0f);
            break;

        case StudioIcon::mute:
            path.startNewSubPath(3.0f, 9.0f);
            path.lineTo(7.0f, 9.0f);
            path.lineTo(12.0f, 5.0f);
            path.lineTo(12.0f, 19.0f);
            path.lineTo(7.0f, 15.0f);
            path.lineTo(3.0f, 15.0f);
            path.closeSubPath();
            path.startNewSubPath(15.0f, 8.0f);
            path.lineTo(21.0f, 16.0f);
            path.startNewSubPath(21.0f, 8.0f);
            path.lineTo(15.0f, 16.0f);
            break;

        case StudioIcon::solo:
            path.startNewSubPath(4.0f, 13.0f);
            path.cubicTo(4.0f, 4.0f, 20.0f, 4.0f, 20.0f, 13.0f);
            path.startNewSubPath(4.0f, 12.0f);
            path.lineTo(4.0f, 19.0f);
            path.lineTo(8.0f, 19.0f);
            path.lineTo(8.0f, 13.0f);
            path.closeSubPath();
            path.startNewSubPath(20.0f, 12.0f);
            path.lineTo(20.0f, 19.0f);
            path.lineTo(16.0f, 19.0f);
            path.lineTo(16.0f, 13.0f);
            path.closeSubPath();
            break;

        case StudioIcon::volume:
            path.startNewSubPath(3.0f, 9.0f);
            path.lineTo(7.0f, 9.0f);
            path.lineTo(12.0f, 5.0f);
            path.lineTo(12.0f, 19.0f);
            path.lineTo(7.0f, 15.0f);
            path.lineTo(3.0f, 15.0f);
            path.closeSubPath();
            path.startNewSubPath(15.0f, 9.0f);
            path.cubicTo(18.0f, 10.0f, 18.0f, 14.0f, 15.0f, 15.0f);
            path.startNewSubPath(17.0f, 6.0f);
            path.cubicTo(23.0f, 9.0f, 23.0f, 15.0f, 17.0f, 18.0f);
            break;

        case StudioIcon::midi:
            path.startNewSubPath(9.0f, 5.0f);
            path.lineTo(20.0f, 3.0f);
            path.lineTo(20.0f, 15.0f);
            path.startNewSubPath(9.0f, 5.0f);
            path.lineTo(9.0f, 17.0f);
            path.startNewSubPath(9.0f, 9.0f);
            path.lineTo(20.0f, 7.0f);
            path.addEllipse(3.0f, 15.0f, 6.0f, 5.0f);
            path.addEllipse(14.0f, 13.0f, 6.0f, 5.0f);
            break;

        case StudioIcon::inspect:
            path.addRoundedRectangle(
                3.0f,
                4.0f,
                18.0f,
                16.0f,
                2.0f);
            path.startNewSubPath(15.0f, 4.0f);
            path.lineTo(15.0f, 20.0f);
            path.startNewSubPath(17.5f, 8.0f);
            path.lineTo(19.0f, 8.0f);
            path.startNewSubPath(17.5f, 12.0f);
            path.lineTo(19.0f, 12.0f);
            break;

        case StudioIcon::mixer:
            path.startNewSubPath(5.0f, 3.0f);
            path.lineTo(5.0f, 21.0f);
            path.startNewSubPath(12.0f, 3.0f);
            path.lineTo(12.0f, 21.0f);
            path.startNewSubPath(19.0f, 3.0f);
            path.lineTo(19.0f, 21.0f);
            path.addRoundedRectangle(2.5f, 7.0f, 5.0f, 5.0f, 1.5f);
            path.addRoundedRectangle(9.5f, 13.0f, 5.0f, 5.0f, 1.5f);
            path.addRoundedRectangle(16.5f, 5.0f, 5.0f, 5.0f, 1.5f);
            break;

        case StudioIcon::tracks:
            for (int row = 0; row < 3; ++row)
            {
                const auto y = 5.0f
                    + static_cast<float>(row) * 7.0f;
                path.addRoundedRectangle(
                    3.0f,
                    y,
                    4.0f,
                    4.0f,
                    1.0f);
                path.startNewSubPath(10.0f, y + 2.0f);
                path.lineTo(21.0f, y + 2.0f);
            }
            break;

        case StudioIcon::snap:
            path.startNewSubPath(5.0f, 4.0f);
            path.lineTo(5.0f, 14.0f);
            path.cubicTo(5.0f, 22.0f, 19.0f, 22.0f, 19.0f, 14.0f);
            path.lineTo(19.0f, 4.0f);
            path.lineTo(14.0f, 4.0f);
            path.lineTo(14.0f, 13.0f);
            path.cubicTo(14.0f, 16.0f, 10.0f, 16.0f, 10.0f, 13.0f);
            path.lineTo(10.0f, 4.0f);
            path.closeSubPath();
            break;

        case StudioIcon::info:
            path.addEllipse(3.0f, 3.0f, 18.0f, 18.0f);
            path.addEllipse(10.5f, 6.0f, 3.0f, 3.0f);
            path.startNewSubPath(12.0f, 11.0f);
            path.lineTo(12.0f, 18.0f);
            break;

        case StudioIcon::add:
            path.startNewSubPath(4.0f, 12.0f);
            path.lineTo(20.0f, 12.0f);
            path.startNewSubPath(12.0f, 4.0f);
            path.lineTo(12.0f, 20.0f);
            break;

        case StudioIcon::bus:
            path.startNewSubPath(4.0f, 5.0f);
            path.lineTo(4.0f, 19.0f);
            path.startNewSubPath(4.0f, 8.0f);
            path.lineTo(14.0f, 8.0f);
            path.lineTo(14.0f, 5.0f);
            path.startNewSubPath(4.0f, 16.0f);
            path.lineTo(14.0f, 16.0f);
            path.lineTo(14.0f, 19.0f);
            path.addEllipse(16.0f, 3.0f, 5.0f, 5.0f);
            path.addEllipse(16.0f, 16.0f, 5.0f, 5.0f);
            break;

        case StudioIcon::importFile:
            path.startNewSubPath(12.0f, 3.0f);
            path.lineTo(12.0f, 16.0f);
            addChevron(path, 7.0f, 11.0f, 12.0f, 16.0f, 17.0f, 11.0f);
            path.startNewSubPath(4.0f, 15.0f);
            path.lineTo(4.0f, 21.0f);
            path.lineTo(20.0f, 21.0f);
            path.lineTo(20.0f, 15.0f);
            break;

        case StudioIcon::duplicate:
            path.addRectangle(7.0f, 7.0f, 14.0f, 14.0f);
            path.startNewSubPath(17.0f, 7.0f);
            path.lineTo(17.0f, 3.0f);
            path.lineTo(3.0f, 3.0f);
            path.lineTo(3.0f, 17.0f);
            path.lineTo(7.0f, 17.0f);
            break;

        case StudioIcon::deleteItem:
            path.startNewSubPath(5.0f, 7.0f);
            path.lineTo(19.0f, 7.0f);
            path.startNewSubPath(9.0f, 4.0f);
            path.lineTo(15.0f, 4.0f);
            path.startNewSubPath(7.0f, 7.0f);
            path.lineTo(8.0f, 21.0f);
            path.lineTo(16.0f, 21.0f);
            path.lineTo(17.0f, 7.0f);
            path.startNewSubPath(10.0f, 10.0f);
            path.lineTo(10.0f, 18.0f);
            path.startNewSubPath(14.0f, 10.0f);
            path.lineTo(14.0f, 18.0f);
            break;

        case StudioIcon::marker:
            path.startNewSubPath(6.0f, 22.0f);
            path.lineTo(6.0f, 3.0f);
            path.lineTo(19.0f, 6.0f);
            path.lineTo(6.0f, 11.0f);
            break;

        case StudioIcon::automation:
            path.startNewSubPath(3.0f, 18.0f);
            path.cubicTo(7.0f, 18.0f, 8.0f, 6.0f, 12.0f, 6.0f);
            path.cubicTo(16.0f, 6.0f, 17.0f, 14.0f, 21.0f, 14.0f);
            path.addEllipse(1.5f, 16.5f, 3.0f, 3.0f);
            path.addEllipse(10.5f, 4.5f, 3.0f, 3.0f);
            path.addEllipse(19.5f, 12.5f, 3.0f, 3.0f);
            break;

        case StudioIcon::chevronLeft:
            addChevron(path, 16.0f, 4.0f, 8.0f, 12.0f, 16.0f, 20.0f);
            break;

        case StudioIcon::chevronRight:
            addChevron(path, 8.0f, 4.0f, 16.0f, 12.0f, 8.0f, 20.0f);
            break;

        case StudioIcon::chevronUp:
            addChevron(path, 4.0f, 16.0f, 12.0f, 8.0f, 20.0f, 16.0f);
            break;

        case StudioIcon::chevronDown:
            addChevron(path, 4.0f, 8.0f, 12.0f, 16.0f, 20.0f, 8.0f);
            break;

        case StudioIcon::split:
            path.addEllipse(3.0f, 4.0f, 6.0f, 6.0f);
            path.addEllipse(3.0f, 14.0f, 6.0f, 6.0f);
            path.startNewSubPath(8.0f, 8.0f);
            path.lineTo(21.0f, 18.0f);
            path.startNewSubPath(8.0f, 16.0f);
            path.lineTo(21.0f, 6.0f);
            break;

        case StudioIcon::trimStart:
            path.startNewSubPath(5.0f, 3.0f);
            path.lineTo(5.0f, 21.0f);
            path.startNewSubPath(19.0f, 6.0f);
            path.lineTo(10.0f, 12.0f);
            path.lineTo(19.0f, 18.0f);
            break;

        case StudioIcon::trimEnd:
            path.startNewSubPath(19.0f, 3.0f);
            path.lineTo(19.0f, 21.0f);
            path.startNewSubPath(5.0f, 6.0f);
            path.lineTo(14.0f, 12.0f);
            path.lineTo(5.0f, 18.0f);
            break;

        case StudioIcon::zoomOut:
        case StudioIcon::zoomIn:
            path.addEllipse(3.0f, 3.0f, 14.0f, 14.0f);
            path.startNewSubPath(15.0f, 15.0f);
            path.lineTo(22.0f, 22.0f);
            path.startNewSubPath(6.0f, 10.0f);
            path.lineTo(14.0f, 10.0f);
            if (icon == StudioIcon::zoomIn)
            {
                path.startNewSubPath(10.0f, 6.0f);
                path.lineTo(10.0f, 14.0f);
            }
            break;

        case StudioIcon::edit:
            path.startNewSubPath(4.0f, 20.0f);
            path.lineTo(8.0f, 19.0f);
            path.lineTo(20.0f, 7.0f);
            path.lineTo(17.0f, 4.0f);
            path.lineTo(5.0f, 16.0f);
            path.closeSubPath();
            path.startNewSubPath(14.0f, 7.0f);
            path.lineTo(17.0f, 10.0f);
            break;

        case StudioIcon::close:
            path.startNewSubPath(5.0f, 5.0f);
            path.lineTo(19.0f, 19.0f);
            path.startNewSubPath(19.0f, 5.0f);
            path.lineTo(5.0f, 19.0f);
            break;

        case StudioIcon::expand:
            path.startNewSubPath(10.0f, 4.0f);
            path.lineTo(4.0f, 4.0f);
            path.lineTo(4.0f, 10.0f);
            path.startNewSubPath(4.0f, 4.0f);
            path.lineTo(10.0f, 10.0f);
            path.startNewSubPath(14.0f, 20.0f);
            path.lineTo(20.0f, 20.0f);
            path.lineTo(20.0f, 14.0f);
            path.startNewSubPath(20.0f, 20.0f);
            path.lineTo(14.0f, 14.0f);
            break;

        case StudioIcon::scan:
            path.startNewSubPath(5.0f, 8.0f);
            path.cubicTo(9.0f, 2.0f, 18.0f, 4.0f, 20.0f, 10.0f);
            addChevron(path, 16.0f, 8.0f, 20.0f, 10.0f, 21.0f, 6.0f);
            path.startNewSubPath(19.0f, 16.0f);
            path.cubicTo(15.0f, 22.0f, 6.0f, 20.0f, 4.0f, 14.0f);
            addChevron(path, 8.0f, 16.0f, 4.0f, 14.0f, 3.0f, 18.0f);
            break;

        case StudioIcon::route:
            path.startNewSubPath(4.0f, 4.0f);
            path.lineTo(4.0f, 20.0f);
            path.startNewSubPath(4.0f, 8.0f);
            path.cubicTo(11.0f, 8.0f, 10.0f, 5.0f, 16.0f, 5.0f);
            path.startNewSubPath(4.0f, 16.0f);
            path.cubicTo(11.0f, 16.0f, 10.0f, 19.0f, 16.0f, 19.0f);
            path.addEllipse(17.0f, 3.0f, 4.0f, 4.0f);
            path.addEllipse(17.0f, 17.0f, 4.0f, 4.0f);
            break;
    }
    return path;
}

void drawStudioIcon(juce::Graphics& graphics,
                    StudioIcon icon,
                    juce::Rectangle<float> bounds,
                    juce::Colour colour,
                    float strokeWidth)
{
    auto path = createStudioIconPath(icon);
    path.scaleToFit(bounds.getX(),
                    bounds.getY(),
                    bounds.getWidth(),
                    bounds.getHeight(),
                    true);
    graphics.setColour(colour);
    if (usesFill(icon))
        graphics.fillPath(path);
    else
        graphics.strokePath(
            path,
            juce::PathStrokeType(
                strokeWidth,
                juce::PathStrokeType::curved,
                juce::PathStrokeType::rounded));
}

StudioIconButton::StudioIconButton(StudioIcon initialIcon,
                                   juce::String accessibleLabel,
                                   juce::String tooltip)
    : juce::TextButton({}, tooltip),
      icon(initialIcon),
      visibleLabel(accessibleLabel)
{
    setButtonText({});
    setAccessibleLabel(std::move(accessibleLabel));
    setTooltip(tooltip.isNotEmpty() ? tooltip : getTitle());
    setWantsKeyboardFocus(true);
}

void StudioIconButton::setIcon(StudioIcon nextIcon)
{
    if (icon == nextIcon)
        return;
    icon = nextIcon;
    repaint();
}

StudioIcon StudioIconButton::getIcon() const noexcept
{
    return icon;
}

void StudioIconButton::setAccessibleLabel(juce::String label)
{
    jassert(label.isNotEmpty());
    setName(label);
    setTitle(label);
}

void StudioIconButton::setVisibleLabel(juce::String label)
{
    visibleLabel = std::move(label);
    repaint();
}

void StudioIconButton::setShowLabel(bool shouldShow)
{
    if (showLabel == shouldShow)
        return;
    showLabel = shouldShow;
    repaint();
}

bool StudioIconButton::isShowingLabel() const noexcept
{
    return showLabel;
}

void StudioIconButton::paintButton(juce::Graphics& graphics,
                                   bool highlighted,
                                   bool down)
{
    getLookAndFeel().drawButtonBackground(
        graphics,
        *this,
        findColour(
            getToggleState()
                ? juce::TextButton::buttonOnColourId
                : juce::TextButton::buttonColourId),
        highlighted,
        down);

    auto colour = findColour(
        getToggleState()
            ? juce::TextButton::textColourOnId
            : juce::TextButton::textColourOffId);
    if (!isEnabled())
        colour = colour.withMultipliedAlpha(0.45f);
    else if (down)
        colour = colour.darker(0.08f);
    else if (highlighted)
        colour = colour.brighter(0.08f);

    auto content = getLocalBounds().toFloat().reduced(5.0f);
    juce::Rectangle<float> iconBounds;
    if (showLabel && visibleLabel.isNotEmpty())
    {
        iconBounds = content.removeFromLeft(
            juce::jmin(
                content.getHeight(),
                24.0f)).reduced(3.0f);
        content.removeFromLeft(5.0f);
        graphics.setColour(colour);
        graphics.setFont(
            juce::Font(
                juce::FontOptions(10.5f,
                                  juce::Font::bold)));
        graphics.drawFittedText(
            visibleLabel,
            content.toNearestInt(),
            juce::Justification::centredLeft,
            1,
            0.8f);
    }
    else
    {
        const auto side = static_cast<float>(
            std::min(getWidth(), getHeight()));
        const auto inset = juce::jmax(4.0f, side * 0.23f);
        iconBounds = getLocalBounds().toFloat()
            .withSizeKeepingCentre(side, side)
            .reduced(inset);
    }
    drawStudioIcon(
        graphics,
        icon,
        iconBounds,
        colour,
        juce::jmax(1.35f, iconBounds.getWidth() / 11.0f));
}
}
