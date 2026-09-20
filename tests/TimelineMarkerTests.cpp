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
}
