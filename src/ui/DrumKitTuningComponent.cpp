#include "DrumKitTuningComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <cmath>

namespace studio
{
DrumKitTuningComponent::DrumKitTuningComponent(
    std::vector<PluginParameterDescriptor> values, int drum)
    : parameters(std::move(values)),
      selectedDrum(juce::jlimit(0, DrumDeviceProcessor::tunableDrumCount - 1, drum))
{
    setTitle("Tune drum kit");
    setFocusContainerType(FocusContainerType::focusContainer);
    drumSelector.setComponentID("kit-tuning-drum");
    drumSelector.setTitle("Drum shell to tune");
    drumSelector.addItemList(DrumDeviceProcessor::tunableDrumNames(), 1);
    drumSelector.setSelectedId(selectedDrum + 1, juce::dontSendNotification);
    drumSelector.onChange = [this] { setSelectedDrum(drumSelector.getSelectedItemIndex()); };
    addAndMakeVisible(drumSelector);

    const std::array<juce::String, 4> names {
        "Batter head", "Resonant head", "Damping", "Snare wires"
    };
    const std::array<juce::String, 4> ids {
        "kit-batter-head", "kit-resonant-head", "kit-damping", "kit-snare-wires"
    };
    const std::array<juce::String, 4> tips {
        "Raise or lower the struck head's pitch relative to the selected kit preset.",
        "Tune the opposite head to change the drum's ring, overtones, and sustain.",
        "Add damping, like a gel, ring, or kick pillow, to shorten the drum's sustain.",
        "Disengage the wires at 0%; 50% keeps the preset response; higher tension dries the buzz."
    };
    for (int index = 0; index < static_cast<int>(sliders.size()); ++index)
    {
        const auto position = static_cast<std::size_t>(index);
        auto& slider = sliders[position];
        labels[position].setText(names[position], juce::dontSendNotification);
        addAndMakeVisible(labels[position]);
        slider.setComponentID(ids[position]);
        slider.setTitle(names[position]);
        slider.setTooltip(tips[position]);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 24);
        slider.onValueChange = [this, index]
        {
            if (!refreshing)
                report(changeValue(index, sliders[static_cast<std::size_t>(index)].getValue()));
        };
        slider.onDragStart = [this, index] { startGesture(index); };
        slider.onDragEnd = [this] { finishGesture(false); };
        addAndMakeVisible(slider);
    }
    hint.setText("Tune by ear. Head values are offsets from the chosen preset.",
                 juce::dontSendNotification);
    hint.setFont(juce::Font(juce::FontOptions(12.0f)));
    hint.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    addAndMakeVisible(hint);
    status.setFont(juce::Font(juce::FontOptions(12.0f)));
    addAndMakeVisible(status);
    tapButton.setComponentID("kit-tap");
    tapButton.setTooltip("Tap this drum through the existing track, instrument, and routing.");
    tapButton.onClick = [this]
    {
        report(onAudition
            ? onAudition(DrumDeviceProcessor::tunableDrumNote(selectedDrum))
            : juce::Result::fail("Drum audition is unavailable."));
    };
    addAndMakeVisible(tapButton);
    resetButton.setComponentID("kit-reset");
    resetButton.setTooltip("Restore only this drum's head, damping, and wire offsets. Keep other drums and the preset.");
    resetButton.onClick = [this] { resetSelectedDrum(); };
    addAndMakeVisible(resetButton);
    refreshControls();
    setSize(480, 372);
    startTimerHz(10);
}

DrumKitTuningComponent::~DrumKitTuningComponent()
{
    stopTimer();
    finishGesture(false);
}

void DrumKitTuningComponent::setParameters(std::vector<PluginParameterDescriptor> values)
{
    if (activeGesture.has_value()
        && std::none_of(values.cbegin(), values.cend(), [this](const auto& value)
            { return value.id == activeGesture->id && value.index >= 0; }))
        finishGesture(true);
    parameters = std::move(values);
    refreshControls();
}

void DrumKitTuningComponent::setSelectedDrum(int drum)
{
    if (!juce::isPositiveAndBelow(drum, DrumDeviceProcessor::tunableDrumCount))
    {
        report(juce::Result::fail("Choose a kick, snare, or tom to tune."));
        return;
    }
    finishGesture(false);
    selectedDrum = drum;
    drumSelector.setSelectedId(drum + 1, juce::dontSendNotification);
    refreshControls();
    resized();
}

const DrumDeviceProcessor::KitTuningParameter*
DrumKitTuningComponent::definition(int control) const
{
    if (control < 0 || control >= static_cast<int>(sliders.size()))
        return nullptr;
    auto index = 0;
    for (const auto& setting : DrumDeviceProcessor::kitTuningParameters())
        if (setting.drum == selectedDrum && index++ == control)
            return &setting;
    return nullptr;
}

const PluginParameterDescriptor* DrumKitTuningComponent::parameter(int control) const
{
    const auto* setting = definition(control);
    if (setting == nullptr)
        return nullptr;
    const auto found = std::find_if(parameters.cbegin(), parameters.cend(),
        [setting](const auto& value)
        {
            return value.id == setting->id && value.index >= 0 && value.automatable
                && std::isfinite(value.value) && value.value >= 0.0f && value.value <= 1.0f;
        });
    return found != parameters.cend() ? &*found : nullptr;
}

void DrumKitTuningComponent::refreshControls()
{
    const juce::ScopedValueSetter<bool> guard(refreshing, true);
    auto available = true;
    for (int index = 0; index < static_cast<int>(sliders.size()); ++index)
    {
        const auto* setting = definition(index);
        auto& slider = sliders[static_cast<std::size_t>(index)];
        auto& label = labels[static_cast<std::size_t>(index)];
        slider.setVisible(setting != nullptr);
        label.setVisible(setting != nullptr);
        const auto* value = parameter(index);
        slider.setEnabled(value != nullptr);
        label.setEnabled(value != nullptr);
        if (setting == nullptr)
            continue;
        available = available && value != nullptr;
        slider.setRange(setting->minimum, setting->maximum, 0.1);
        slider.setTextValueSuffix(" " + juce::String(setting->unit));
        slider.setDoubleClickReturnValue(true, setting->defaultValue);
        slider.setTitle(setting->name);
        const auto editing = std::any_of(slider.getChildren().begin(), slider.getChildren().end(),
            [](const auto* child)
            {
                const auto* textBox = dynamic_cast<const juce::Label*>(child);
                return textBox != nullptr && textBox->isBeingEdited();
            });
        if (value != nullptr && !editing
            && (!activeGesture.has_value() || activeGesture->id != value->id))
            slider.setValue(setting->minimum + (setting->maximum - setting->minimum) * value->value,
                            juce::dontSendNotification);
    }
    tapButton.setEnabled(available);
    resetButton.setEnabled(available);
    if (!available)
        status.setText("The drum instrument is unavailable.", juce::dontSendNotification);
}

void DrumKitTuningComponent::startGesture(int control)
{
    finishGesture(false);
    if (const auto* value = parameter(control))
    {
        activeGesture = *value;
        gestureChanged = false;
        if (onGestureStarted)
            onGestureStarted(*activeGesture, activeGesture->value);
    }
}

void DrumKitTuningComponent::finishGesture(bool cancelled)
{
    if (!activeGesture.has_value())
        return;
    const auto value = *activeGesture;
    activeGesture.reset();
    if (cancelled || !gestureChanged)
    {
        if (onGestureCancelled)
            onGestureCancelled(value);
    }
    else if (onGestureEnded)
    {
        onGestureEnded(value, value.value);
    }
    gestureChanged = false;
}

juce::Result DrumKitTuningComponent::changeValue(int control, double value)
{
    const auto* setting = definition(control);
    const auto* current = parameter(control);
    if (setting == nullptr || current == nullptr)
        return juce::Result::fail("This drum's tuning control is unavailable.");
    if (!std::isfinite(value) || value < setting->minimum || value > setting->maximum)
        return juce::Result::fail("Choose a tuning value within the displayed range.");
    const auto previous = *current;
    const auto normalized = static_cast<float>(
        (value - setting->minimum) / (setting->maximum - setting->minimum));
    if (std::abs(normalized - previous.value) < 0.000001f)
        return juce::Result::ok();
    const auto dragging = activeGesture.has_value() && activeGesture->id == previous.id;
    if (!dragging && onGestureStarted)
        onGestureStarted(previous, previous.value);
    const auto result = onValueChanged
        ? onValueChanged(previous, normalized)
        : juce::Result::fail("The drum instrument's tuning control is unavailable.");
    if (result.failed())
    {
        if (dragging)
            finishGesture(true);
        else if (onGestureCancelled)
            onGestureCancelled(previous);
        sliders[static_cast<std::size_t>(control)].setValue(
            setting->minimum + (setting->maximum - setting->minimum) * previous.value,
            juce::dontSendNotification);
        return result;
    }
    for (auto& target : parameters)
        if (target.id == previous.id)
            target.value = normalized;
    if (dragging)
    {
        activeGesture->value = normalized;
        gestureChanged = true;
    }
    else if (onGestureEnded)
    {
        onGestureEnded(previous, normalized);
    }
    return juce::Result::ok();
}

void DrumKitTuningComponent::resetSelectedDrum()
{
    finishGesture(false);
    for (int index = 0; index < static_cast<int>(sliders.size()); ++index)
    {
        const auto* setting = definition(index);
        if (setting == nullptr)
            continue;
        const auto result = changeValue(index, setting->defaultValue);
        if (result.failed())
        {
            refreshControls();
            report(result);
            return;
        }
    }
    refreshControls();
    report(juce::Result::ok());
}

void DrumKitTuningComponent::report(const juce::Result& result)
{
    status.setText(result.failed() ? result.getErrorMessage() : "Tap the drum to hear your tuning.",
                   juce::dontSendNotification);
    if (result.failed() && onStatus)
        onStatus(result.getErrorMessage(), true);
}

void DrumKitTuningComponent::timerCallback()
{
    if (onRefreshParameters)
        setParameters(onRefreshParameters());
    repaint();
}

void DrumKitTuningComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    graphics.drawText("Tune drum kit", 14, 10, getWidth() - 28, 24,
                      juce::Justification::centredLeft);
    graphics.setColour(juce::Colour(StudioColours::orange));
    for (const auto* control : std::array<const juce::Component*, 7> {
             &drumSelector, &sliders[0], &sliders[1], &sliders[2], &sliders[3],
             &tapButton, &resetButton })
        if (control->isVisible() && control->hasKeyboardFocus(true))
            graphics.drawRoundedRectangle(control->getBounds().toFloat().expanded(1.0f), 3.0f, 2.0f);
}

void DrumKitTuningComponent::resized()
{
    auto bounds = getLocalBounds().reduced(14);
    bounds.removeFromTop(24);
    hint.setBounds(bounds.removeFromTop(28));
    drumSelector.setBounds(bounds.removeFromTop(30));
    bounds.removeFromTop(8);
    for (std::size_t index = 0; index < sliders.size(); ++index)
    {
        if (!sliders[index].isVisible())
            continue;
        auto row = bounds.removeFromTop(40);
        labels[index].setBounds(row.removeFromLeft(110));
        sliders[index].setBounds(row.reduced(2, 5));
    }
    bounds.removeFromTop(8);
    auto buttons = bounds.removeFromTop(30);
    tapButton.setBounds(buttons.removeFromLeft(112).reduced(2));
    resetButton.setBounds(buttons.removeFromLeft(176).reduced(2));
    status.setBounds(bounds.reduced(0, 3));
}
}
