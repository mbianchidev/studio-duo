#include "DrumPerformanceComponent.h"

#include "StudioTheme.h"
#include "midi/MidiEditing.h"

#include <algorithm>

namespace studio
{
namespace
{
int normalizedKey(int key)
{
    return key >= 'a' && key <= 'z' ? key - 'a' + 'A' : key;
}
}

void DrumPerformanceComponent::PadButton::paintButton(
    juce::Graphics& graphics, bool over, bool down)
{
    const auto accent = juce::Colour(StudioColours::orange);
    const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    graphics.setColour(
        sounding || down ? accent.withAlpha(0.28f)
                         : juce::Colour(over ? StudioColours::panel
                                             : StudioColours::raised));
    graphics.fillRoundedRectangle(bounds, 5.0f);
    graphics.setColour(
        hasKeyboardFocus(true) || getToggleState() || sounding
            ? accent : juce::Colour(StudioColours::border));
    graphics.drawRoundedRectangle(
        bounds, 5.0f, hasKeyboardFocus(true) || getToggleState() ? 2.0f : 1.0f);

    auto text = getLocalBounds().reduced(8, 5);
    auto badge = text.removeFromTop(18);
    graphics.setColour(accent);
    graphics.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    graphics.drawText(juce::String::charToString(static_cast<juce::juce_wchar>(keyCode)),
                      badge, juce::Justification::centredLeft);
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(10.0f);
    graphics.drawText(juce::String(noteNumber), badge,
                      juce::Justification::centredRight);
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(13.0f);
    graphics.drawFittedText(soundName, text, juce::Justification::centredLeft, 2);
}

DrumPerformanceComponent::DrumPerformanceComponent()
{
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::focusContainer);
    setTitle("Drum performance pads");
    for (auto* component : std::initializer_list<juce::Component*> {
             &keyboardButton, &playButton, &recordButton, &clickButton,
             &transportLabel, &selectedPadLabel, &soundLabel, &soundSelector,
             &velocityLabel, &velocitySlider, &channelLabel, &channelSelector,
             &bindKeyButton, &detailsLabel, &hintLabel })
        addAndMakeVisible(*component);

    keyboardButton.setClickingTogglesState(true);
    keyboardButton.setTooltip("Play pad keys while this panel has focus. Escape switches keyboard mode off.");
    keyboardButton.onClick = [this]
    {
        setKeyboardEnabled(keyboardButton.getToggleState());
    };
    playButton.setTooltip("Play or pause the song and click (Space)");
    playButton.onClick = [this] { if (onPlay) onPlay(); };
    recordButton.setTooltip("Record this track with the song's count-in, loop, and punch settings");
    recordButton.onClick = [this] { if (onRecord) onRecord(); };
    clickButton.setTooltip("Toggle the song's metronome");
    clickButton.onClick = [this] { if (onClick) onClick(); };
    transportLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    soundLabel.setText("SOUND / ARTICULATION", juce::dontSendNotification);
    soundLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    soundSelector.setTitle("Selected drum pad sound");
    soundSelector.setTooltip("Choose the MIDI note and articulation played by the selected pad");
    soundSelector.onChange = [this]
    {
        if (soundSelector.getSelectedId() > 0)
            showFailure(assignSound(selectedPad, soundSelector.getSelectedId() - 1));
    };
    velocityLabel.setText("VELOCITY", juce::dontSendNotification);
    velocityLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    velocitySlider.setSliderStyle(juce::Slider::LinearHorizontal);
    velocitySlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 42, 22);
    velocitySlider.setRange(1.0, 127.0, 1.0);
    velocitySlider.setValue(100.0, juce::dontSendNotification);
    velocitySlider.setTitle("Drum hit velocity");
    velocitySlider.setTooltip("Velocity for mouse and keyboard hits; hold Shift for full-velocity accents");
    channelLabel.setText("CHANNEL", juce::dontSendNotification);
    channelLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    channelSelector.setTitle("Drum MIDI channel");
    for (int channel = 1; channel <= 16; ++channel)
        channelSelector.addItem(juce::String(channel), channel);
    channelSelector.setSelectedId(1, juce::dontSendNotification);
    channelSelector.onChange = [this] { releaseAllNotes(); };
    bindKeyButton.setTooltip("Assign an unused letter or number to the selected pad");
    bindKeyButton.onClick = [this]
    {
        setKeyboardEnabled(false);
        learningKey = true;
        if (isShowing())
            grabKeyboardFocus();
        refreshHint();
    };
    detailsLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    detailsLabel.setJustificationType(juce::Justification::topLeft);
    hintLabel.setFont(juce::Font(juce::FontOptions(11.0f)));

    for (std::size_t index = 0; index < pads.size(); ++index)
    {
        auto& pad = pads[index];
        addAndMakeVisible(pad);
        pad.setComponentID("drum-pad-" + juce::String(static_cast<int>(index)));
        pad.setMouseClickGrabsKeyboardFocus(false);
        pad.setTriggeredOnMouseDown(true);
        pad.onClick = [this, index]
        {
            if (keyboardEnabled && isShowing())
                grabKeyboardFocus();
            selectPad(index);
            pressPad(index, false, static_cast<int>(velocitySlider.getValue()));
        };
        pad.onStateChange = [this, index]
        {
            if (!pads[index].isDown())
                releasePad(index, false);
        };
    }
    startTimerHz(60);
    refreshHint();
}

DrumPerformanceComponent::~DrumPerformanceComponent()
{
    stopTimer();
    releaseAllNotes();
}

void DrumPerformanceComponent::setContext(
    const juce::String& trackId, const MidiClip* clip, const DrumMap* map)
{
    const auto clipId = clip != nullptr ? clip->id : juce::String();
    auto nextBindings = clip != nullptr && !clip->drumPadBindings.empty()
        ? clip->drumPadBindings : defaultDrumPadBindings(map);
    if (selectedTrackId != trackId || selectedClipId != clipId)
        setKeyboardEnabled(false);
    else if (bindings != nextBindings)
        releaseAllNotes();
    selectedTrackId = trackId;
    selectedClipId = clipId;
    bindings = std::move(nextBindings);
    drumMap = map != nullptr ? std::optional<DrumMap>(*map) : std::nullopt;

    soundSelector.clear(juce::dontSendNotification);
    if (map != nullptr)
    {
        soundSelector.addSectionHeading(map->name);
        for (const auto& entry : map->entries)
            soundSelector.addItem(
                entry.name + " / " + entry.articulation + " (" + juce::String(entry.noteNumber) + ")",
                entry.noteNumber + 1);
        soundSelector.addSectionHeading("Other MIDI notes");
    }
    for (int note = 0; note < 128; ++note)
    {
        const auto* entry = map != nullptr ? map->entryForPitch(note) : nullptr;
        if (entry != nullptr && entry->noteNumber == note)
            continue;
        soundSelector.addItem(
            entry != nullptr
                ? entry->name + " / " + entry->articulation + " (" + juce::String(note) + ")"
                : "MIDI " + juce::String(note),
            note + 1);
    }
    const auto enabled = selectedTrackId.isNotEmpty() && selectedClipId.isNotEmpty();
    for (auto* child : getChildren())
        child->setEnabled(enabled);
    refreshPads();
    refreshAuditionState();
}

void DrumPerformanceComponent::setKeyboardEnabled(bool enabled)
{
    if (!enabled)
        releaseAllNotes();
    keyboardEnabled = enabled && selectedTrackId.isNotEmpty() && selectedClipId.isNotEmpty();
    learningKey = false;
    keyboardButton.setToggleState(keyboardEnabled, juce::dontSendNotification);
    keyboardButton.setButtonText(keyboardEnabled ? "KEYBOARD ON" : "KEYBOARD OFF");
    if (keyboardEnabled && isShowing())
        grabKeyboardFocus();
    refreshHint();
}

bool DrumPerformanceComponent::isKeyboardEnabled() const noexcept
{
    return keyboardEnabled;
}

juce::Result DrumPerformanceComponent::assignSound(std::size_t pad, int noteNumber)
{
    if (pad >= bindings.size() || noteNumber < 0 || noteNumber > 127)
        return juce::Result::fail("Choose a pad and a MIDI sound from 0 to 127.");
    auto binding = bindings[pad];
    binding.noteNumber = noteNumber;
    return changeBinding(pad, binding);
}

juce::Result DrumPerformanceComponent::assignKey(std::size_t pad, int keyCode)
{
    keyCode = normalizedKey(keyCode);
    if (pad >= bindings.size() || !isDrumPadKey(keyCode))
        return juce::Result::fail("Choose a letter A-Z or a number 0-9. Transport keys are reserved.");
    for (std::size_t index = 0; index < bindings.size(); ++index)
        if (index != pad && bindings[index].keyCode == keyCode)
            return juce::Result::fail("That key is already assigned to another pad.");
    auto binding = bindings[pad];
    binding.keyCode = keyCode;
    return changeBinding(pad, binding);
}

juce::Result DrumPerformanceComponent::changeBinding(std::size_t pad, DrumPadBinding binding)
{
    if (bindings[pad] == binding)
        return juce::Result::ok();
    releaseAllNotes();
    auto after = bindings;
    after[pad] = binding;
    if (onBindingsEdited && !onBindingsEdited(after))
        return juce::Result::fail("The drum-pad assignment could not be saved.");
    bindings = std::move(after);
    refreshPads();
    return juce::Result::ok();
}

void DrumPerformanceComponent::pressPad(std::size_t pad, bool keyboard, int velocity)
{
    if (pad >= bindings.size() || selectedTrackId.isEmpty() || selectedClipId.isEmpty())
        return;
    auto& held = keyboard ? heldKeys[pad] : heldMouse[pad];
    if (held)
        return;
    const auto alreadySounding = heldKeys[pad] || heldMouse[pad];
    held = true;
    if (alreadySounding)
        return;
    selectPad(pad);
    const auto channel = channelSelector.getSelectedId();
    const auto pitch = bindings[pad].noteNumber;
    MidiNote note;
    note.pitch = pitch;
    applyDrumMapMetadata(note, drumMap ? &*drumMap : nullptr);
    const auto* entry = drumMap ? drumMap->entryForPitch(pitch) : nullptr;
    if (entry != nullptr && entry->footControlCC >= 0 && note.footControlValue >= 0
        && !send(juce::MidiMessage::controllerEvent(
            channel, entry->footControlCC, note.footControlValue)))
    {
        held = false;
        return;
    }
    if (!send(juce::MidiMessage::noteOn(
            channel, pitch, static_cast<juce::uint8>(juce::jlimit(1, 127, velocity)))))
    {
        held = false;
        return;
    }
    soundingChannels[pad] = channel;
    pads[pad].sounding = true;
    pads[pad].repaint();
}

void DrumPerformanceComponent::releasePad(std::size_t pad, bool keyboard)
{
    auto& held = keyboard ? heldKeys[pad] : heldMouse[pad];
    if (!held)
        return;
    held = false;
    if (heldKeys[pad] || heldMouse[pad])
        return;
    if (pad < bindings.size() && soundingChannels[pad] > 0)
        send(juce::MidiMessage::noteOff(soundingChannels[pad], bindings[pad].noteNumber));
    soundingChannels[pad] = 0;
    pads[pad].sounding = false;
    pads[pad].repaint();
}

void DrumPerformanceComponent::releaseAllNotes()
{
    for (std::size_t index = 0; index < pads.size(); ++index)
    {
        releasePad(index, true);
        releasePad(index, false);
    }
}

bool DrumPerformanceComponent::send(const juce::MidiMessage& message)
{
    const auto result = onMidiMessage
        ? onMidiMessage(selectedTrackId, message)
        : juce::Result::fail("Drum MIDI input is not connected.");
    showFailure(result);
    return result.wasOk();
}

void DrumPerformanceComponent::showFailure(const juce::Result& result)
{
    if (result.wasOk())
        return;
    hintLabel.setText(result.getErrorMessage(), juce::dontSendNotification);
    if (onStatus)
        onStatus(result.getErrorMessage(), true);
}

bool DrumPerformanceComponent::performanceHasFocus() const
{
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    return focused == nullptr || focused == this
        || std::any_of(pads.cbegin(), pads.cend(), [focused](const auto& pad)
        {
            return focused == &pad;
        });
}

bool DrumPerformanceComponent::keyPressed(const juce::KeyPress& key)
{
    if ((!keyboardEnabled && !learningKey) || !performanceHasFocus())
        return false;
    const auto modifiers = key.getModifiers();
    if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown())
        return false;
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        setKeyboardEnabled(false);
        return true;
    }
    const auto keyCode = normalizedKey(key.getKeyCode());
    if (learningKey)
    {
        const auto result = assignKey(selectedPad, keyCode);
        if (result.wasOk())
        {
            learningKey = false;
            refreshHint();
        }
        else
            showFailure(result);
        return true;
    }
    for (std::size_t index = 0; index < bindings.size(); ++index)
        if (bindings[index].keyCode == keyCode)
        {
            pressPad(index, true, modifiers.isShiftDown()
                ? 127 : static_cast<int>(velocitySlider.getValue()));
            return true;
        }
    return isDrumPadKey(keyCode);
}

bool DrumPerformanceComponent::keyStateChanged(bool)
{
    auto released = false;
    for (std::size_t index = 0; index < bindings.size(); ++index)
        if (heldKeys[index] && !juce::KeyPress::isKeyCurrentlyDown(bindings[index].keyCode))
        {
            releasePad(index, true);
            released = true;
        }
    return released;
}

void DrumPerformanceComponent::timerCallback()
{
    refreshAuditionState();
    if (!auditionActive)
    {
        if (keyboardEnabled || learningKey
            || std::any_of(heldMouse.cbegin(), heldMouse.cend(), [](bool held) { return held; }))
            setKeyboardEnabled(false);
        return;
    }
    if (!hasKeyboardFocus(true))
    {
        if (keyboardEnabled || learningKey)
            setKeyboardEnabled(false);
    }
    keyStateChanged(false);
    for (std::size_t index = 0; index < pads.size(); ++index)
        if (heldMouse[index] && !pads[index].isMouseButtonDown())
            releasePad(index, false);
}

void DrumPerformanceComponent::focusLost(FocusChangeType)
{
    if (!hasKeyboardFocus(true))
        setKeyboardEnabled(false);
    repaint();
}

void DrumPerformanceComponent::visibilityChanged()
{
    if (!isShowing())
        setKeyboardEnabled(false);
    refreshAuditionState();
}

void DrumPerformanceComponent::parentHierarchyChanged()
{
    visibilityChanged();
}

void DrumPerformanceComponent::refreshAuditionState()
{
    const auto active = isShowing() && selectedClipId.isNotEmpty();
    if (auditionActive == active)
        return;
    auditionActive = active;
    if (onAuditionChanged)
        onAuditionChanged(active);
}

void DrumPerformanceComponent::selectPad(std::size_t pad)
{
    selectedPad = pad;
    for (std::size_t index = 0; index < pads.size(); ++index)
        pads[index].setToggleState(index == selectedPad, juce::dontSendNotification);
    refreshSelectedPad();
}

void DrumPerformanceComponent::refreshPads()
{
    for (std::size_t index = 0; index < pads.size() && index < bindings.size(); ++index)
    {
        auto& pad = pads[index];
        const auto& binding = bindings[index];
        const auto* entry = drumMap ? drumMap->entryForPitch(binding.noteNumber) : nullptr;
        pad.soundName = entry != nullptr ? entry->name : "MIDI " + juce::String(binding.noteNumber);
        pad.noteNumber = binding.noteNumber;
        pad.keyCode = binding.keyCode;
        const auto title = pad.soundName + ", key "
            + juce::String::charToString(static_cast<juce::juce_wchar>(binding.keyCode));
        pad.setButtonText(title);
        pad.setTitle(title);
        pad.setTooltip(title + ". Click or press Enter to audition; select a sound on the right.");
        pad.repaint();
    }
    selectPad(selectedPad);
}

void DrumPerformanceComponent::refreshSelectedPad()
{
    if (selectedPad >= bindings.size())
        return;
    selectedPadLabel.setText(pads[selectedPad].soundName, juce::dontSendNotification);
    soundSelector.setSelectedId(bindings[selectedPad].noteNumber + 1, juce::dontSendNotification);
    bindKeyButton.setButtonText("BIND KEY: "
        + juce::String::charToString(static_cast<juce::juce_wchar>(bindings[selectedPad].keyCode)));
    const auto* entry = drumMap ? drumMap->entryForPitch(bindings[selectedPad].noteNumber) : nullptr;
    detailsLabel.setText(
        entry != nullptr ? entry->outputGroup + " / " + entry->articulation
            + "\n" + (drumMap ? drumMap->name : juce::String())
                         : "Uses the instrument's sound for this MIDI note.",
        juce::dontSendNotification);
}

void DrumPerformanceComponent::refreshHint()
{
    hintLabel.setText(
        learningKey ? "Press an unused letter or number for this pad. Escape cancels."
        : keyboardEnabled ? "Pad keys play. Shift accents. Space plays/pauses. Escape exits keyboard mode."
                          : "Click pads or enable Keyboard. Choose a sound on the right. Record adds an editable MIDI take.",
        juce::dontSendNotification);
}

void DrumPerformanceComponent::setTransportState(
    const juce::String& position, bool playing, bool recording,
    bool clickEnabled, bool countingIn)
{
    transportLabel.setText(
        (countingIn ? "COUNT-IN  " : recording ? "REC  " : "") + position,
        juce::dontSendNotification);
    playButton.setButtonText(playing ? "PAUSE" : "PLAY");
    recordButton.setButtonText(recording ? "STOP TAKE" : "RECORD");
    recordButton.setToggleState(recording, juce::dontSendNotification);
    clickButton.setToggleState(clickEnabled, juce::dontSendNotification);
}

void DrumPerformanceComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    if (hasKeyboardFocus(false))
    {
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.drawRect(getLocalBounds().reduced(1), 2);
    }
}

void DrumPerformanceComponent::resized()
{
    auto area = getLocalBounds().reduced(6);
    auto toolbar = area.removeFromTop(28);
    keyboardButton.setBounds(toolbar.removeFromLeft(112).reduced(2));
    playButton.setBounds(toolbar.removeFromLeft(62).reduced(2));
    recordButton.setBounds(toolbar.removeFromLeft(84).reduced(2));
    clickButton.setBounds(toolbar.removeFromLeft(64).reduced(2));
    transportLabel.setBounds(toolbar);
    hintLabel.setBounds(area.removeFromBottom(22));
    area.removeFromTop(6);
    auto details = area.removeFromRight(getWidth() < 700 ? 180 : 220);
    area.removeFromRight(8);
    selectedPadLabel.setBounds(details.removeFromTop(23));
    soundLabel.setBounds(details.removeFromTop(18));
    soundSelector.setBounds(details.removeFromTop(27).reduced(2));
    velocityLabel.setBounds(details.removeFromTop(18));
    velocitySlider.setBounds(details.removeFromTop(26));
    auto channelRow = details.removeFromTop(26);
    channelLabel.setBounds(channelRow.removeFromLeft(82));
    channelSelector.setBounds(channelRow.reduced(2));
    bindKeyButton.setBounds(details.removeFromTop(28).reduced(2));
    detailsLabel.setBounds(details.reduced(2, 3));
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
        {
            const auto left = area.getX() + column * area.getWidth() / 4;
            const auto top = area.getY() + row * area.getHeight() / 3;
            const auto right = area.getX() + (column + 1) * area.getWidth() / 4;
            const auto bottom = area.getY() + (row + 1) * area.getHeight() / 3;
            pads[static_cast<std::size_t>(row * 4 + column)].setBounds(
                juce::Rectangle<int>(left, top, right - left, bottom - top).reduced(3));
        }
}
}
