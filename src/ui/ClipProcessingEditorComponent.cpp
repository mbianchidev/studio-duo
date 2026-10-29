#include "ClipProcessingEditorComponent.h"

#include "NumericInput.h"
#include "StudioTheme.h"

namespace studio
{
ClipProcessingEditorComponent::ClipProcessingEditorComponent(const AudioClip& clip, Mode editMode)
    : mode(editMode), duration(clip.durationSeconds)
{
    const auto gain = mode == Mode::gain;
    const auto fadeIn = mode == Mode::fadeIn;
    const auto fade = fadeIn || mode == Mode::fadeOut;
    const auto slip = mode == Mode::slip;
    const auto stretch = mode == Mode::stretch;
    minimumValue = gain ? -60.0 : 0.0;
    maximumValue = gain ? 24.0 : duration;
    if (slip)
    {
        minimumValue = clip.sourceRangeStartSeconds - clip.sourceOffsetSeconds;
        maximumValue = clip.sourceRangeEnd() - clip.sourceOffsetSeconds - clip.sourceSpanSeconds();
    }
    if (stretch)
    {
        minimumValue = clip.sourceSpanSeconds() / 4.0;
        maximumValue = clip.sourceSpanSeconds() / 0.25;
    }
    setName(gain ? "Edit clip gain" : slip ? "Slip clip audio" : stretch ? "Stretch clip duration"
        : fadeIn ? "Edit fade in" : "Edit fade out");
    setTitle(getName() + ": " + clip.name);
    const auto initialise = [this](juce::TextEditor& editor, juce::Label& label,
                                   const char* id, const juce::String& title)
    {
        label.setText(title, juce::dontSendNotification);
        label.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
        label.setFont(juce::FontOptions(13.0f));
        label.setBorderSize({});
        editor.setComponentID(id);
        editor.setTitle(title);
        editor.setName(title);
        editor.setWantsKeyboardFocus(true);
        editor.setSelectAllWhenFocused(true);
        editor.setInputRestrictions(64);
        editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(StudioColours::window));
        editor.setColour(juce::TextEditor::textColourId, juce::Colour(StudioColours::text));
        editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(StudioColours::border));
        editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(StudioColours::orange));
        editor.onReturnKey = [this] { juce::ignoreUnused(applyChanges()); };
        editor.onEscapeKey = [this] { if (onClose) onClose(); };
        addAndMakeVisible(label);
        addAndMakeVisible(editor);
    };
    initialise(value, valueLabel, "clip-edit.value", gain ? "Clip gain (dB)"
        : slip ? "Source slip offset (seconds)" : stretch ? "Clip duration (seconds)" : "Fade duration (seconds)");
    value.setText(gain ? formatRoundTripNumber(clip.gainDecibels)
        : formatRoundTripNumber(slip ? 0.0 : stretch ? duration : fadeIn ? clip.fadeInSeconds : clip.fadeOutSeconds), false);
    value.setDescription(gain ? "Enter a gain from -60 to +24 decibels."
        : "Enter a value from " + formatRoundTripNumber(minimumValue) + " to "
            + formatRoundTripNumber(maximumValue) + " seconds.");
    initialise(curve, curveLabel, "clip-edit.curve", "Fade curve (-1 to +1; 0 is linear)");
    curve.setText(formatRoundTripNumber(fadeIn ? clip.fadeInCurve : clip.fadeOutCurve), false);
    curve.setDescription("Enter a curve between -1 and +1. Zero is linear.");
    curve.setVisible(fade);
    curveLabel.setVisible(fade);
    status.setComponentID("clip-edit.status");
    status.setTitle("Clip edit validation");
    status.setColour(juce::Label::textColourId, juce::Colour(StudioColours::text));
    status.setFont(juce::FontOptions(13.0f));
    status.setBorderSize({});
    addAndMakeVisible(status);
    applyButton.setTitle(gain ? "Apply clip gain" : slip ? "Apply source slip" : stretch ? "Apply stretch duration"
        : fadeIn ? "Apply fade in" : "Apply fade out");
    cancelButton.setTitle("Cancel clip edit");
    applyButton.setWantsKeyboardFocus(true);
    cancelButton.setWantsKeyboardFocus(true);
    applyButton.onClick = [this] { juce::ignoreUnused(applyChanges()); };
    cancelButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(applyButton);
    addAndMakeVisible(cancelButton);
    setSize(360, fade ? 244 : 180);
}

juce::Result ClipProcessingEditorComponent::showError(juce::String message, juce::TextEditor& field)
{
    status.setText("Error: " + message, juce::dontSendNotification);
    if (auto* handler = status.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    if (field.isShowing())
        field.grabKeyboardFocus();
    return juce::Result::fail(std::move(message));
}

juce::Result ClipProcessingEditorComponent::applyChanges()
{
    const auto gain = mode == Mode::gain;
    const auto fade = mode == Mode::fadeIn || mode == Mode::fadeOut;
    const auto number = gain ? parseDecibels(value.getText()) : parseFiniteNumber(value.getText());
    if (!number)
        return showError("Enter a complete finite number.", value);
    if (*number < minimumValue || *number > maximumValue || (mode == Mode::stretch && *number <= 0.0))
        return showError(gain ? "Clip gain must be between -60 and +24 dB."
            : mode == Mode::slip ? "The slip must fit within recoverable source audio."
            : mode == Mode::stretch ? "The duration must retain a playback rate between 0.25x and 4x."
            : "Fade duration must fit within this clip.", value);
    const auto shape = fade ? parseFiniteNumber(curve.getText()) : std::optional<double>(0.0);
    if (!shape || *shape < -1.0 || *shape > 1.0)
        return showError("Fade curve must be a finite number between -1 and +1.", curve);
    if (!onApply)
        return showError("Clip editing is unavailable. Close and reopen this editor.", value);
    const auto action = onApply;
    const auto close = onClose;
    const auto safe = juce::Component::SafePointer<ClipProcessingEditorComponent>(this);
    const auto result = action(*number, static_cast<float>(*shape));
    if (safe == nullptr)
        return result;
    if (result.failed())
        return showError(result.getErrorMessage(), value);
    status.setText({}, juce::dontSendNotification);
    if (close)
        close();
    return juce::Result::ok();
}

void ClipProcessingEditorComponent::focusValue()
{
    value.grabKeyboardFocus();
    value.selectAll();
}

bool ClipProcessingEditorComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isAnyModifierKeyDown())
        return false;
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::returnKey)
    {
        juce::ignoreUnused(applyChanges());
        return true;
    }
    return false;
}

void ClipProcessingEditorComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void ClipProcessingEditorComponent::resized()
{
    auto bounds = getLocalBounds().reduced(14);
    valueLabel.setBounds(bounds.removeFromTop(20));
    value.setBounds(bounds.removeFromTop(32));
    if (mode == Mode::fadeIn || mode == Mode::fadeOut)
    {
        bounds.removeFromTop(10);
        curveLabel.setBounds(bounds.removeFromTop(20));
        curve.setBounds(bounds.removeFromTop(32));
    }
    auto buttons = bounds.removeFromBottom(32);
    cancelButton.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(8);
    applyButton.setBounds(buttons.removeFromRight(90));
    bounds.removeFromTop(4);
    bounds.removeFromBottom(4);
    status.setBounds(bounds);
}
}
