#include "ControlSurfaceSettingsComponent.h"
#include "StudioTheme.h"

namespace studio
{
ControlSurfaceSettingsComponent::ControlSurfaceSettingsComponent(
    ControlSurfaceSession& sessionToUse, const ControlSurfaceSettings& settings,
    std::function<juce::Result(const ControlSurfaceSettings&)> applyToUse)
    : session(sessionToUse), apply(std::move(applyToUse))
{
    for (auto* label : { &title, &help, &protocolLabel, &inputLabel, &outputLabel, &connectionStatus })
        addAndMakeVisible(*label);
    title.setText("Control surfaces", juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    help.setText("Use a dedicated controller in Mackie Control mode: eight strips plus master. "
        "Disable these ports in Audio / MIDI's musical assignments first. Control input is excluded from musical recording. "
        "Do not select a synthesizer or an output-to-input MIDI loop.", juce::dontSendNotification);
    help.setJustificationType(juce::Justification::topLeft);
    help.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    protocolLabel.setText("Protocol", juce::dontSendNotification);
    inputLabel.setText("Dedicated MIDI input", juce::dontSendNotification);
    outputLabel.setText("Dedicated MIDI output", juce::dontSendNotification);
    connectionStatus.setTitle("Control-surface connection status");
    connectionStatus.setJustificationType(juce::Justification::topLeft);
    protocol.setTitle("Control-surface protocol");
    protocol.addItem("Disabled", 1);
    protocol.addItem("Mackie Control (8 strips + master)", 2);
    protocol.setSelectedId(settings.enabled() ? 2 : 1, juce::dontSendNotification);
    input.setTitle("Dedicated control input");
    input.setDescription("Reserved for control commands, not musical note or CC recording");
    output.setTitle("Dedicated control output");
    output.setDescription("Controller feedback only; do not choose an instrument");
    input.setTextWhenNothingSelected("Choose a dedicated input");
    output.setTextWhenNothingSelected("Choose a dedicated output");
    auto order = 1;
    for (auto* selector : { &protocol, &input, &output })
    {
        addAndMakeVisible(*selector);
        selector->setWantsKeyboardFocus(true);
        selector->setExplicitFocusOrder(order++);
        selector->onChange = [this] { localError.clear(); refreshStatus(); };
    }
    for (auto* button : { &refresh, &reconnect, &save })
    {
        addAndMakeVisible(*button);
        button->setWantsKeyboardFocus(true);
        button->setExplicitFocusOrder(order++);
    }
    refresh.setTitle("Refresh control-surface ports");
    reconnect.setTitle("Reconnect saved control-surface ports");
    save.setTitle("Apply control-surface settings");
    reconnect.setTooltip("Retry the saved ports. Unsaved selections are not applied.");
    refresh.onClick = [this]
    {
        const auto desired = selectedSettings();
        localError.clear();
        refreshPorts(desired);
    };
    reconnect.onClick = [this] { localError.clear(); session.reconnect(); refreshStatus(); };
    save.onClick = [this]
    {
        const auto desired = selectedSettings();
        auto result = desired.validate();
        if (result.wasOk())
            result = apply ? apply(desired) : juce::Result::fail("The control-surface settings handler is unavailable.");
        localError = result.failed() ? result.getErrorMessage() : juce::String();
        refreshStatus();
    };
    refreshPorts(settings);
    startTimerHz(4);
    setSize(680, 480);
}

ControlSurfaceSettings ControlSurfaceSettingsComponent::selectedSettings() const
{
    ControlSurfaceSettings result;
    result.protocol = protocol.getSelectedId() == 2
        ? ControlSurfaceProtocol::mackieControl : ControlSurfaceProtocol::disabled;
    const auto inputIndex = input.getSelectedItemIndex();
    const auto outputIndex = output.getSelectedItemIndex();
    if (juce::isPositiveAndBelow(inputIndex, inputIds.size()))
        result.inputDeviceId = inputIds[inputIndex];
    if (juce::isPositiveAndBelow(outputIndex, outputIds.size()))
        result.outputDeviceId = outputIds[outputIndex];
    return result;
}

void ControlSurfaceSettingsComponent::refreshPorts(const ControlSurfaceSettings& desired)
{
    const auto populate = [](juce::ComboBox& selector, juce::StringArray& ids,
                             const auto& devices, const juce::String& selected)
    {
        selector.clear(juce::dontSendNotification);
        ids.clear();
        for (const auto& device : devices)
        {
            ids.add(device.identifier);
            selector.addItem(device.name, ids.size());
        }
        if (selected.isNotEmpty() && !ids.contains(selected))
        {
            ids.add(selected);
            selector.addItem("Saved device (unavailable)", ids.size());
        }
        const auto index = ids.indexOf(selected);
        selector.setSelectedId(index >= 0 ? index + 1 : 0, juce::dontSendNotification);
    };
    populate(input, inputIds, session.inputs(), desired.inputDeviceId);
    populate(output, outputIds, session.outputs(), desired.outputDeviceId);
    refreshStatus();
}

void ControlSurfaceSettingsComponent::timerCallback() { refreshStatus(); }

void ControlSurfaceSettingsComponent::refreshStatus()
{
    const auto enabled = protocol.getSelectedId() == 2;
    input.setEnabled(enabled && session.hardwareAvailable());
    output.setEnabled(enabled && session.hardwareAvailable());
    refresh.setEnabled(session.hardwareAvailable());
    reconnect.setEnabled(session.hardwareAvailable() && session.settings().enabled());
    const auto message = localError.isNotEmpty() ? localError : !session.hardwareAvailable()
        ? juce::String("MIDI discovery is unavailable. Reopen Settings to retry after checking Startup diagnostics.")
        : session.status();
    connectionStatus.setText(message, juce::dontSendNotification);
    connectionStatus.setColour(juce::Label::textColourId,
        juce::Colour(localError.isNotEmpty() || session.hasError() ? StudioColours::orange : StudioColours::secondaryText));
}

void ControlSurfaceSettingsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void ControlSurfaceSettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced(24);
    title.setBounds(bounds.removeFromTop(32));
    help.setBounds(bounds.removeFromTop(76));
    const auto row = [&bounds](juce::Label& label, juce::ComboBox& selector)
    {
        auto line = bounds.removeFromTop(34);
        label.setBounds(line.removeFromLeft(160));
        selector.setBounds(line);
        bounds.removeFromTop(8);
    };
    row(protocolLabel, protocol);
    row(inputLabel, input);
    row(outputLabel, output);
    bounds.removeFromTop(8);
    auto buttons = bounds.removeFromTop(32);
    save.setBounds(buttons.removeFromRight(120));
    buttons.removeFromRight(10);
    reconnect.setBounds(buttons.removeFromRight(132));
    refresh.setBounds(buttons.removeFromLeft(160));
    bounds.removeFromTop(12);
    connectionStatus.setBounds(bounds);
}
}
