#include "AutomationPanel.h"

#include "StudioTheme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace studio
{
AutomationPanel::AutomationPanel()
{
    for (const auto& [label, automationMode] :
         std::initializer_list<std::pair<const char*, AutomationMode>> {
             { "Read", AutomationMode::read },
             { "Touch", AutomationMode::touch },
             { "Latch", AutomationMode::latch },
             { "Write", AutomationMode::write },
             { "Trim", AutomationMode::trim },
             { "Preview", AutomationMode::preview }
         })
        mode.addItem(label, static_cast<int>(automationMode) + 1);
    value.setRange(0.0, 1.0, 0.001);
    value.setValue(0.5);
    value.setSliderStyle(juce::Slider::LinearHorizontal);
    value.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);

    for (auto* component : std::array<juce::Component*, 11> {
             &mode,
             &armed,
             &lanes,
             &target,
             &beatTime,
             &stepInterpolation,
             &value,
             &addLaneButton,
             &addPointButton,
             &removePointButton,
             &removeLaneButton
         })
        addAndMakeVisible(component);

    mode.onChange = [this]
    {
        if (rebuilding || project == nullptr)
            return;
        const auto* track = project->findTrack(trackId);
        if (track == nullptr || mode.getSelectedId() <= 0)
            return;
        if (onModeChanged)
            onModeChanged(
                trackId,
                static_cast<AutomationMode>(mode.getSelectedId() - 1),
                armed.getToggleState());
    };
    armed.onClick = [this]
    {
        if (rebuilding || project == nullptr)
            return;
        const auto* track = project->findTrack(trackId);
        if (track != nullptr && onModeChanged)
            onModeChanged(trackId,
                          track->automationMode,
                          armed.getToggleState());
    };
    lanes.onChange = [this]
    {
        if (rebuilding)
            return;
        if (const auto* lane = selectedLane())
        {
            beatTime.setToggleState(
                lane->timebase == AutomationTimebase::beats,
                juce::dontSendNotification);
            stepInterpolation.setToggleState(
                lane->interpolation == AutomationInterpolation::step,
                juce::dontSendNotification);
            value.setValue(
                lane->points.empty()
                    ? 0.5
                    : lane->points.back().value,
                juce::dontSendNotification);
        }
    };
    addLaneButton.onClick = [this] { addLane(); };
    addPointButton.onClick = [this] { addPoint(); };
    removePointButton.onClick = [this] { removeNearestPoint(); };
    removeLaneButton.onClick = [this] { removeLane(); };
    setSize(560, 330);
}

void AutomationPanel::setProject(Project* projectToUse)
{
    project = projectToUse;
    refresh();
}

void AutomationPanel::setTrack(const juce::String& trackToUse)
{
    trackId = trackToUse;
    refresh();
}

void AutomationPanel::setPositionSeconds(double valueToUse)
{
    positionSeconds = std::max(0.0, valueToUse);
}

void AutomationPanel::setRuntimeStatuses(
    std::vector<StudioAudioEngine::PluginRuntimeStatus> valueToUse)
{
    runtimeStatuses = std::move(valueToUse);
    refresh();
}

void AutomationPanel::refresh()
{
    rebuilding = true;
    lanes.clear(juce::dontSendNotification);
    target.clear(juce::dontSendNotification);
    laneIds.clear();
    targets.clear();
    const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
    if (track == nullptr)
    {
        rebuilding = false;
        return;
    }

    mode.setSelectedId(
        static_cast<int>(track->automationMode) + 1,
        juce::dontSendNotification);
    armed.setToggleState(track->automationArmed,
                         juce::dontSendNotification);

    const auto addTarget = [this](juce::String label,
                                  AutomationTargetType type)
    {
        AutomationTarget automationTarget;
        automationTarget.type = type;
        automationTarget.trackId = trackId;
        targets.push_back({
            std::move(label),
            std::move(automationTarget)
        });
    };
    addTarget("Track volume", AutomationTargetType::trackVolume);
    addTarget("Track pan", AutomationTargetType::trackPan);
    addTarget("Track mute", AutomationTargetType::trackMute);
    addTarget("Track polarity", AutomationTargetType::trackPolarity);

    for (const auto& route : project->routingConnections)
    {
        if (route.sourceTrackId != trackId
            || route.kind == RouteKind::mainOutput)
            continue;
        AutomationTarget gain;
        gain.type = AutomationTargetType::sendGain;
        gain.trackId = trackId;
        gain.routeId = route.id;
        targets.push_back({
            route.name + " level",
            gain
        });
    }
    for (const auto& status : runtimeStatuses)
    {
        if (status.trackId != trackId)
            continue;
        for (const auto& parameter : status.parameters)
        {
            if (!parameter.automatable)
                continue;
            AutomationTarget pluginTarget;
            pluginTarget.type = AutomationTargetType::pluginParameter;
            pluginTarget.trackId = trackId;
            pluginTarget.insertId = status.insertId;
            pluginTarget.parameterId = parameter.id;
            pluginTarget.parameterIndex = parameter.index;
            targets.push_back({
                status.name + " / " + parameter.name,
                std::move(pluginTarget)
            });
        }
    }
    for (std::size_t index = 0; index < targets.size(); ++index)
        target.addItem(targets[index].label, static_cast<int>(index + 1));
    if (!targets.empty())
        target.setSelectedId(1, juce::dontSendNotification);

    for (const auto& lane : project->automationLanes)
    {
        if (lane.target.trackId != trackId)
            continue;
        laneIds.push_back(lane.id);
        lanes.addItem(lane.name, lanes.getNumItems() + 1);
    }
    if (!laneIds.empty())
        lanes.setSelectedId(1, juce::dontSendNotification);
    rebuilding = false;
}

AutomationLane* AutomationPanel::selectedLane() const
{
    if (project == nullptr
        || lanes.getSelectedItemIndex() < 0
        || lanes.getSelectedItemIndex() >= static_cast<int>(laneIds.size()))
        return nullptr;
    const auto& laneId =
        laneIds[static_cast<std::size_t>(lanes.getSelectedItemIndex())];
    const auto lane = std::find_if(
        project->automationLanes.begin(),
        project->automationLanes.end(),
        [&laneId](const auto& candidate)
        {
            return candidate.id == laneId;
        });
    return lane == project->automationLanes.end() ? nullptr : &*lane;
}

double AutomationPanel::lanePosition(const AutomationLane& lane) const
{
    return lane.timebase == AutomationTimebase::beats && project != nullptr
        ? project->beatsAt(positionSeconds)
        : positionSeconds;
}

void AutomationPanel::addLane()
{
    const auto selected = target.getSelectedItemIndex();
    if (selected < 0 || selected >= static_cast<int>(targets.size()))
        return;
    AutomationLane lane;
    lane.name = targets[static_cast<std::size_t>(selected)].label;
    lane.target = targets[static_cast<std::size_t>(selected)].target;
    lane.timebase = beatTime.getToggleState()
        ? AutomationTimebase::beats
        : AutomationTimebase::seconds;
    lane.interpolation = stepInterpolation.getToggleState()
        ? AutomationInterpolation::step
        : AutomationInterpolation::linear;
    lane.points.push_back({
        juce::Uuid().toString(),
        lanePosition(lane),
        value.getValue()
    });
    if (onAddLane)
        onAddLane(std::move(lane));
    refresh();
    lanes.setSelectedItemIndex(lanes.getNumItems() - 1,
                               juce::sendNotificationSync);
}

void AutomationPanel::addPoint()
{
    auto* lane = selectedLane();
    if (lane == nullptr)
    {
        addLane();
        return;
    }
    auto before = *lane;
    auto after = before;
    after.points.push_back({
        juce::Uuid().toString(),
        lanePosition(after),
        value.getValue()
    });
    std::stable_sort(after.points.begin(),
                     after.points.end(),
                     [](const auto& left, const auto& right)
                     {
                         return left.position < right.position;
                     });
    if (onUpdateLane)
        onUpdateLane(std::move(before), std::move(after));
    refresh();
}

void AutomationPanel::removeNearestPoint()
{
    auto* lane = selectedLane();
    if (lane == nullptr || lane->points.empty())
        return;
    auto before = *lane;
    auto after = before;
    const auto position = lanePosition(after);
    const auto point = std::min_element(
        after.points.begin(),
        after.points.end(),
        [position](const auto& left, const auto& right)
        {
            return std::abs(left.position - position)
                < std::abs(right.position - position);
        });
    if (point != after.points.end())
        after.points.erase(point);
    if (onUpdateLane)
        onUpdateLane(std::move(before), std::move(after));
    refresh();
}

void AutomationPanel::removeLane()
{
    auto* lane = selectedLane();
    if (lane == nullptr)
        return;
    const auto id = lane->id;
    if (onRemoveLane)
        onRemoveLane(id);
    refresh();
}

void AutomationPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    graphics.drawText("AUTOMATION",
                      14,
                      10,
                      getWidth() - 28,
                      24,
                      juce::Justification::centredLeft);
}

void AutomationPanel::resized()
{
    auto bounds = getLocalBounds().reduced(14);
    bounds.removeFromTop(32);
    auto modeRow = bounds.removeFromTop(32);
    mode.setBounds(modeRow.removeFromLeft(180).reduced(2));
    armed.setBounds(modeRow.removeFromLeft(120).reduced(2));
    beatTime.setBounds(modeRow.removeFromLeft(110).reduced(2));
    stepInterpolation.setBounds(modeRow.removeFromLeft(90).reduced(2));
    bounds.removeFromTop(8);
    lanes.setBounds(bounds.removeFromTop(32).reduced(2));
    bounds.removeFromTop(8);
    target.setBounds(bounds.removeFromTop(32).reduced(2));
    bounds.removeFromTop(8);
    value.setBounds(bounds.removeFromTop(36).reduced(2));
    bounds.removeFromTop(10);
    auto buttons = bounds.removeFromTop(34);
    addLaneButton.setBounds(buttons.removeFromLeft(120).reduced(2));
    addPointButton.setBounds(buttons.removeFromLeft(120).reduced(2));
    removePointButton.setBounds(buttons.removeFromLeft(140).reduced(2));
    removeLaneButton.setBounds(buttons.removeFromLeft(130).reduced(2));
}
}
