#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/MixerPanel.h"
#include "ui/NumericInput.h"

#include <cmath>

namespace
{
juce::MouseEvent mixerMouseEvent(
    juce::Component& component,
    juce::Point<float> position,
    juce::Point<float> mouseDownPosition,
    bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        {},
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        &component,
        &component,
        now,
        mouseDownPosition,
        now,
        1,
        dragged
    };
}
}

void mixerPanelTests()
{
    expect(std::abs(
               studio::parseDecibels("-6").value_or(99.0)
               + 6.0) < 0.000001
               && std::abs(
                      studio::parseDecibels("-6 dB").value_or(99.0)
                      + 6.0) < 0.000001
               && std::abs(
                      studio::parseDecibels("-6db").value_or(99.0)
                      + 6.0) < 0.000001
               && !studio::parseDecibels("-6 dBx").has_value(),
           "Mixer dB input accepts an optional case-insensitive suffix and rejects trailing text.");
    expect(std::abs(
               studio::parseTrackDecibels("-6.24 dB")
                   .value_or(99.0f)
               + 6.2f) < 0.0001f
               && studio::parseTrackDecibels("-60").has_value()
               && studio::parseTrackDecibels("+12 dB").has_value()
               && !studio::parseTrackDecibels("-60.1").has_value()
               && !studio::parseTrackDecibels("12.1").has_value(),
           "Mixer dB input enforces the real -60 to +12 range and 0.1 dB step.");
    expect(studio::formatDecibels(6.0) == "+6.0 dB"
               && studio::formatDecibels(0.0) == "0.0 dB"
               && studio::formatDecibels(-6.0) == "-6.0 dB",
           "Positive dB displays include a plus sign while zero and negative values do not.");

    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.pan = 0.0f;

    studio::MixerPanel mixer;
    mixer.setBounds(0, 0, 900, 260);
    mixer.setProject(&project);

    auto pan = 0.0f;
    mixer.onPanChanged = [&pan](const juce::String&, float value)
    {
        pan = value;
    };

    const juce::Point<float> panCentre(70.0f, 217.0f);
    const juce::Point<float> panRight(126.0f, 217.0f);
    mixer.mouseDown(mixerMouseEvent(
        mixer,
        panCentre,
        panCentre,
        false));
    mixer.mouseDrag(mixerMouseEvent(
        mixer,
        panRight,
        panCentre,
        true));
    mixer.mouseUp(mixerMouseEvent(
        mixer,
        panRight,
        panCentre,
        true));
    expect(pan > 0.99f,
           "Dragging the mixer pan control right produces full-right pan.");

    const juce::Point<float> panLeft(14.0f, 217.0f);
    mixer.mouseDown(mixerMouseEvent(
        mixer,
        panCentre,
        panCentre,
        false));
    mixer.mouseDrag(mixerMouseEvent(
        mixer,
        panLeft,
        panCentre,
        true));
    mixer.mouseUp(mixerMouseEvent(
        mixer,
        panLeft,
        panCentre,
        true));
    expect(pan < -0.99f,
           "Dragging the mixer pan control left produces full-left pan.");

    juce::String mutedTrack;
    mixer.onTrackMute = [&mutedTrack](const juce::String& trackId)
    {
        mutedTrack = trackId;
    };
    const juce::Point<float> muteControl(37.0f, 73.0f);
    mixer.mouseDown(mixerMouseEvent(
        mixer,
        muteControl,
        muteControl,
        false));
    expect(mutedTrack == track.id,
           "Mixer strip mute icons target the clicked track.");

    auto appliedVolume = 99.0f;
    mixer.onVolumeChanged =
        [&appliedVolume](const juce::String&, float value)
    {
        appliedVolume = value;
    };
    const juce::Point<float> decibelReadout(70.0f, 98.0f);
    mixer.mouseDown(mixerMouseEvent(
        mixer,
        decibelReadout,
        decibelReadout,
        false));
    juce::TextEditor* inlineEditor = nullptr;
    for (auto index = 0;
         index < mixer.getNumChildComponents();
         ++index)
    {
        if (auto* editor = dynamic_cast<juce::TextEditor*>(
                mixer.getChildComponent(index));
            editor != nullptr && editor->isVisible())
        {
            inlineEditor = editor;
            break;
        }
    }
    if (inlineEditor != nullptr)
    {
        inlineEditor->setText("-6db", false);
        if (inlineEditor->onReturnKey)
            inlineEditor->onReturnKey();
    }
    expect(inlineEditor != nullptr
               && std::abs(appliedVolume + 6.0f) < 0.0001f,
           "Clicking the mixer dB readout edits and applies volume inline.");
}
