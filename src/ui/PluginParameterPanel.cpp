#include "PluginParameterPanel.h"

#include "StudioTheme.h"

namespace studio
{
PluginParameterPanel::PluginParameterPanel(
    juce::String insertName,
    juce::String insertId,
    std::vector<PluginParameterDescriptor> parameters)
    : name(std::move(insertName)),
      id(std::move(insertId)),
      descriptors(std::move(parameters))
{
    for (std::size_t index = 0; index < descriptors.size(); ++index)
        parameter.addItem(descriptors[index].name,
                          static_cast<int>(index + 1));
    addAndMakeVisible(parameter);
    parameter.onChange = [this] { selectParameter(); };

    value.setRange(0.0, 1.0, 0.0001);
    value.setSliderStyle(juce::Slider::LinearHorizontal);
    value.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 24);
    value.onValueChange = [this]
    {
        if (rebuilding
            || parameter.getSelectedItemIndex() < 0
            || parameter.getSelectedItemIndex()
                >= static_cast<int>(descriptors.size()))
            return;
        auto& descriptor = descriptors[static_cast<std::size_t>(
            parameter.getSelectedItemIndex())];
        descriptor.value = static_cast<float>(value.getValue());
        if (onValueChanged)
            onValueChanged(id, descriptor.index, descriptor.value);
    };
    addAndMakeVisible(value);

    valueLabel.setText("Normalized value", juce::dontSendNotification);
    valueLabel.setColour(juce::Label::textColourId,
                         juce::Colour(StudioColours::secondaryText));
    addAndMakeVisible(valueLabel);

    if (!descriptors.empty())
        parameter.setSelectedId(1, juce::sendNotificationSync);
    setSize(440, 170);
}

void PluginParameterPanel::selectParameter()
{
    const auto index = parameter.getSelectedItemIndex();
    if (index < 0 || index >= static_cast<int>(descriptors.size()))
        return;
    rebuilding = true;
    value.setValue(descriptors[static_cast<std::size_t>(index)].value,
                   juce::dontSendNotification);
    value.setEnabled(
        descriptors[static_cast<std::size_t>(index)].automatable);
    rebuilding = false;
}

void PluginParameterPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    graphics.drawFittedText(name,
                            14,
                            10,
                            getWidth() - 28,
                            24,
                            juce::Justification::centredLeft,
                            1);
}

void PluginParameterPanel::resized()
{
    auto bounds = getLocalBounds().reduced(14);
    bounds.removeFromTop(36);
    parameter.setBounds(bounds.removeFromTop(32));
    bounds.removeFromTop(12);
    valueLabel.setBounds(bounds.removeFromTop(20));
    value.setBounds(bounds.removeFromTop(36));
}
}
