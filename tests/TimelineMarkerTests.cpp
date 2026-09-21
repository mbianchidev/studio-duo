#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/TimelineComponent.h"

#include <cmath>

namespace
{
juce::MouseEvent mouseEvent(
    juce::Component& component,
    juce::Point<float> position,
    juce::Point<float> mouseDownPosition,
    int clicks,
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
        clicks,
        dragged
    };
}
}

void timelineMarkerTests()
{
    auto project = studio::Project::createDefault();
    project.markers = {
        { "marker", "Flag", 1.0 }
    };
    project.sections = {
        { "section", "Verse", 0.0 }
    };
    project.sections.front().endTimeSeconds = 4.0;

    studio::TimelineComponent timeline;
    timeline.setBounds(0, 0, 1200, 400);
    timeline.setProject(&project);

    juce::String movedId;
    double movedSeconds = -1.0;
    timeline.onMoveMarkerRequested =
        [&movedId, &movedSeconds](
            const juce::String& markerId,
            double seconds)
    {
        movedId = markerId;
        movedSeconds = seconds;
    };

    const auto markerPosition = juce::Point<float>(
        timeline.xForSeconds(1.0),
        8.0f);
    const auto movedPosition = juce::Point<float>(
        timeline.xForSeconds(3.0),
        8.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        markerPosition,
        markerPosition,
        1,
        false));
    timeline.mouseDrag(mouseEvent(
        timeline,
        movedPosition,
        markerPosition,
        1,
        true));
    timeline.mouseUp(mouseEvent(
        timeline,
        movedPosition,
        markerPosition,
        1,
        true));
    expect(movedId == "marker"
               && std::abs(movedSeconds - 3.0) < 0.000001,
           "Dragging a marker flag requests a stable-ID move to the new timeline position.");

    timeline.setEditGridBeats(0.25);
    const auto offGridPosition = juce::Point<float>(
        timeline.xForSeconds(3.13),
        8.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        markerPosition,
        markerPosition,
        1,
        false));
    timeline.mouseDrag(mouseEvent(
        timeline,
        offGridPosition,
        markerPosition,
        1,
        true));
    timeline.mouseUp(mouseEvent(
        timeline,
        offGridPosition,
        markerPosition,
        1,
        true));
    expect(std::abs(movedSeconds - 3.125) < 0.000001,
           "Marker edits snap to the visible sixteenth-note grid.");

    auto markerAdds = 0;
    auto sectionEdits = 0;
    timeline.onAddMarkerRequested = [&markerAdds](double)
    {
        ++markerAdds;
    };
    timeline.onEditSectionRequested = [&sectionEdits](const juce::String&)
    {
        ++sectionEdits;
    };
    const auto emptyPosition = juce::Point<float>(
        timeline.xForSeconds(5.0),
        8.0f);
    timeline.mouseDoubleClick(mouseEvent(
        timeline,
        emptyPosition,
        emptyPosition,
        2,
        false));
    expect(markerAdds == 1 && sectionEdits == 0,
           "Double-clicking the shared ruler adds a marker flag without treating the underlying song section as a marker.");

    const auto sectionPosition = juce::Point<float>(
        timeline.xForSeconds(0.5),
        30.0f);
    timeline.mouseDoubleClick(mouseEvent(
        timeline,
        sectionPosition,
        sectionPosition,
        2,
        false));
    expect(markerAdds == 1 && sectionEdits == 1,
           "The dedicated section row edits its song section without creating a marker.");

    auto rangeStart = -1.0;
    auto rangeEnd = -1.0;
    timeline.onSectionRangeChanged =
        [&rangeStart, &rangeEnd](
            const juce::String&,
            double start,
            double end)
    {
        rangeStart = start;
        rangeEnd = end;
    };
    const auto sectionBody = juce::Point<float>(
        timeline.xForSeconds(1.0),
        30.0f);
    const auto movedSection = juce::Point<float>(
        timeline.xForSeconds(2.0),
        30.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        sectionBody,
        sectionBody,
        1,
        false));
    timeline.mouseDrag(mouseEvent(
        timeline,
        movedSection,
        sectionBody,
        1,
        true));
    timeline.mouseUp(mouseEvent(
        timeline,
        movedSection,
        sectionBody,
        1,
        true));
    expect(std::abs(rangeStart - 1.0) < 0.000001
               && std::abs(rangeEnd - 5.0) < 0.000001,
           "Dragging a section body moves its complete range.");

    const auto sectionEnd = juce::Point<float>(
        timeline.xForSeconds(4.0),
        30.0f);
    const auto resizedSection = juce::Point<float>(
        timeline.xForSeconds(6.0),
        30.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        sectionEnd,
        sectionEnd,
        1,
        false));
    timeline.mouseDrag(mouseEvent(
        timeline,
        resizedSection,
        sectionEnd,
        1,
        true));
    timeline.mouseUp(mouseEvent(
        timeline,
        resizedSection,
        sectionEnd,
        1,
        true));
    expect(std::abs(rangeStart) < 0.000001
               && std::abs(rangeEnd - 6.0) < 0.000001,
           "Dragging a section end resizes the range.");

    juce::String inputTrack;
    timeline.onInputMenuRequested =
        [&inputTrack](const juce::String& trackId,
                      juce::Rectangle<int>)
    {
        inputTrack = trackId;
    };
    const juce::Point<float> inputControl(136.0f, 118.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        inputControl,
        inputControl,
        1,
        false));
    expect(inputTrack == project.tracks.front().id,
           "Timeline input dropdowns target the clicked audio track.");

    juce::String mutedTrack;
    timeline.onTrackMute = [&mutedTrack](const juce::String& trackId)
    {
        mutedTrack = trackId;
    };
    const juce::Point<float> muteControl(35.0f, 139.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        muteControl,
        muteControl,
        1,
        false));
    expect(mutedTrack == project.tracks.front().id,
           "Timeline track mute icons target the clicked track.");

    auto volume = -100.0f;
    timeline.onTrackVolumeChanged =
        [&volume](const juce::String&, float value)
    {
        volume = value;
    };
    const juce::Point<float> volumeControl(164.0f, 139.0f);
    timeline.mouseDown(mouseEvent(
        timeline,
        volumeControl,
        volumeControl,
        1,
        false));
    timeline.mouseUp(mouseEvent(
        timeline,
        volumeControl,
        volumeControl,
        1,
        false));
    expect(volume > 11.9f,
           "Timeline track volume faders expose their dB range through direct manipulation.");

    auto scrollProject =
        studio::Project::createDefault();
    for (auto index = 0; index < 10; ++index)
    {
        studio::Track extra;
        extra.name =
            "Scroll track "
            + juce::String(index + 1);
        scrollProject.tracks.insert(
            scrollProject.tracks.end() - 1,
            std::move(extra));
    }
    studio::TimelineComponent scrollingTimeline;
    juce::Viewport viewport;
    viewport.setBounds(0, 0, 640, 240);
    viewport.setViewedComponent(
        &scrollingTimeline,
        false);
    scrollingTimeline.setProject(&scrollProject);
    scrollingTimeline.setSize(
        scrollingTimeline.preferredWidth(
            viewport.getWidth()),
        scrollingTimeline.preferredHeight(
            viewport.getHeight()));
    viewport.setViewPosition(0, 100);
    scrollingTimeline.setViewportPosition(0);
    auto zoomRequests = 0;
    scrollingTimeline.onZoomRequested =
        [&zoomRequests](double, double)
    {
        ++zoomRequests;
    };
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = -1.0f;
    const auto headerWheel =
        juce::Point<float>(80.0f, 180.0f);
    scrollingTimeline.mouseWheelMove(
        mouseEvent(
            scrollingTimeline,
            headerWheel,
            headerWheel,
            1,
            false),
        wheel);
    expect(viewport.getViewPositionY() > 100
               && zoomRequests == 0,
           "Wheel input over track headers scrolls tracks vertically without zooming.");

    const auto timelineWheel =
        juce::Point<float>(320.0f, 180.0f);
    const auto previousScroll =
        viewport.getViewPositionY();
    scrollingTimeline.mouseWheelMove(
        mouseEvent(
            scrollingTimeline,
            timelineWheel,
            timelineWheel,
            1,
            false),
        wheel);
    expect(viewport.getViewPositionY()
                   == previousScroll
               && zoomRequests == 1,
           "Wheel input over the timeline canvas keeps the existing zoom behavior.");
    viewport.setViewedComponent(nullptr, false);
}
