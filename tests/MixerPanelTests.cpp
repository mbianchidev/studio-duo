#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/MixerPanel.h"

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
}
