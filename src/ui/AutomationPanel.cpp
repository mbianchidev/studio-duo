#include "AutomationPanel.h"

#include "StudioTheme.h"
#include "util/NumberParsing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace studio
{
AutomationPanel::AutomationPanel(double playbackSampleRate)
    : sampleRate(playbackSampleRate)
{
    mode.setTitle("Track automation mode");
    armed.setTitle("Arm track automation writing");
    lanes.setTitle("Automation lane");
    lanes.setComponentID("automation.lanes");
    target.setTitle("New automation lane target");
    points.setTitle("Automation point");
    points.setComponentID("automation.points");
    points.setDescription("Choose an existing point by position and value, then edit or remove it.");
    points.setTextWhenNothingSelected("No point selected");
    points.setTextWhenNoChoicesAvailable("No automation points");
    beatTime.setTitle("Use beat-based automation time");
    stepInterpolation.setTitle("Use step interpolation");
    value.setTitle("Automation point normalized value");
    value.setDescription("Normalized range 0 to 1. Use arrow keys or enter a value.");
    value.setWantsKeyboardFocus(true);
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
    value.setRange(0.0, 1.0);
    value.setValue(0.5);
    value.setSliderStyle(juce::Slider::LinearHorizontal);
    value.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pointPosition.setComponentID("automation.position");
    pointValue.setComponentID("automation.value");
    pointValue.setTitle("Exact automation point value");
    pointValue.setDescription("Enter a finite normalized value from 0 to 1. Return applies the selected point.");
    for (auto* editor : { &pointPosition, &pointValue })
    {
        editor->setSelectAllWhenFocused(true);
        editor->setInputRestrictions(64);
        editor->setEscapeAndReturnKeysConsumed(false);
    }
    value.onValueChange = [this]
    {
        if (!rebuilding)
            pointValue.setText(formatRoundTripNumber(value.getValue()), false);
    };
    pointValue.onTextChange = [this]
    {
        const auto number = parseFiniteNumber(pointValue.getText());
        if (!rebuilding && number && *number >= 0.0 && *number <= 1.0)
            value.setValue(*number, juce::dontSendNotification);
    };
    valueLabel.setText("Value (0 to 1)", juce::dontSendNotification);
    updatePointButton.setTitle("Apply selected automation point");
    updatePointButton.setComponentID("automation.apply");
    removeSelectedPointButton.setTitle("Remove selected automation point");
    removeSelectedPointButton.setComponentID("automation.remove-selected");
    status.setTitle("Automation edit validation");
    status.setComponentID("automation.status");
    for (auto* label : { &positionLabel, &valueLabel, &status })
    {
        label->setColour(juce::Label::textColourId, juce::Colour(StudioColours::text));
        label->setFont(juce::FontOptions(13.0f));
        label->setBorderSize({});
        addAndMakeVisible(label);
    }

    for (auto* component : std::array<juce::Component*, 17> {
             &mode,
             &armed,
             &beatTime,
             &stepInterpolation,
             &lanes,
             &target,
             &points,
             &pointPosition,
             &value,
             &pointValue,
             &updatePointButton,
             &removeSelectedPointButton,
             &addLaneButton,
             &addPointButton,
             &removePointButton,
             &removeLaneButton,
             &commitPreviewButton
         })
    {
        component->setWantsKeyboardFocus(true);
        addAndMakeVisible(component);
    }

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
        selectedPointId.clear();
        points.clear(juce::dontSendNotification);
        refreshPoints();
    };
    points.onChange = [this]
    {
        if (rebuilding)
            return;
        const auto index = points.getSelectedItemIndex();
        selectedPointId = index >= 0 && index < static_cast<int>(pointIds.size())
            ? pointIds[static_cast<std::size_t>(index)] : juce::String();
        refreshPointValues();
    };
    beatTime.onClick = [this]
    {
        if (rebuilding || project == nullptr)
            return;
        auto* lane = selectedLane();
        if (lane == nullptr)
            return;
        const auto nextTimebase = beatTime.getToggleState()
            ? AutomationTimebase::beats
            : AutomationTimebase::seconds;
        if (lane->timebase == nextTimebase)
            return;
        auto before = *lane;
        auto after = before;
        for (auto& point : after.points)
        {
            point.position =
                nextTimebase == AutomationTimebase::beats
                ? project->beatsAt(point.position)
                : project->secondsAtBeat(point.position);
        }
        after.timebase = nextTimebase;
        juce::ignoreUnused(updateLane(std::move(before), std::move(after)));
    };
    stepInterpolation.onClick = [this]
    {
        if (rebuilding)
            return;
        auto* lane = selectedLane();
        if (lane == nullptr)
            return;
        auto before = *lane;
        auto after = before;
        after.interpolation =
            stepInterpolation.getToggleState()
            ? AutomationInterpolation::step
            : AutomationInterpolation::linear;
        juce::ignoreUnused(updateLane(std::move(before), std::move(after)));
    };
    addLaneButton.onClick = [this] { addLane(); };
    addPointButton.onClick = [this] { addPoint(); };
    updatePointButton.onClick = [this] { updateSelectedPoint(); };
    removeSelectedPointButton.onClick = [this] { removeSelectedPoint(); };
    removePointButton.onClick = [this] { removeNearestPoint(); };
    removeLaneButton.onClick = [this] { removeLane(); };
    commitPreviewButton.onClick = [this]
    {
        if (onCommitPreview)
            onCommitPreview();
    };
    refresh();
    setSize(560, 520);
}

void AutomationPanel::setProject(Project* projectToUse)
{
    if (project != projectToUse)
    {
        lanes.clear(juce::dontSendNotification);
        selectedPointId.clear();
    }
    project = projectToUse;
    refresh();
}

void AutomationPanel::setTrack(const juce::String& trackToUse)
{
    if (trackId != trackToUse)
    {
        lanes.clear(juce::dontSendNotification);
        selectedPointId.clear();
    }
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
    const juce::ScopedValueSetter<bool> guard(rebuilding, true);
    const auto laneIndex = lanes.getSelectedItemIndex();
    const auto previousLaneId = laneIndex >= 0 && laneIndex < static_cast<int>(laneIds.size())
        ? laneIds[static_cast<std::size_t>(laneIndex)] : juce::String();
    const auto targetIndex = target.getSelectedItemIndex();
    const auto previousTarget = targetIndex >= 0 && targetIndex < static_cast<int>(targets.size())
        ? std::optional<AutomationTarget>(targets[static_cast<std::size_t>(targetIndex)].target) : std::nullopt;
    lanes.clear(juce::dontSendNotification);
    target.clear(juce::dontSendNotification);
    laneIds.clear();
    targets.clear();
    const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
    for (auto* component : std::array<juce::Component*, 6> {
             &mode, &armed, &beatTime, &stepInterpolation, &value, &commitPreviewButton })
        component->setEnabled(track != nullptr);
    lanes.setEnabled(false);
    target.setEnabled(false);
    addLaneButton.setEnabled(false);
    addPointButton.setEnabled(false);
    if (track == nullptr)
    {
        mode.setSelectedId(0, juce::dontSendNotification);
        armed.setToggleState(false, juce::dontSendNotification);
        refreshPoints();
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
    if (track->type == TrackType::vca)
    {
        addTarget("VCA volume", AutomationTargetType::vcaVolume);
    }
    else if (track->type != TrackType::folder
             && track->type != TrackType::midi)
    {
        addTarget("Track volume", AutomationTargetType::trackVolume);
        addTarget("Track pan", AutomationTargetType::trackPan);
        addTarget("Track mute", AutomationTargetType::trackMute);
        addTarget("Track polarity", AutomationTargetType::trackPolarity);
        if (track->type == TrackType::controlRoom)
        {
            addTarget(
                "Control room dim",
                AutomationTargetType::controlRoomDim);
        }
    }

    for (const auto& route : project->routingConnections)
    {
        if (route.sourceTrackId != trackId
            || route.kind == RouteKind::mainOutput
            || route.kind == RouteKind::controlRoom
            || route.signalType != SignalType::audio)
            continue;
        AutomationTarget gain;
        gain.type = AutomationTargetType::sendGain;
        gain.trackId = trackId;
        gain.routeId = route.id;
        targets.push_back({
            route.name + " level",
            gain
        });
        auto pan = gain;
        pan.type = AutomationTargetType::sendPan;
        targets.push_back({
            route.name + " pan",
            pan
        });
        auto mute = gain;
        mute.type = AutomationTargetType::sendMute;
        targets.push_back({
            route.name + " mute",
            mute
        });
    }
    for (const auto& runtimeStatus : runtimeStatuses)
    {
        if (runtimeStatus.trackId != trackId)
            continue;
        const auto insert = std::find_if(
            track->inserts.cbegin(),
            track->inserts.cend(),
            [&runtimeStatus](const auto& candidate)
            {
                return candidate.id == runtimeStatus.insertId;
            });
        const auto targetType =
            insert != track->inserts.cend()
                && insert->bundledDevice
            ? AutomationTargetType::deviceParameter
            : AutomationTargetType::pluginParameter;
        for (const auto& parameter : runtimeStatus.parameters)
        {
            if (!parameter.automatable)
                continue;
            AutomationTarget pluginTarget;
            pluginTarget.type = targetType;
            pluginTarget.trackId = trackId;
            pluginTarget.insertId = runtimeStatus.insertId;
            pluginTarget.parameterId = parameter.id;
            pluginTarget.parameterIndex = parameter.index;
            targets.push_back({
                runtimeStatus.name + " / " + parameter.name,
                std::move(pluginTarget)
            });
        }
    }
    auto nextTargetIndex = 0;
    for (std::size_t index = 0; index < targets.size(); ++index)
    {
        target.addItem(targets[index].label, static_cast<int>(index + 1));
        if (previousTarget && targets[index].target == *previousTarget)
            nextTargetIndex = static_cast<int>(index);
    }
    if (!targets.empty())
        target.setSelectedItemIndex(nextTargetIndex, juce::dontSendNotification);
    target.setEnabled(!targets.empty());
    addLaneButton.setEnabled(!targets.empty());

    for (const auto& lane : project->automationLanes)
    {
        if (lane.target.trackId != trackId)
            continue;
        laneIds.push_back(lane.id);
        lanes.addItem(lane.name, lanes.getNumItems() + 1);
    }
    if (!laneIds.empty())
    {
        const auto previous = std::find(laneIds.cbegin(), laneIds.cend(), previousLaneId);
        const auto nextIndex = previous != laneIds.cend()
            ? static_cast<int>(previous - laneIds.cbegin()) : 0;
        lanes.setSelectedItemIndex(nextIndex, juce::dontSendNotification);
        if (laneIds[static_cast<std::size_t>(nextIndex)] != previousLaneId)
        {
            selectedPointId.clear();
            points.clear(juce::dontSendNotification);
        }
    }
    lanes.setEnabled(!laneIds.empty());
    addPointButton.setEnabled(!laneIds.empty() || !targets.empty());
    value.setEnabled(addPointButton.isEnabled());
    refreshPoints();
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

const AutomationPoint* AutomationPanel::selectedPoint() const
{
    const auto* lane = selectedLane();
    if (lane == nullptr)
        return nullptr;
    const auto point = std::find_if(lane->points.cbegin(), lane->points.cend(),
        [this](const auto& candidate) { return candidate.id == selectedPointId; });
    return point != lane->points.cend() ? &*point : nullptr;
}

void AutomationPanel::refreshPoints()
{
    const juce::ScopedValueSetter<bool> guard(rebuilding, true);
    const auto previousIndex = points.getSelectedItemIndex();
    points.clear(juce::dontSendNotification);
    pointIds.clear();
    const auto* lane = selectedLane();
    if (lane != nullptr)
    {
        beatTime.setToggleState(lane->timebase == AutomationTimebase::beats, juce::dontSendNotification);
        stepInterpolation.setToggleState(lane->interpolation == AutomationInterpolation::step, juce::dontSendNotification);
        std::vector<const AutomationPoint*> ordered;
        for (const auto& point : lane->points)
            ordered.push_back(&point);
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const auto* left, const auto* right) { return left->position < right->position; });
        for (const auto* point : ordered)
        {
            pointIds.push_back(point->id);
            points.addItem(juce::String(static_cast<int>(pointIds.size())) + ": "
                + formatRoundTripNumber(point->position) + " " + automationTimebaseToString(lane->timebase)
                + ", value " + formatRoundTripNumber(point->value), static_cast<int>(pointIds.size()));
        }
    }
    if (!pointIds.empty())
    {
        const auto selected = std::find(pointIds.cbegin(), pointIds.cend(), selectedPointId);
        const auto index = selected != pointIds.cend() ? static_cast<int>(selected - pointIds.cbegin())
            : juce::jlimit(0, static_cast<int>(pointIds.size()) - 1, previousIndex);
        points.setSelectedItemIndex(index, juce::dontSendNotification);
        selectedPointId = pointIds[static_cast<std::size_t>(index)];
    }
    else
    {
        selectedPointId.clear();
    }
    points.setEnabled(!pointIds.empty());
    removePointButton.setEnabled(!pointIds.empty());
    removeLaneButton.setEnabled(lane != nullptr);
    refreshPointValues();
}

void AutomationPanel::refreshPointValues()
{
    const juce::ScopedValueSetter<bool> guard(rebuilding, true);
    const auto* lane = selectedLane();
    const auto* point = selectedPoint();
    const auto enableEditor = [](juce::TextEditor& editor, bool enabled)
    {
        if (editor.isEnabled() != enabled)
        {
            editor.setEnabled(enabled);
            // JUCE caches the text role using isReadOnly(), which includes disabled state.
            editor.invalidateAccessibilityHandler();
        }
    };
    enableEditor(pointPosition, point != nullptr);
    enableEditor(pointValue, value.isEnabled());
    const auto units = lane != nullptr ? automationTimebaseToString(lane->timebase)
        : beatTime.getToggleState() ? juce::String("beats") : juce::String("seconds");
    positionLabel.setText("Position (" + units + ")", juce::dontSendNotification);
    pointPosition.setTitle("Automation point position (" + units + ")");
    pointPosition.setDescription("Enter a non-negative finite position in " + units + ". Return applies the selected point.");
    pointPosition.setText(point != nullptr ? formatRoundTripNumber(point->position) : juce::String(), false);
    pointValue.setText(formatRoundTripNumber(point != nullptr ? point->value : 0.5), false);
    value.setValue(point != nullptr ? point->value : 0.5, juce::dontSendNotification);
    updatePointButton.setEnabled(point != nullptr);
    removeSelectedPointButton.setEnabled(point != nullptr);
    status.setText({}, juce::dontSendNotification);
    if (auto* handler = points.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

juce::Result AutomationPanel::showError(juce::String message, juce::Component& field)
{
    status.setText("Error: " + message, juce::dontSendNotification);
    if (auto* handler = status.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    if (field.isShowing())
        field.grabKeyboardFocus();
    return juce::Result::fail(std::move(message));
}

std::optional<double> AutomationPanel::editedValue()
{
    const auto number = parseFiniteNumber(pointValue.getText());
    if (!number || *number < 0.0 || *number > 1.0)
    {
        juce::ignoreUnused(showError("Enter a finite normalized value from 0 to 1.", pointValue));
        return std::nullopt;
    }
    return number;
}

juce::Result AutomationPanel::updateLane(AutomationLane before, AutomationLane after, juce::String pointToSelect)
{
    if (!onUpdateLane)
        return showError("Automation editing is unavailable. Close and reopen this editor.", points);
    const auto previousPointId = selectedPointId;
    if (pointToSelect.isNotEmpty())
        selectedPointId = std::move(pointToSelect);
    const auto action = onUpdateLane;
    const juce::Component::SafePointer<AutomationPanel> safe(this);
    const auto result = action(std::move(before), std::move(after));
    if (safe == nullptr)
        return result;
    if (result.failed())
    {
        selectedPointId = previousPointId;
        if (const auto* lane = selectedLane())
        {
            beatTime.setToggleState(lane->timebase == AutomationTimebase::beats, juce::dontSendNotification);
            stepInterpolation.setToggleState(lane->interpolation == AutomationInterpolation::step, juce::dontSendNotification);
        }
        return showError(result.getErrorMessage(), points);
    }
    refresh();
    return result;
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
    const auto number = editedValue();
    if (!number)
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
        *number
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
    const auto number = editedValue();
    if (!number)
        return;
    auto before = *lane;
    auto after = before;
    const auto pointId = juce::Uuid().toString();
    after.points.push_back({
        pointId,
        lanePosition(after),
        *number
    });
    std::stable_sort(after.points.begin(),
                     after.points.end(),
                     [](const auto& left, const auto& right)
                     {
                         return left.position < right.position;
                     });
    juce::ignoreUnused(updateLane(std::move(before), std::move(after), pointId));
}

void AutomationPanel::updateSelectedPoint()
{
    const auto* lane = selectedLane();
    const auto* point = selectedPoint();
    if (lane == nullptr || point == nullptr)
    {
        juce::ignoreUnused(showError("Select an existing automation point.", points));
        return;
    }
    const auto position = parseFiniteNumber(pointPosition.getText());
    if (!position || *position < 0.0)
    {
        juce::ignoreUnused(showError("Enter a complete, non-negative finite position.", pointPosition));
        return;
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        juce::ignoreUnused(showError("Automation editing requires a positive finite audio sample rate.", pointPosition));
        return;
    }
    const auto seconds = lane->timebase == AutomationTimebase::beats
        ? project->secondsAtBeat(*position) : *position;
    const auto samplePosition = seconds * sampleRate;
    if (!std::isfinite(samplePosition) || samplePosition < 0.0
        || samplePosition >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
    {
        juce::ignoreUnused(showError("Position must fit the signed audio sample range at the current sample rate.", pointPosition));
        return;
    }
    const auto number = editedValue();
    if (!number)
        return;
    if (juce::exactlyEqual(point->position, *position) && juce::exactlyEqual(point->value, *number))
    {
        status.setText({}, juce::dontSendNotification);
        return;
    }
    auto before = *lane;
    auto after = before;
    const auto index = static_cast<std::size_t>(point - lane->points.data());
    after.points[index].position = *position;
    after.points[index].value = *number;
    std::stable_sort(after.points.begin(), after.points.end(),
        [](const auto& left, const auto& right) { return left.position < right.position; });
    juce::ignoreUnused(updateLane(std::move(before), std::move(after)));
}

void AutomationPanel::removeSelectedPoint()
{
    const auto* lane = selectedLane();
    const auto* point = selectedPoint();
    if (lane == nullptr || point == nullptr)
    {
        juce::ignoreUnused(showError("Select an existing automation point.", points));
        return;
    }
    auto before = *lane;
    auto after = before;
    const auto id = point->id;
    std::erase_if(after.points, [&id](const auto& candidate) { return candidate.id == id; });
    juce::ignoreUnused(updateLane(std::move(before), std::move(after)));
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
    juce::ignoreUnused(updateLane(std::move(before), std::move(after)));
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

bool AutomationPanel::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isAnyModifierKeyDown()
        || key.getKeyCode() != juce::KeyPress::returnKey || selectedPoint() == nullptr)
        return false;
    updateSelectedPoint();
    return true;
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
    points.setBounds(bounds.removeFromTop(32).reduced(2));
    bounds.removeFromTop(8);
    positionLabel.setBounds(bounds.removeFromTop(20).reduced(2, 0));
    pointPosition.setBounds(bounds.removeFromTop(32).reduced(2));
    bounds.removeFromTop(8);
    valueLabel.setBounds(bounds.removeFromTop(20).reduced(2, 0));
    auto valueRow = bounds.removeFromTop(36);
    pointValue.setBounds(valueRow.removeFromRight(190).reduced(2));
    value.setBounds(valueRow.reduced(2));
    bounds.removeFromTop(8);
    auto pointButtons = bounds.removeFromTop(34);
    updatePointButton.setBounds(pointButtons.removeFromLeft(150).reduced(2));
    removeSelectedPointButton.setBounds(pointButtons.removeFromLeft(180).reduced(2));
    status.setBounds(bounds.removeFromTop(42).reduced(2));
    bounds.removeFromTop(10);
    auto buttons = bounds.removeFromTop(34);
    addLaneButton.setBounds(buttons.removeFromLeft(120).reduced(2));
    addPointButton.setBounds(buttons.removeFromLeft(120).reduced(2));
    removePointButton.setBounds(buttons.removeFromLeft(140).reduced(2));
    removeLaneButton.setBounds(buttons.removeFromLeft(130).reduced(2));
    bounds.removeFromTop(6);
    commitPreviewButton.setBounds(
        bounds.removeFromTop(34).removeFromLeft(180).reduced(2));
}
}
