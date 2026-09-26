#include "MidiEditorComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace studio
{
namespace
{
bool containsId(const std::vector<juce::String>& ids,
                const juce::String& id)
{
    return std::find(ids.cbegin(), ids.cend(), id) != ids.cend();
}

juce::String laneName(MidiEditorLane lane)
{
    switch (lane)
    {
        case MidiEditorLane::velocity: return "Velocity";
        case MidiEditorLane::timing: return "Timing";
        case MidiEditorLane::duration: return "Duration";
        case MidiEditorLane::probability: return "Probability";
        case MidiEditorLane::expression: return "Expression";
    }
    return "Velocity";
}
}

MidiEditorComponent::MidiEditorComponent()
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::focusContainer);

    const auto addButton = [this](juce::Button& button,
                                  const juce::String& tooltip)
    {
        addAndMakeVisible(button);
        button.setTooltip(tooltip);
    };
    addButton(modeButton, "Switch between piano roll and metal drum editor");
    addButton(padsButton, "Open visual drum pads and keyboard performance without replacing the MIDI grid");
    addButton(captureButton, "Capture recent MIDI input (Command/Ctrl+Shift+M)");
    addButton(humanizeButton, "Apply deterministic seeded timing and velocity humanization");
    addButton(importMapButton, "Import a Studio Duo drum-map JSON file");
    addButton(editMapButton, "Edit the selected drum-map row");
    addButton(closeButton, "Close the MIDI lower editor");
    addButton(flamButton, "Insert deterministic flam notes");
    addButton(rollButton, "Insert an even roll");
    addButton(gravityButton, "Insert a gravity-blast pattern");
    addButton(blastButton, "Insert a blast-beat pattern");
    addButton(doubleKickButton, "Insert alternating double-kick notes");
    addButton(expandPatternButton, "Expand the selected alias into ordinary notes");
    addButton(applyRoutingButton, "Create and route the selected multi-output MIDI template");

    for (auto* box : {
             &laneSelector,
             &expressionSelector,
             &gridSelector,
             &patternSelector,
             &routingTemplateSelector })
        addAndMakeVisible(*box);

    laneSelector.addItem("Velocity", 1);
    laneSelector.addItem("Timing", 2);
    laneSelector.addItem("Duration", 3);
    laneSelector.addItem("Probability", 4);
    laneSelector.addItem("Expression", 5);
    laneSelector.setSelectedId(1, juce::dontSendNotification);
    laneSelector.setTooltip(
        "Select the lower note-property lane; Alt+Up/Down edits selected notes");
    laneSelector.onChange = [this]
    {
        expressionSelector.setVisible(
            selectedLane() == MidiEditorLane::expression);
        resized();
        repaint();
    };

    expressionSelector.addItem("Poly pressure", 1);
    expressionSelector.addItem("Timbre", 2);
    expressionSelector.addItem("Pitch", 3);
    expressionSelector.addItem("Expression CC", 4);
    expressionSelector.addItem("Channel pressure", 5);
    expressionSelector.setSelectedId(1, juce::dontSendNotification);
    expressionSelector.setTooltip("Select per-note expression data");

    gridSelector.addItem("1/4", 1);
    gridSelector.addItem("1/8", 2);
    gridSelector.addItem("1/16", 3);
    gridSelector.addItem("1/32", 4);
    gridSelector.addItem("1/16T", 5);
    gridSelector.setSelectedId(3, juce::dontSendNotification);
    gridSelector.setTooltip("Set note-entry and keyboard nudge grid");

    patternSelector.setTooltip("Select a saved pattern alias");
    routingTemplateSelector.setTooltip("Select a multi-output MIDI routing template");

    modeButton.onClick = [this]
    {
        const auto* clip = currentClip();
        if (clip == nullptr)
            return;
        showingPads = false;
        changeEditorMode(clip->editorMode == MidiEditorMode::drums
            ? MidiEditorMode::pianoRoll
            : MidiEditorMode::drums);
        refreshControls();
    };
    padsButton.onClick = [this] { showDrumPads(!showingPads); };
    addChildComponent(drumPerformancePanel);
    drumPerformancePanel.onBindingsEdited = [this](const auto& bindings)
    {
        const auto* clip = currentClip();
        if (clip == nullptr)
            return false;
        auto after = *clip;
        after.drumPadBindings = bindings;
        commitEdit(*clip, std::move(after), "Change drum-pad assignments");
        const auto* updated = currentClip();
        return updated != nullptr && updated->drumPadBindings == bindings;
    };
    captureButton.onClick = [this]
    {
        if (onCaptureRetrospective)
            onCaptureRetrospective();
    };
    humanizeButton.onClick = [this]
    {
        if (onHumanize)
            onHumanize();
    };
    importMapButton.onClick = [this]
    {
        if (onImportDrumMap)
            onImportDrumMap();
    };
    editMapButton.onClick = [this]
    {
        if (onEditDrumMapEntry)
            onEditDrumMapEntry(cursorPitch);
    };
    closeButton.onClick = [this]
    {
        if (onClose)
            onClose();
    };
    flamButton.onClick = [this] { runEntryTool(MidiEntryTool::flam); };
    rollButton.onClick = [this] { runEntryTool(MidiEntryTool::roll); };
    gravityButton.onClick = [this]
    {
        runEntryTool(MidiEntryTool::gravityBlast);
    };
    blastButton.onClick = [this]
    {
        runEntryTool(MidiEntryTool::blastBeat);
    };
    doubleKickButton.onClick = [this]
    {
        runEntryTool(MidiEntryTool::doubleKick);
    };
    expandPatternButton.onClick = [this] { expandSelectedPattern(); };
    applyRoutingButton.onClick = [this]
    {
        if (onApplyRoutingTemplate
            && routingTemplateSelector.getSelectedId() > 0)
        {
            const auto index =
                routingTemplateSelector.getSelectedItemIndex();
            if (project != nullptr
                && index >= 0
                && index < static_cast<int>(
                    project->midiRoutingTemplates.size()))
            {
                onApplyRoutingTemplate(
                    project->midiRoutingTemplates[
                        static_cast<std::size_t>(index)]
                        .id);
            }
        }
    };
    expressionSelector.setVisible(false);
}

void MidiEditorComponent::setProject(const Project* projectToEdit)
{
    project = projectToEdit;
    refreshControls();
    repaint();
}

void MidiEditorComponent::setSelection(juce::String trackId,
                                       juce::String clipId)
{
    if (trackId != selectedTrackId)
        showingPads = false;
    const auto changedClip = clipId != selectedClipId;
    if (changedClip)
    {
        selectedNoteIds.clear();
        dragBefore.reset();
        dragPreview.reset();
        cursorBeat = 0.0;
        rowOffset = 0;
    }
    selectedTrackId = std::move(trackId);
    selectedClipId = std::move(clipId);
    if (const auto* clip = currentClip();
        clip == nullptr || clip->editorMode != MidiEditorMode::drums)
        showingPads = false;
    if (const auto* clip = currentClip(); clip != nullptr && !clip->notes.empty())
    {
        cursorPitch = clip->notes.front().pitch;
        cursorBeat = clip->notes.front().startBeats;
        if (changedClip)
        {
            if (clip->editorMode == MidiEditorMode::drums)
            {
                const auto rows = drumRows();
                const auto row = std::find_if(
                    rows.cbegin(),
                    rows.cend(),
                    [this](const auto* entry)
                    {
                        return entry != nullptr
                            && (entry->noteNumber == cursorPitch
                                || std::find(
                                       entry->roundRobinNotes.cbegin(),
                                       entry->roundRobinNotes.cend(),
                                       cursorPitch)
                                    != entry->roundRobinNotes.cend());
                    });
                if (row != rows.cend())
                    rowOffset = std::max(
                        0,
                        static_cast<int>(
                            std::distance(rows.cbegin(), row))
                            - 4);
            }
            else
            {
                rowOffset = juce::jlimit(
                    0,
                    128 - pianoRows,
                    83 - cursorPitch - pianoRows / 2);
            }
        }
    }
    refreshControls();
    repaint();
}

void MidiEditorComponent::changeEditorMode(MidiEditorMode mode)
{
    const auto* clip = currentClip();
    if (clip == nullptr)
        return;
    auto after = *clip;
    after.editorMode = mode;
    if (mode == MidiEditorMode::drums && after.drumMapId.isEmpty()
        && project != nullptr && !project->drumMaps.empty())
        after.drumMapId = project->drumMaps.front().id;
    if (mode == MidiEditorMode::drums)
    {
        const auto* map = project != nullptr ? project->findDrumMap(after.drumMapId) : nullptr;
        for (auto& note : after.notes)
            applyDrumMapMetadata(note, map);
    }
    commitEdit(*clip, std::move(after), "Change MIDI editor mode");
}

void MidiEditorComponent::showDrumPads(bool show)
{
    const auto* clip = currentClip();
    showingPads = show && clip != nullptr;
    if (showingPads
        && (clip->editorMode != MidiEditorMode::drums || clip->drumMapId.isEmpty()))
        changeEditorMode(MidiEditorMode::drums);
    refreshControls();
    repaint();
}

bool MidiEditorComponent::isShowingDrumPads() const noexcept
{
    return showingPads;
}

DrumPerformanceComponent& MidiEditorComponent::drumPerformance() noexcept
{
    return drumPerformancePanel;
}

void MidiEditorComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, 0, getWidth(), toolbarHeight);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawHorizontalLine(
        toolbarHeight - 1,
        0.0f,
        static_cast<float>(getWidth()));
    if (showingPads)
        return;

    const auto* clipPointer = currentClip();
    if (clipPointer == nullptr)
    {
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(14.0f);
        graphics.drawText(
            "Select a MIDI clip to edit.",
            getLocalBounds().withTrimmedTop(toolbarHeight),
            juce::Justification::centred);
        return;
    }
    const auto& clip = displayedClip();
    const auto notes = noteArea();
    const auto lane = laneArea();
    const auto labels = notes.withWidth(labelWidth());
    const auto grid = notes.withTrimmedLeft(labelWidth());

    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(labels);
    graphics.setColour(juce::Colour(StudioColours::raised));
    graphics.fillRect(grid);
    graphics.setColour(juce::Colour(StudioColours::window));
    graphics.fillRect(lane);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawRect(notes);
    graphics.drawRect(lane);
    graphics.drawVerticalLine(
        notes.getX() + labelWidth(),
        static_cast<float>(notes.getY()),
        static_cast<float>(notes.getBottom()));

    const auto step = gridBeats();
    for (auto beat = 0.0;
         beat <= clip.durationBeats + 0.0000001;
         beat += step)
    {
        const auto x = xForBeat(beat);
        const auto major = std::abs(std::fmod(beat, 1.0)) < 0.000001;
        graphics.setColour(
            juce::Colour(
                major ? StudioColours::border : 0xff2a2e32)
                .withAlpha(major ? 0.9f : 0.55f));
        graphics.drawVerticalLine(
            static_cast<int>(std::round(x)),
            static_cast<float>(notes.getY()),
            static_cast<float>(lane.getBottom()));
        if (major)
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(9.0f);
            graphics.drawText(
                juce::String(static_cast<int>(beat + 1.0)),
                static_cast<int>(x) + 3,
                notes.getY() + 1,
                32,
                12,
                juce::Justification::centredLeft);
        }
    }

    if (clip.editorMode == MidiEditorMode::drums)
    {
        const auto rows = drumRows();
        const auto rowHeight = std::max(
            minimumDrumRowHeight,
            static_cast<float>(notes.getHeight())
                / static_cast<float>(std::max(1, pianoRows)));
        const auto visibleCount = static_cast<int>(
            std::ceil(static_cast<float>(notes.getHeight()) / rowHeight));
        for (int visible = 0; visible < visibleCount; ++visible)
        {
            const auto index = rowOffset + visible;
            if (index < 0 || index >= static_cast<int>(rows.size()))
                break;
            const auto* entry = rows[static_cast<std::size_t>(index)];
            const auto y = static_cast<float>(notes.getY())
                + static_cast<float>(visible) * rowHeight;
            graphics.setColour(
                juce::Colour(index % 2 == 0 ? 0xff171a1d : 0xff14171a));
            graphics.fillRect(
                static_cast<float>(notes.getX()),
                y,
                static_cast<float>(notes.getWidth()),
                rowHeight);
            graphics.setColour(juce::Colour(StudioColours::border));
            graphics.drawHorizontalLine(
                static_cast<int>(std::round(y + rowHeight)),
                static_cast<float>(notes.getX()),
                static_cast<float>(notes.getRight()));
            graphics.setColour(
                entry->noteNumber == cursorPitch
                    ? juce::Colour(StudioColours::orange)
                    : juce::Colour(StudioColours::text));
            graphics.setFont(10.0f);
            auto label = entry->name + " · " + entry->articulation;
            if (entry->cymbalState != CymbalState::none)
                label << " · " << cymbalStateToString(entry->cymbalState);
            if (entry->footControlCC >= 0)
                label << " · CC" << juce::String(entry->footControlCC);
            if (!entry->roundRobinNotes.empty())
                label << " · RR" << juce::String(
                    static_cast<int>(entry->roundRobinNotes.size() + 1));
            graphics.drawText(
                label,
                notes.getX() + 5,
                static_cast<int>(y),
                labelWidth() - 8,
                static_cast<int>(rowHeight),
                juce::Justification::centredLeft,
                true);
        }
    }
    else
    {
        const auto rowHeight = static_cast<float>(notes.getHeight())
            / static_cast<float>(pianoRows);
        for (int row = 0; row < pianoRows; ++row)
        {
            const auto pitch = juce::jlimit(
                0,
                127,
                83 - rowOffset - row);
            const auto y = static_cast<float>(notes.getY())
                + static_cast<float>(row) * rowHeight;
            const auto pitchClass = pitch % 12;
            const auto black = pitchClass == 1
                || pitchClass == 3
                || pitchClass == 6
                || pitchClass == 8
                || pitchClass == 10;
            graphics.setColour(
                juce::Colour(black ? 0xff121518 : 0xff1b1f23));
            graphics.fillRect(
                static_cast<float>(notes.getX()),
                y,
                static_cast<float>(notes.getWidth()),
                rowHeight);
            graphics.setColour(juce::Colour(StudioColours::border));
            graphics.drawHorizontalLine(
                static_cast<int>(std::round(y + rowHeight)),
                static_cast<float>(notes.getX()),
                static_cast<float>(notes.getRight()));
            if (pitch % 12 == 0 || pitch == cursorPitch)
            {
                graphics.setColour(
                    pitch == cursorPitch
                        ? juce::Colour(StudioColours::orange)
                        : juce::Colour(StudioColours::secondaryText));
                graphics.setFont(9.5f);
                graphics.drawText(
                    juce::MidiMessage::getMidiNoteName(
                        pitch,
                        true,
                        true,
                        3),
                    notes.getX() + 5,
                    static_cast<int>(y),
                    labelWidth() - 8,
                    static_cast<int>(rowHeight),
                    juce::Justification::centredLeft);
            }
        }
    }

    for (const auto& note : clip.notes)
    {
        auto bounds = noteBounds(note);
        if (!bounds.intersects(notes.toFloat()))
            continue;
        const auto selected = containsId(selectedNoteIds, note.id);
        const auto colour = note.drumMapEntryId.isNotEmpty()
            ? juce::Colour(StudioColours::orange)
            : juce::Colour(StudioColours::violet);
        graphics.setColour(colour.withAlpha(
            static_cast<float>(0.45 + note.probability * 0.5)));
        graphics.fillRoundedRectangle(bounds, 2.5f);
        graphics.setColour(
            selected ? juce::Colours::white : colour.brighter(0.25f));
        graphics.drawRoundedRectangle(
            bounds,
            2.5f,
            selected ? 2.0f : 1.0f);
        if (note.roundRobinHint >= 0 && bounds.getWidth() > 24.0f)
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.8f));
            graphics.setFont(8.0f);
            graphics.drawText(
                "RR" + juce::String(note.roundRobinHint + 1),
                bounds.toNearestInt().reduced(3, 0),
                juce::Justification::centredRight);
        }
    }

    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
    graphics.drawText(
        laneName(selectedLane()).toUpperCase(),
        lane.withWidth(labelWidth()).reduced(6, 2),
        juce::Justification::topLeft);
    const auto laneBottom = static_cast<float>(lane.getBottom() - 5);
    const auto laneTop = static_cast<float>(lane.getY() + 18);
    for (const auto& note : clip.notes)
    {
        const auto x = xForBeat(note.actualStartBeats());
        if (x < static_cast<float>(lane.getX() + labelWidth())
            || x > static_cast<float>(lane.getRight()))
            continue;
        const auto selected = containsId(selectedNoteIds, note.id);
        graphics.setColour(
            selected
                ? juce::Colour(StudioColours::orange)
                : juce::Colour(StudioColours::violet).withAlpha(0.7f));
        switch (selectedLane())
        {
            case MidiEditorLane::velocity:
            {
                const auto height = static_cast<float>(note.velocity)
                    / 127.0f
                    * (laneBottom - laneTop);
                graphics.fillRect(x - 2.0f, laneBottom - height, 5.0f, height);
                break;
            }
            case MidiEditorLane::timing:
            {
                const auto centre = (laneTop + laneBottom) * 0.5f;
                const auto y = centre
                    - static_cast<float>(note.timingOffsetBeats / 0.125)
                        * (laneBottom - laneTop) * 0.5f;
                graphics.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
                break;
            }
            case MidiEditorLane::duration:
            {
                const auto normalized = static_cast<float>(
                    note.durationBeats / clip.durationBeats);
                const auto height = normalized * (laneBottom - laneTop);
                graphics.fillRect(x - 2.0f, laneBottom - height, 5.0f, height);
                break;
            }
            case MidiEditorLane::probability:
            {
                const auto height = static_cast<float>(note.probability)
                    * (laneBottom - laneTop);
                graphics.fillRect(x - 2.0f, laneBottom - height, 5.0f, height);
                break;
            }
            case MidiEditorLane::expression:
            {
                for (const auto& expression : note.expressions)
                {
                    if (expression.type != selectedExpressionType())
                        continue;
                    const auto expressionX = xForBeat(
                        note.actualStartBeats()
                        + expression.offsetBeats);
                    const auto normalized =
                        expression.type == MidiExpressionType::pitchBend
                        ? (expression.value + 1.0) * 0.5
                        : expression.value;
                    const auto y = laneBottom
                        - static_cast<float>(normalized)
                            * (laneBottom - laneTop);
                    graphics.fillEllipse(
                        expressionX - 3.0f,
                        y - 3.0f,
                        6.0f,
                        6.0f);
                }
                break;
            }
        }
    }

    const auto cursorX = xForBeat(cursorBeat);
    graphics.setColour(juce::Colour(StudioColours::green).withAlpha(0.8f));
    graphics.drawVerticalLine(
        static_cast<int>(std::round(cursorX)),
        static_cast<float>(notes.getY()),
        static_cast<float>(lane.getBottom()));

    if (hasKeyboardFocus(true))
    {
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.drawRect(getLocalBounds().reduced(2), 2);
    }
}

void MidiEditorComponent::resized()
{
    auto toolbar = getLocalBounds().removeFromTop(toolbarHeight).reduced(6, 4);
    auto first = toolbar.removeFromTop(28);
    toolbar.removeFromTop(4);
    auto second = toolbar.removeFromTop(28);
    closeButton.setBounds(first.removeFromRight(36).reduced(2));
    modeButton.setBounds(first.removeFromLeft(72).reduced(2));
    padsButton.setBounds(first.removeFromLeft(94).reduced(2));
    if (showingPads)
    {
        captureButton.setBounds(first.removeFromLeft(78).reduced(2));
        importMapButton.setBounds(first.removeFromLeft(36).reduced(2));
        drumPerformancePanel.setBounds(getLocalBounds().withTrimmedTop(36));
        return;
    }
    laneSelector.setBounds(first.removeFromLeft(112).reduced(2));
    expressionSelector.setBounds(first.removeFromLeft(
        expressionSelector.isVisible() ? 104 : 0).reduced(2));
    gridSelector.setBounds(first.removeFromLeft(76).reduced(2));
    captureButton.setBounds(first.removeFromLeft(78).reduced(2));
    humanizeButton.setBounds(first.removeFromLeft(92).reduced(2));
    importMapButton.setBounds(first.removeFromLeft(36).reduced(2));
    editMapButton.setBounds(first.removeFromLeft(36).reduced(2));

    flamButton.setBounds(second.removeFromLeft(66).reduced(2));
    rollButton.setBounds(second.removeFromLeft(66).reduced(2));
    gravityButton.setBounds(second.removeFromLeft(78).reduced(2));
    blastButton.setBounds(second.removeFromLeft(66).reduced(2));
    doubleKickButton.setBounds(second.removeFromLeft(100).reduced(2));
    patternSelector.setBounds(second.removeFromLeft(142).reduced(2));
    expandPatternButton.setBounds(second.removeFromLeft(36).reduced(2));
    routingTemplateSelector.setBounds(second.removeFromLeft(154).reduced(2));
    applyRoutingButton.setBounds(second.removeFromLeft(112).reduced(2));
}

void MidiEditorComponent::mouseDown(const juce::MouseEvent& event)
{
    if (currentClip() == nullptr)
        return;
    grabKeyboardFocus();
    if (laneArea().contains(event.getPosition()))
    {
        dragMode = DragMode::lane;
        dragBefore = *currentClip();
        dragPreview = *dragBefore;
        applyLaneAt(event.position);
        repaint();
        return;
    }
    if (!noteArea().contains(event.getPosition()))
        return;

    cursorBeat = juce::jlimit(
        0.0,
        currentClip()->durationBeats,
        std::round(beatForX(event.position.x) / gridBeats())
            * gridBeats());
    cursorPitch = pitchForY(event.position.y);
    if (const auto* note = noteAt(event.position))
    {
        if (event.mods.isCommandDown())
        {
            if (containsId(selectedNoteIds, note->id))
            {
                selectedNoteIds.erase(
                    std::remove(
                        selectedNoteIds.begin(),
                        selectedNoteIds.end(),
                        note->id),
                    selectedNoteIds.end());
            }
            else
            {
                selectedNoteIds.push_back(note->id);
            }
        }
        else if (!containsId(selectedNoteIds, note->id))
        {
            selectOnly(note->id);
        }
        dragBefore = *currentClip();
        dragPreview = *dragBefore;
        dragStart = event.position;
        dragStartBeat = note->actualStartBeats();
        dragStartPitch = note->pitch;
        dragMode = std::abs(
                       event.position.x - noteBounds(*note).getRight())
                <= 7.0f
            ? DragMode::resize
            : DragMode::move;
    }
    else
    {
        addNoteAt(cursorBeat, cursorPitch);
    }
    repaint();
}

void MidiEditorComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (!dragBefore.has_value() || !dragPreview.has_value())
        return;
    if (dragMode == DragMode::lane)
    {
        applyLaneAt(event.position);
        repaint();
        return;
    }
    *dragPreview = *dragBefore;
    const auto deltaBeats = std::round(
        (beatForX(event.position.x) - beatForX(dragStart.x))
        / gridBeats())
        * gridBeats();
    if (dragMode == DragMode::resize)
    {
        juce::ignoreUnused(resizeMidiNotes(
            *dragPreview,
            selectedNoteIds,
            deltaBeats,
            gridBeats() * 0.25));
    }
    else if (dragMode == DragMode::move)
    {
        const auto pitch = pitchForY(event.position.y);
        juce::ignoreUnused(moveMidiNotes(
            *dragPreview,
            selectedNoteIds,
            deltaBeats,
            pitch - dragStartPitch,
            currentDrumMap()));
        cursorBeat = juce::jlimit(
            0.0,
            dragPreview->durationBeats,
            dragStartBeat + deltaBeats);
        cursorPitch = juce::jlimit(
            0,
            127,
            dragStartPitch + pitch - dragStartPitch);
    }
    repaint();
}

void MidiEditorComponent::mouseUp(const juce::MouseEvent&)
{
    if (dragBefore.has_value() && dragPreview.has_value())
    {
        const auto commandName = dragMode == DragMode::resize
            ? "Resize MIDI notes"
            : dragMode == DragMode::lane
                ? "Edit MIDI note lane"
                : "Move MIDI notes";
        if (juce::JSON::toString(dragBefore->toVar(), false)
            != juce::JSON::toString(dragPreview->toVar(), false))
            commitEdit(*dragBefore, *dragPreview, commandName);
    }
    dragBefore.reset();
    dragPreview.reset();
    dragMode = DragMode::none;
    repaint();
}

void MidiEditorComponent::mouseWheelMove(
    const juce::MouseEvent& event,
    const juce::MouseWheelDetails& wheel)
{
    if (noteArea().contains(event.getPosition())
        && std::abs(wheel.deltaY) > 0.0001f)
    {
        const auto direction = wheel.deltaY > 0.0f ? -1 : 1;
        rowOffset = juce::jlimit(
            0,
            maximumRowOffset(),
            rowOffset + direction * (wheel.isSmooth ? 1 : 3));
        repaint();
        return;
    }
    juce::Component::mouseWheelMove(event, wheel);
}

bool MidiEditorComponent::keyPressed(const juce::KeyPress& key)
{
    if (showingPads)
        return drumPerformancePanel.keyPressed(key);
    const auto command = key.getModifiers().isCommandDown();
    const auto shift = key.getModifiers().isShiftDown();
    if (command && shift && key.getKeyCode() == 'M')
    {
        if (onCaptureRetrospective)
            onCaptureRetrospective();
        return true;
    }
    if (command && key.getKeyCode() == 'A')
    {
        selectedNoteIds.clear();
        if (const auto* clip = currentClip())
            for (const auto& note : clip->notes)
                selectedNoteIds.push_back(note.id);
        repaint();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        selectedNoteIds.clear();
        repaint();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::deleteKey
        || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        removeSelectedNotes();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::returnKey)
    {
        addNoteAt(cursorBeat, cursorPitch);
        return true;
    }

    const auto* clip = currentClip();
    if (clip == nullptr || selectedNoteIds.empty())
        return false;
    if (key.getModifiers().isAltDown()
        && (key.getKeyCode() == juce::KeyPress::upKey
            || key.getKeyCode() == juce::KeyPress::downKey))
    {
        const auto selected = std::find_if(
            clip->notes.cbegin(),
            clip->notes.cend(),
            [this](const auto& note)
            {
                return containsId(selectedNoteIds, note.id);
            });
        if (selected == clip->notes.cend())
            return true;
        auto normalized = 0.5;
        switch (selectedLane())
        {
            case MidiEditorLane::velocity:
                normalized = (selected->velocity - 1.0) / 126.0;
                break;
            case MidiEditorLane::timing:
                normalized = selected->timingOffsetBeats / 0.25 + 0.5;
                break;
            case MidiEditorLane::duration:
                normalized = selected->durationBeats
                    / clip->durationBeats;
                break;
            case MidiEditorLane::probability:
                normalized = selected->probability;
                break;
            case MidiEditorLane::expression:
            {
                const auto expression = std::find_if(
                    selected->expressions.crbegin(),
                    selected->expressions.crend(),
                    [this](const auto& point)
                    {
                        return point.type
                            == selectedExpressionType();
                    });
                if (expression != selected->expressions.crend())
                    normalized = expression->type
                            == MidiExpressionType::pitchBend
                        ? (expression->value + 1.0) * 0.5
                        : expression->value;
                break;
            }
        }
        normalized = juce::jlimit(
            0.0,
            1.0,
            normalized
                + (key.getKeyCode() == juce::KeyPress::upKey
                       ? 0.05
                       : -0.05));
        auto after = *clip;
        if (setMidiLaneValue(
                after,
                selectedNoteIds,
                selectedLane(),
                normalized,
                cursorBeat,
                selectedExpressionType()))
            commitEdit(*clip, std::move(after), "Edit MIDI note lane");
        return true;
    }
    auto after = *clip;
    auto changed = false;
    auto commandName = juce::String("Move MIDI notes");
    if (key.getKeyCode() == juce::KeyPress::leftKey)
    {
        changed = shift
            ? resizeMidiNotes(
                  after,
                  selectedNoteIds,
                  -gridBeats(),
                  gridBeats() * 0.25)
            : moveMidiNotes(after, selectedNoteIds, -gridBeats(), 0);
        commandName = shift ? "Resize MIDI notes" : "Move MIDI notes";
    }
    else if (key.getKeyCode() == juce::KeyPress::rightKey)
    {
        changed = shift
            ? resizeMidiNotes(
                  after,
                  selectedNoteIds,
                  gridBeats(),
                  gridBeats() * 0.25)
            : moveMidiNotes(after, selectedNoteIds, gridBeats(), 0);
        commandName = shift ? "Resize MIDI notes" : "Move MIDI notes";
    }
    else if (key.getKeyCode() == juce::KeyPress::upKey)
    {
        auto deltaPitch = 1;
        if (after.editorMode == MidiEditorMode::drums)
        {
            const auto selected = std::find_if(
                after.notes.cbegin(),
                after.notes.cend(),
                [this](const auto& note)
                {
                    return containsId(selectedNoteIds, note.id);
                });
            const auto rows = drumRows();
            const auto row = selected != after.notes.cend()
                ? std::find_if(
                      rows.cbegin(),
                      rows.cend(),
                      [&selected](const auto* entry)
                      {
                          return entry != nullptr
                              && (entry->noteNumber == selected->pitch
                                  || std::find(
                                         entry->roundRobinNotes.cbegin(),
                                         entry->roundRobinNotes.cend(),
                                         selected->pitch)
                                      != entry->roundRobinNotes.cend());
                      })
                : rows.cend();
            if (row != rows.cend() && row != rows.cbegin())
                deltaPitch = (*(row - 1))->noteNumber - selected->pitch;
        }
        changed = moveMidiNotes(
            after,
            selectedNoteIds,
            0.0,
            deltaPitch,
            currentDrumMap());
    }
    else if (key.getKeyCode() == juce::KeyPress::downKey)
    {
        auto deltaPitch = -1;
        if (after.editorMode == MidiEditorMode::drums)
        {
            const auto selected = std::find_if(
                after.notes.cbegin(),
                after.notes.cend(),
                [this](const auto& note)
                {
                    return containsId(selectedNoteIds, note.id);
                });
            const auto rows = drumRows();
            const auto row = selected != after.notes.cend()
                ? std::find_if(
                      rows.cbegin(),
                      rows.cend(),
                      [&selected](const auto* entry)
                      {
                          return entry != nullptr
                              && (entry->noteNumber == selected->pitch
                                  || std::find(
                                         entry->roundRobinNotes.cbegin(),
                                         entry->roundRobinNotes.cend(),
                                         selected->pitch)
                                      != entry->roundRobinNotes.cend());
                      })
                : rows.cend();
            if (row != rows.cend() && row + 1 != rows.cend())
                deltaPitch = (*(row + 1))->noteNumber - selected->pitch;
        }
        changed = moveMidiNotes(
            after,
            selectedNoteIds,
            0.0,
            deltaPitch,
            currentDrumMap());
    }
    else
    {
        return false;
    }
    if (changed)
        commitEdit(*clip, std::move(after), commandName);
    return true;
}

void MidiEditorComponent::focusGained(FocusChangeType)
{
    repaint();
}

void MidiEditorComponent::focusLost(FocusChangeType)
{
    repaint();
}

const MidiClip* MidiEditorComponent::currentClip() const
{
    return project != nullptr
        ? project->findMidiClip(selectedClipId)
        : nullptr;
}

const MidiClip& MidiEditorComponent::displayedClip() const
{
    if (dragPreview.has_value())
        return *dragPreview;
    return *currentClip();
}

const DrumMap* MidiEditorComponent::currentDrumMap() const
{
    const auto* clip = currentClip();
    if (project == nullptr
        || clip == nullptr
        || clip->editorMode != MidiEditorMode::drums
        || clip->drumMapId.isEmpty())
        return nullptr;
    return project->findDrumMap(clip->drumMapId);
}

juce::Rectangle<int> MidiEditorComponent::noteArea() const
{
    auto area = getLocalBounds().withTrimmedTop(toolbarHeight);
    return area.withTrimmedBottom(
        std::min(laneHeight, std::max(48, area.getHeight() / 3)));
}

juce::Rectangle<int> MidiEditorComponent::laneArea() const
{
    const auto notes = noteArea();
    return getLocalBounds().withTop(notes.getBottom());
}

int MidiEditorComponent::labelWidth() const noexcept
{
    const auto* clip = currentClip();
    return clip != nullptr && clip->editorMode == MidiEditorMode::drums
        ? 190
        : 66;
}

double MidiEditorComponent::gridBeats() const noexcept
{
    switch (gridSelector.getSelectedId())
    {
        case 1: return 1.0;
        case 2: return 0.5;
        case 4: return 0.125;
        case 5: return 1.0 / 6.0;
        case 3:
        default: return 0.25;
    }
}

double MidiEditorComponent::beatForX(float x) const noexcept
{
    const auto* clip = currentClip();
    const auto grid = noteArea().withTrimmedLeft(labelWidth());
    if (clip == nullptr || grid.getWidth() <= 0)
        return 0.0;
    return juce::jlimit(
        0.0,
        clip->durationBeats,
        static_cast<double>(x - static_cast<float>(grid.getX()))
            / static_cast<double>(grid.getWidth())
            * clip->durationBeats);
}

float MidiEditorComponent::xForBeat(double beat) const noexcept
{
    const auto* clip = currentClip();
    const auto grid = noteArea().withTrimmedLeft(labelWidth());
    if (clip == nullptr || clip->durationBeats <= 0.0)
        return static_cast<float>(grid.getX());
    return static_cast<float>(grid.getX())
        + static_cast<float>(
            beat / clip->durationBeats
            * static_cast<double>(grid.getWidth()));
}

int MidiEditorComponent::pitchForY(float y) const
{
    const auto notes = noteArea();
    const auto* clip = currentClip();
    if (clip != nullptr && clip->editorMode == MidiEditorMode::drums)
    {
        const auto rows = drumRows();
        const auto rowHeight = std::max(
            minimumDrumRowHeight,
            static_cast<float>(notes.getHeight())
                / static_cast<float>(std::max(1, pianoRows)));
        const auto row = rowOffset + static_cast<int>(
            std::floor((y - static_cast<float>(notes.getY())) / rowHeight));
        return row >= 0 && row < static_cast<int>(rows.size())
            ? rows[static_cast<std::size_t>(row)]->noteNumber
            : cursorPitch;
    }
    const auto rowHeight = static_cast<float>(notes.getHeight())
        / static_cast<float>(pianoRows);
    const auto row = static_cast<int>(
        std::floor((y - static_cast<float>(notes.getY())) / rowHeight));
    return juce::jlimit(0, 127, 83 - rowOffset - row);
}

float MidiEditorComponent::yForPitch(int pitch) const
{
    const auto notes = noteArea();
    const auto* clip = currentClip();
    if (clip != nullptr && clip->editorMode == MidiEditorMode::drums)
    {
        const auto rows = drumRows();
        const auto row = std::find_if(
            rows.cbegin(),
            rows.cend(),
            [pitch](const auto* entry)
            {
                return entry != nullptr
                    && (entry->noteNumber == pitch
                        || std::find(
                               entry->roundRobinNotes.cbegin(),
                               entry->roundRobinNotes.cend(),
                               pitch)
                            != entry->roundRobinNotes.cend());
            });
        if (row == rows.cend())
            return -1000.0f;
        const auto rowHeight = std::max(
            minimumDrumRowHeight,
            static_cast<float>(notes.getHeight())
                / static_cast<float>(std::max(1, pianoRows)));
        return static_cast<float>(notes.getY())
            + static_cast<float>(
                static_cast<int>(std::distance(rows.cbegin(), row))
                - rowOffset)
                * rowHeight;
    }
    const auto rowHeight = static_cast<float>(notes.getHeight())
        / static_cast<float>(pianoRows);
    return static_cast<float>(notes.getY())
        + static_cast<float>(83 - rowOffset - pitch) * rowHeight;
}

juce::Rectangle<float> MidiEditorComponent::noteBounds(
    const MidiNote& note) const
{
    const auto notes = noteArea();
    const auto* clip = currentClip();
    const auto rowHeight = clip != nullptr
            && clip->editorMode == MidiEditorMode::drums
        ? std::max(
              minimumDrumRowHeight,
              static_cast<float>(notes.getHeight())
                  / static_cast<float>(std::max(1, pianoRows)))
        : static_cast<float>(notes.getHeight())
            / static_cast<float>(pianoRows);
    const auto x = xForBeat(note.actualStartBeats());
    const auto right = xForBeat(note.endBeats());
    return {
        x,
        yForPitch(note.pitch) + 1.0f,
        std::max(5.0f, right - x),
        std::max(4.0f, rowHeight - 2.0f)
    };
}

const MidiNote* MidiEditorComponent::noteAt(
    juce::Point<float> position) const
{
    const auto& clip = displayedClip();
    for (auto iterator = clip.notes.crbegin();
         iterator != clip.notes.crend();
         ++iterator)
        if (noteBounds(*iterator).contains(position))
            return &*iterator;
    return nullptr;
}

std::vector<const DrumMapEntry*> MidiEditorComponent::drumRows() const
{
    std::vector<const DrumMapEntry*> rows;
    if (const auto* map = currentDrumMap())
    {
        rows.reserve(map->entries.size());
        for (const auto& entry : map->entries)
            rows.push_back(&entry);
        std::stable_sort(
            rows.begin(),
            rows.end(),
            [](const auto* left, const auto* right)
            {
                return left->noteNumber > right->noteNumber;
            });
    }
    return rows;
}

int MidiEditorComponent::maximumRowOffset() const
{
    const auto* clip = currentClip();
    if (clip != nullptr && clip->editorMode == MidiEditorMode::drums)
    {
        const auto visible = static_cast<int>(
            std::floor(
                static_cast<float>(noteArea().getHeight())
                / minimumDrumRowHeight));
        return std::max(
            0,
            static_cast<int>(drumRows().size()) - visible);
    }
    return 128 - pianoRows;
}

MidiEditorLane MidiEditorComponent::selectedLane() const noexcept
{
    switch (laneSelector.getSelectedId())
    {
        case 2: return MidiEditorLane::timing;
        case 3: return MidiEditorLane::duration;
        case 4: return MidiEditorLane::probability;
        case 5: return MidiEditorLane::expression;
        case 1:
        default: return MidiEditorLane::velocity;
    }
}

MidiExpressionType
MidiEditorComponent::selectedExpressionType() const noexcept
{
    switch (expressionSelector.getSelectedId())
    {
        case 2: return MidiExpressionType::timbre;
        case 3: return MidiExpressionType::pitchBend;
        case 4: return MidiExpressionType::controller;
        case 5: return MidiExpressionType::channelPressure;
        case 1:
        default: return MidiExpressionType::pressure;
    }
}

void MidiEditorComponent::refreshControls()
{
    const auto* clip = currentClip();
    const auto enabled = clip != nullptr;
    const std::array<juce::Component*, 17> controls {
        &modeButton,
        &laneSelector,
        &expressionSelector,
        &gridSelector,
        &captureButton,
        &humanizeButton,
        &importMapButton,
        &editMapButton,
        &flamButton,
        &rollButton,
        &gravityButton,
        &blastButton,
        &doubleKickButton,
        &patternSelector,
        &expandPatternButton,
        &routingTemplateSelector,
        &applyRoutingButton
    };
    for (auto* component : controls)
    {
        component->setEnabled(enabled);
        component->setVisible(!showingPads);
    }
    modeButton.setVisible(true);
    padsButton.setEnabled(enabled);
    padsButton.setToggleState(showingPads, juce::dontSendNotification);
    captureButton.setVisible(true);
    modeButton.setButtonText(
        clip != nullptr && clip->editorMode == MidiEditorMode::drums
            ? "DRUMS"
            : "PIANO");
    editMapButton.setVisible(
        !showingPads && clip != nullptr && clip->editorMode == MidiEditorMode::drums);
    importMapButton.setVisible(
        clip != nullptr && clip->editorMode == MidiEditorMode::drums);
    for (auto* button : {
             &flamButton,
             &rollButton,
             &gravityButton,
             &blastButton,
             &doubleKickButton })
        button->setVisible(
            !showingPads && clip != nullptr && clip->editorMode == MidiEditorMode::drums);
    expressionSelector.setVisible(
        !showingPads && selectedLane() == MidiEditorLane::expression);
    drumPerformancePanel.setContext(selectedTrackId, clip, currentDrumMap());
    drumPerformancePanel.setVisible(showingPads && enabled);

    patternSelector.clear(juce::dontSendNotification);
    routingTemplateSelector.clear(juce::dontSendNotification);
    if (project != nullptr)
    {
        auto item = 1;
        for (const auto& pattern : project->midiPatterns)
            patternSelector.addItem(pattern.name, item++);
        item = 1;
        for (const auto& routing : project->midiRoutingTemplates)
            routingTemplateSelector.addItem(routing.name, item++);
    }
    if (patternSelector.getNumItems() > 0)
        patternSelector.setSelectedId(1, juce::dontSendNotification);
    if (routingTemplateSelector.getNumItems() > 0)
        routingTemplateSelector.setSelectedId(1, juce::dontSendNotification);
    rowOffset = juce::jlimit(0, maximumRowOffset(), rowOffset);
    resized();
}

void MidiEditorComponent::commitEdit(const MidiClip& before,
                                     MidiClip after,
                                     const juce::String& commandName)
{
    sortMidiNotes(after);
    if (onClipEdited)
        onClipEdited(selectedTrackId, before, after, commandName);
}

void MidiEditorComponent::addNoteAt(double beat, int pitch)
{
    const auto* clip = currentClip();
    if (clip == nullptr)
        return;
    auto after = *clip;
    auto note = createMidiNote(
        after,
        beat,
        pitch,
        gridBeats(),
        100,
        currentDrumMap());
    const auto noteId = note.id;
    after.notes.push_back(std::move(note));
    commitEdit(*clip, std::move(after), "Create MIDI note");
    selectOnly(noteId);
}

void MidiEditorComponent::runEntryTool(MidiEntryTool tool)
{
    const auto* clip = currentClip();
    if (clip == nullptr)
        return;
    MidiEntryRequest request;
    request.startBeats = cursorBeat;
    request.lengthBeats = std::min(
        4.0,
        clip->durationBeats - cursorBeat);
    request.stepBeats = gridBeats();
    request.noteDurationBeats = std::min(0.125, gridBeats());
    request.pitch = cursorPitch;
    auto generated = generateMidiEntryTool(
        tool,
        request,
        currentDrumMap());
    generated.erase(
        std::remove_if(
            generated.begin(),
            generated.end(),
            [clip](const auto& note)
            {
                return note.endBeats() > clip->durationBeats + 0.0000001;
            }),
        generated.end());
    if (generated.empty())
        return;
    auto after = *clip;
    selectedNoteIds.clear();
    for (auto& note : generated)
    {
        selectedNoteIds.push_back(note.id);
        after.notes.push_back(std::move(note));
    }
    commitEdit(*clip, std::move(after), "Insert MIDI entry pattern");
}

void MidiEditorComponent::expandSelectedPattern()
{
    const auto* clip = currentClip();
    if (clip == nullptr
        || project == nullptr
        || patternSelector.getSelectedItemIndex() < 0
        || patternSelector.getSelectedItemIndex()
            >= static_cast<int>(project->midiPatterns.size()))
        return;
    const auto& pattern = project->midiPatterns[
        static_cast<std::size_t>(
            patternSelector.getSelectedItemIndex())];
    auto notes = expandMidiPattern(
        pattern,
        currentDrumMap(),
        cursorBeat,
        1);
    auto after = *clip;
    if (cursorBeat + pattern.lengthBeats > after.durationBeats)
        after.durationBeats = cursorBeat + pattern.lengthBeats;
    selectedNoteIds.clear();
    for (auto& note : notes)
    {
        selectedNoteIds.push_back(note.id);
        after.notes.push_back(std::move(note));
    }
    commitEdit(*clip, std::move(after), "Expand MIDI pattern alias");
}

void MidiEditorComponent::applyLaneAt(juce::Point<float> position)
{
    if (!dragPreview.has_value())
        return;
    if (selectedNoteIds.empty())
    {
        const auto beat = beatForX(position.x);
        const auto nearest = std::min_element(
            dragPreview->notes.cbegin(),
            dragPreview->notes.cend(),
            [beat](const auto& left, const auto& right)
            {
                return std::abs(left.actualStartBeats() - beat)
                    < std::abs(right.actualStartBeats() - beat);
            });
        if (nearest != dragPreview->notes.cend())
            selectOnly(nearest->id);
    }
    const auto lane = laneArea();
    const auto normalized = 1.0
        - juce::jlimit(
            0.0,
            1.0,
            static_cast<double>(
                position.y - static_cast<float>(lane.getY()) - 18.0f)
                / static_cast<double>(std::max(1, lane.getHeight() - 23)));
    juce::ignoreUnused(setMidiLaneValue(
        *dragPreview,
        selectedNoteIds,
        selectedLane(),
        normalized,
        beatForX(position.x),
        selectedExpressionType()));
}

void MidiEditorComponent::selectOnly(const juce::String& noteId)
{
    selectedNoteIds = { noteId };
}

void MidiEditorComponent::removeSelectedNotes()
{
    const auto* clip = currentClip();
    if (clip == nullptr || selectedNoteIds.empty())
        return;
    auto after = *clip;
    if (!deleteMidiNotes(after, selectedNoteIds))
        return;
    commitEdit(*clip, std::move(after), "Delete MIDI notes");
    selectedNoteIds.clear();
    grabKeyboardFocus();
}
}
