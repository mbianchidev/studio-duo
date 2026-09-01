#include "MainComponent.h"

#include "plugin_host/PluginStateStore.h"

#include <StudioDuoBrandData.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace studio
{
namespace
{
class TrackColourSelector final : public juce::ColourSelector,
                                  private juce::ChangeListener
{
public:
    TrackColourSelector(juce::Colour initial,
                        std::function<void(juce::Colour)> previewCallback,
                        std::function<void(juce::Colour, juce::Colour)> commitCallback)
        : juce::ColourSelector(showColourAtTop
                                   | editableColour
                                   | showSliders
                                   | showColourspace),
          initialColour(initial),
          preview(std::move(previewCallback)),
          commit(std::move(commitCallback))
    {
        setName("Track colour");
        setSize(320, 400);
        setCurrentColour(initialColour, juce::dontSendNotification);
        addChangeListener(this);
    }

    ~TrackColourSelector() override
    {
        removeChangeListener(this);
        const auto selected = getCurrentColour();
        if (selected != initialColour && commit)
            commit(initialColour, selected);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (preview)
            preview(getCurrentColour());
    }

    juce::Colour initialColour;
    std::function<void(juce::Colour)> preview;
    std::function<void(juce::Colour, juce::Colour)> commit;
};

class TrackQuickEditor final : public juce::Component
{
public:
    TrackQuickEditor(juce::String currentName,
                     juce::Colour currentColour,
                     std::function<void(juce::String, juce::Colour)> applyCallback)
        : colourSelector(juce::ColourSelector::showColourAtTop
                             | juce::ColourSelector::editableColour
                             | juce::ColourSelector::showSliders
                             | juce::ColourSelector::showColourspace),
          apply(std::move(applyCallback))
    {
        nameEditor.setText(std::move(currentName), false);
        nameEditor.setSelectAllWhenFocused(true);
        nameEditor.setColour(juce::TextEditor::backgroundColourId,
                             juce::Colour(StudioColours::panel));
        nameEditor.setColour(juce::TextEditor::outlineColourId,
                             juce::Colour(StudioColours::border));
        nameEditor.setColour(juce::TextEditor::focusedOutlineColourId,
                             juce::Colour(StudioColours::orange));
        nameEditor.onReturnKey = [this] { applyAndClose(); };
        nameEditor.onEscapeKey = [this] { close(); };
        addAndMakeVisible(nameEditor);

        colourSelector.setCurrentColour(currentColour,
                                        juce::dontSendNotification);
        addAndMakeVisible(colourSelector);

        applyButton.onClick = [this] { applyAndClose(); };
        cancelButton.onClick = [this] { close(); };
        addAndMakeVisible(applyButton);
        addAndMakeVisible(cancelButton);
        setSize(340, 430);
    }

    void focusName()
    {
        nameEditor.grabKeyboardFocus();
        nameEditor.selectAll();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        nameEditor.setBounds(bounds.removeFromTop(34));
        bounds.removeFromTop(8);
        auto buttons = bounds.removeFromBottom(34);
        cancelButton.setBounds(buttons.removeFromRight(92).reduced(2));
        applyButton.setBounds(buttons.removeFromRight(92).reduced(2));
        bounds.removeFromBottom(8);
        colourSelector.setBounds(bounds);
    }

private:
    void applyAndClose()
    {
        const auto name = nameEditor.getText().trim();
        if (name.isEmpty())
        {
            nameEditor.setColour(juce::TextEditor::outlineColourId,
                                 juce::Colour(StudioColours::orange));
            nameEditor.grabKeyboardFocus();
            return;
        }
        if (apply)
            apply(name, colourSelector.getCurrentColour());
        close();
    }

    void close()
    {
        if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
            callout->dismiss();
    }

    juce::TextEditor nameEditor;
    juce::ColourSelector colourSelector;
    juce::TextButton applyButton { "APPLY" };
    juce::TextButton cancelButton { "CANCEL" };
    std::function<void(juce::String, juce::Colour)> apply;
};
}

MainComponent::MainComponent()
{
    setLookAndFeel(&theme);
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addKeyListener(this);

    brandLogo = juce::Drawable::createFromImageData(
        studio_brand::studioduoicon_svg,
        static_cast<std::size_t>(studio_brand::studioduoicon_svgSize));

    const auto configureButton = [this](juce::Button& button, const juce::String& tooltip)
    {
        addAndMakeVisible(button);
        button.setTooltip(tooltip);
        button.setWantsKeyboardFocus(false);
        button.addKeyListener(this);
    };

    configureButton(newButton, "Create a new project");
    configureButton(openButton, "Open a .studioduo project");
    configureButton(saveButton, "Save project (Command/Ctrl+S)");
    configureButton(exportButton, "Export a stereo WAV");
    configureButton(audioSetupButton, "Configure audio and MIDI devices");
    configureButton(undoButton, "Undo (Command/Ctrl+Z)");
    configureButton(redoButton, "Redo (Command/Ctrl+Shift+Z)");
    configureButton(playButton, "Play or pause (Space)");
    configureButton(stopButton, "Stop and return to the start");
    configureButton(recordButton, "Record the selected audio track");
    configureButton(loopButton, "Loop the project range");
    configureButton(metronomeButton, "Toggle the metronome");
    configureButton(addTrackButton, "Add an audio, aux, bus, folder, VCA, or control-room track");
    configureButton(addBusButton, "Add a stereo bus track");
    configureButton(importButton, "Import WAV, AIFF, FLAC, or MP3 audio");
    configureButton(duplicateTrackButton, "Duplicate the selected track and its edits");
    configureButton(deleteTrackButton, "Delete the selected track");
    configureButton(trackingButton, "Configure tempo, meter, punch, count-in, and click routing");
    configureButton(muteButton, "Mute selected track");
    configureButton(soloButton, "Solo selected track");
    configureButton(armButton, "Arm selected track for recording");
    configureButton(trackColourButton, "Change selected track colour");
    configureButton(stereoInputButton, "Capture this input and the following input as stereo");
    configureButton(monitorButton, "Monitor the selected track input through Studio Duo");
    configureButton(splitClipButton, "Split the selected clip at the playhead");
    configureButton(deleteClipButton, "Delete the selected clip");
    configureButton(trimClipStartButton, "Trim selected clip start to playhead ([)");
    configureButton(trimClipEndButton, "Trim selected clip end to playhead (])");
    configureButton(zoomOutButton, "Zoom timeline out (Command/Ctrl+-)");
    configureButton(zoomResetButton, "Reset timeline zoom (Command/Ctrl+0)");
    configureButton(zoomInButton, "Zoom timeline in (Command/Ctrl++)");

    newButton.onClick = [this] { createNewProject(); };
    openButton.onClick = [this] { beginOpenProject(); };
    saveButton.onClick = [this] { beginSaveProject(); };
    exportButton.onClick = [this] { beginExportMix(); };
    audioSetupButton.onClick = [this] { showAudioSettings(); };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    playButton.onClick = [this] { togglePlayback(); };
    stopButton.onClick = [this] { stopTransportAndRecording(); };
    recordButton.onClick = [this] { toggleRecording(); };
    addTrackButton.onClick = [this] { showAddTrackMenu(); };
    addBusButton.onClick = [this] { addBusTrack(); };
    importButton.onClick = [this] { beginImportAudio(); };
    duplicateTrackButton.onClick = [this] { duplicateSelectedTrack(); };
    deleteTrackButton.onClick = [this] { deleteSelectedTrack(); };
    trackingButton.onClick = [this] { showTrackingMenu(); };
    muteButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.muted = !state.muted; });
    };
    soloButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.solo = !state.solo; });
    };
    armButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.armed = !state.armed; });
    };
    trackColourButton.onClick = [this] { showTrackColourMenu(); };
    stereoInputButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.stereoInput = !state.stereoInput; });
    };
    monitorButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.inputMonitoring = !state.inputMonitoring; });
    };
    splitClipButton.onClick = [this] { splitSelectedClip(); };
    deleteClipButton.onClick = [this] { deleteSelectedClip(); };
    trimClipStartButton.onClick = [this] { trimSelectedClipStartToPlayhead(); };
    trimClipEndButton.onClick = [this] { trimSelectedClipEndToPlayhead(); };
    zoomOutButton.onClick = [this] { zoomTimeline(1.0 / 1.25); };
    zoomResetButton.onClick = [this] { zoomTimeline(1.0, true); };
    zoomInButton.onClick = [this] { zoomTimeline(1.25); };

    loopButton.setToggleState(project.loopEnabled, juce::dontSendNotification);
    loopButton.onClick = [this]
    {
        changeTransportState([this](auto& state)
        {
            state.loopEnabled = loopButton.getToggleState();
        });
    };

    metronomeButton.setToggleState(project.metronomeEnabled, juce::dontSendNotification);
    metronomeButton.onClick = [this]
    {
        changeTransportState([this](auto& state)
        {
            state.metronomeEnabled = metronomeButton.getToggleState();
        });
    };

    addAndMakeVisible(projectLabel);
    projectLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    projectLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(positionLabel);
    positionLabel.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    positionLabel.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(tempoLabel);
    tempoLabel.setText("BPM", juce::dontSendNotification);
    tempoLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    tempoLabel.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(tempoSlider);
    tempoSlider.setSliderStyle(juce::Slider::LinearBar);
    tempoSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 54, 24);
    tempoSlider.setRange(20.0, 400.0, 1.0);
    tempoSlider.setValue(project.tempo, juce::dontSendNotification);
    tempoSlider.setDoubleClickReturnValue(true, 120.0);
    tempoSlider.onDragStart = [this]
    {
        tempoEditStart = ProjectTransportState::fromProject(project);
        tempoEditActive = true;
    };
    tempoSlider.onValueChange = [this]
    {
        const auto applyTempo = [this](ProjectTransportState& state)
        {
            state.tempo = tempoSlider.getValue();
            if (!state.tempoChanges.empty()
                && state.tempoChanges.front().timeSeconds <= 0.0001)
                state.tempoChanges.front().bpm = state.tempo;
        };

        if (tempoEditActive)
        {
            auto preview = ProjectTransportState::fromProject(project);
            applyTempo(preview);
            project.tempo = preview.tempo;
            project.tempoChanges = std::move(preview.tempoChanges);
            projectChanged(false, false);
            return;
        }

        changeTransportState(applyTempo);
    };
    tempoSlider.onDragEnd = [this]
    {
        if (!tempoEditActive)
            return;
        tempoEditActive = false;
        const auto after = ProjectTransportState::fromProject(project);
        perform(std::make_unique<SetProjectTransportCommand>(tempoEditStart, after));
    };
    tempoSlider.addKeyListener(this);

    addAndMakeVisible(inspectorName);
    inspectorName.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    inspectorName.setEditable(false, true, false);
    inspectorName.setTooltip("Double-click to rename the selected audio track");
    inspectorName.onTextChange = [this]
    {
        if (updatingTrackName || selectedClipId.isNotEmpty())
            return;

        const auto* track = project.findTrack(selectedTrackId);
        if (track == nullptr || track->type == TrackType::master)
            return;

        const auto replacement = inspectorName.getText().trim();
        if (replacement.isEmpty())
        {
            updatingTrackName = true;
            inspectorName.setText(track->name, juce::dontSendNotification);
            updatingTrackName = false;
            setStatus("A track name cannot be empty.", true);
            return;
        }
        if (replacement == track->name)
            return;

        perform(std::make_unique<RenameTrackCommand>(track->id, replacement));
    };
    addAndMakeVisible(inspectorDetails);
    inspectorDetails.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));

    addAndMakeVisible(inputLabel);
    inputLabel.setText("INPUT", juce::dontSendNotification);
    inputLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    addAndMakeVisible(inputSelector);
    inputSelector.setTooltip("Select the hardware input for this audio track");
    inputSelector.onChange = [this]
    {
        if (updatingInputControls || inputSelector.getSelectedItemIndex() < 0)
            return;

        const auto* track = project.findTrack(selectedTrackId);
        if (track == nullptr || track->type != TrackType::audio)
            return;

        const auto before = TrackMixState::fromTrack(*track);
        auto after = before;
        after.inputChannel = inputSelector.getSelectedItemIndex();
        perform(std::make_unique<SetTrackMixCommand>(track->id, before, after));
    };

    addAndMakeVisible(outputLabel);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    outputLabel.setColour(juce::Label::textColourId,
                          juce::Colour(StudioColours::secondaryText));
    addAndMakeVisible(outputSelector);
    outputSelector.setTooltip("Route this track through a bus or directly to the master");
    outputSelector.onChange = [this]
    {
        const auto index = outputSelector.getSelectedItemIndex();
        if (updatingOutputControls
            || index < 0
            || index >= static_cast<int>(outputTrackIds.size()))
            return;

        const auto* track = project.findTrack(selectedTrackId);
        if (track == nullptr
            || track->type == TrackType::master
            || track->parentTrackId.isNotEmpty())
            return;
        const auto& destinationId = outputTrackIds[static_cast<std::size_t>(index)];
        if (track->outputTrackId == destinationId)
            return;
        perform(std::make_unique<SetTrackOutputCommand>(track->id, destinationId));
    };

    addAndMakeVisible(volumeLabel);
    volumeLabel.setText("VOLUME", juce::dontSendNotification);
    volumeLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    addAndMakeVisible(panLabel);
    panLabel.setText("PAN", juce::dontSendNotification);
    panLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));

    const auto configureInspectorSlider = [this](juce::Slider& slider,
                                                 double minimum,
                                                 double maximum,
                                                 double interval)
    {
        addAndMakeVisible(slider);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
        slider.setRange(minimum, maximum, interval);
        slider.addKeyListener(this);
    };
    configureInspectorSlider(volumeSlider, -60.0, 12.0, 0.1);
    volumeSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    volumeSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 24);
    volumeSlider.setDoubleClickReturnValue(true, 0.0);
    volumeSlider.setNumDecimalPlacesToDisplay(1);
    volumeSlider.setTextValueSuffix(" dB");
    configureInspectorSlider(panSlider, -1.0, 1.0, 0.01);
    panSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    panSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 24);
    panSlider.setDoubleClickReturnValue(true, 0.0);
    panSlider.textFromValueFunction = [](double value)
    {
        if (std::abs(value) < 0.005)
            return juce::String("Center");
        return juce::String(static_cast<int>(std::round(std::abs(value) * 100.0)))
            + (value < 0.0 ? "% L" : "% R");
    };
    panSlider.valueFromTextFunction = [](const juce::String& text)
    {
        const auto value = juce::jlimit(0.0, 100.0, static_cast<double>(text.getFloatValue())) / 100.0;
        return text.containsIgnoreCase("L") ? -value
                                            : text.containsIgnoreCase("R") ? value : 0.0;
    };

    volumeSlider.onDragStart = [this]
    {
        if (auto* track = project.findTrack(selectedTrackId))
            volumeSlider.getProperties().set("start", track->volumeDecibels);
    };
    volumeSlider.onValueChange = [this]
    {
        if (auto* track = project.findTrack(selectedTrackId))
        {
            track->volumeDecibels = static_cast<float>(volumeSlider.getValue());
            projectChanged(false, false);
        }
    };
    volumeSlider.onDragEnd = [this]
    {
        auto* track = project.findTrack(selectedTrackId);
        if (track == nullptr)
            return;

        auto before = TrackMixState::fromTrack(*track);
        before.volumeDecibels = static_cast<float>(
            static_cast<double>(volumeSlider.getProperties().getWithDefault("start",
                                                                            track->volumeDecibels)));
        const auto after = TrackMixState::fromTrack(*track);
        perform(std::make_unique<SetTrackMixCommand>(track->id, before, after));
    };

    panSlider.onDragStart = [this]
    {
        if (auto* track = project.findTrack(selectedTrackId))
            panSlider.getProperties().set("start", track->pan);
    };
    panSlider.onValueChange = [this]
    {
        if (auto* track = project.findTrack(selectedTrackId))
        {
            track->pan = static_cast<float>(panSlider.getValue());
            projectChanged(false);
        }
    };
    panSlider.onDragEnd = [this]
    {
        auto* track = project.findTrack(selectedTrackId);
        if (track == nullptr)
            return;

        auto before = TrackMixState::fromTrack(*track);
        before.pan = static_cast<float>(
            static_cast<double>(panSlider.getProperties().getWithDefault("start", track->pan)));
        const auto after = TrackMixState::fromTrack(*track);
        perform(std::make_unique<SetTrackMixCommand>(track->id, before, after));
    };

    timelineViewport.setViewedComponent(&timeline, false);
    timelineViewport.setScrollBarsShown(true, true);
    timelineViewport.setWantsKeyboardFocus(false);
    timelineViewport.addKeyListener(this);
    addAndMakeVisible(timelineViewport);
    timeline.addKeyListener(this);
    timeline.setProject(&project);
    timeline.onTrackSelected = [this](const auto& trackId) { selectTrack(trackId); };
    timeline.onEditTrack = [this](const auto& trackId, auto targetArea)
    {
        selectTrack(trackId);
        showTrackQuickEditor(trackId, targetArea);
    };
    timeline.onTrackMute = [this](const auto& trackId)
    {
        selectTrack(trackId);
        changeSelectedTrackState([](auto& state) { state.muted = !state.muted; });
    };
    timeline.onTrackSolo = [this](const auto& trackId)
    {
        selectTrack(trackId);
        changeSelectedTrackState([](auto& state) { state.solo = !state.solo; });
    };
    timeline.onTrackArm = [this](const auto& trackId)
    {
        selectTrack(trackId);
        changeSelectedTrackState([](auto& state) { state.armed = !state.armed; });
    };
    timeline.onToggleTrackVersions = [this](const auto& trackId)
    {
        selectTrack(trackId);
        changeSelectedTrackState([](auto& state)
        {
            state.versionsCollapsed = !state.versionsCollapsed;
        });
    };
    timeline.onDuplicateTrack = [this](const auto& trackId)
    {
        selectTrack(trackId);
        duplicateSelectedTrack();
    };
    timeline.onDeleteTrack = [this](const auto& trackId)
    {
        selectTrack(trackId);
        deleteSelectedTrack();
    };
    timeline.onAddTrack = [this] { addAudioTrack(); };
    timeline.onClipSelected = [this](const auto& trackId, const auto& clipId)
    {
        selectClip(trackId, clipId);
    };
    timeline.onClipMoved = [this](const auto& clipId, const auto& destinationTrackId, double start)
    {
        moveClip(clipId, destinationTrackId, start);
    };
    timeline.onClipTrimmed = [this](const auto& clipId,
                                    double start,
                                    double sourceOffset,
                                    double duration)
    {
        trimClip(clipId, start, sourceOffset, duration);
    };
    timeline.onSeek = [this](double seconds)
    {
        if (project.hasActivePluginInserts())
        {
            playAfterRuntimeTransition = audioEngine.isPlaying();
            audioEngine.pause();
            audioEngine.forcePluginRuntimeReload(project, pluginRuntimeRequests());
            setStatus("Playhead moved. Resetting sandbox pipelines...");
        }
        audioEngine.seekSeconds(seconds);
        timeline.setPlayheadSeconds(seconds);
    };
    timeline.onZoomRequested = [this](double factor) { zoomTimeline(factor); };
    timeline.onSplitSelected = [this] { splitSelectedClip(); };
    timeline.onTrimStartSelected = [this] { trimSelectedClipStartToPlayhead(); };
    timeline.onTrimEndSelected = [this] { trimSelectedClipEndToPlayhead(); };
    timeline.onDeleteSelected = [this] { deleteSelectedClip(); };
    timeline.onUseTake = [this](const auto& takeTrackId)
    {
        const auto* take = project.findTrack(takeTrackId);
        if (take == nullptr || take->parentTrackId.isEmpty())
            return;
        perform(std::make_unique<SetActiveTakeCommand>(take->parentTrackId,
                                                       takeTrackId));
    };
    timeline.onUseClipForComp = [this](const auto& clipId)
    {
        const auto* take = project.findTrackContainingClip(clipId);
        const auto* clip = project.findClip(clipId);
        if (take == nullptr || clip == nullptr || take->parentTrackId.isEmpty())
            return;
        const auto* parent = project.findTrack(take->parentTrackId);
        if (parent == nullptr)
            return;

        const auto* group = project.editGroupForTrack(parent->id);
        const auto reference = clip->startSeconds + clip->durationSeconds * 0.5;
        const auto parentIds = group != nullptr && group->enabled
            ? group->trackIds
            : std::vector<juce::String> { parent->id };
        std::vector<std::unique_ptr<ProjectCommand>> commands;
        for (const auto& parentId : parentIds)
        {
            const auto* compParent = project.findTrack(parentId);
            const auto* sourceClip = parentId == parent->id
                ? clip
                : activeClipAt(parentId, reference);
            const auto* sourceTake = sourceClip != nullptr
                ? project.findTrackContainingClip(sourceClip->id)
                : nullptr;
            if (compParent == nullptr
                || sourceTake == nullptr
                || sourceTake->parentTrackId != parentId)
            {
                setStatus("Every linked parent needs a take at the comp range.", true);
                return;
            }

            CompRegion region;
            region.sourceTrackId = sourceTake->id;
            region.startSeconds = clip->startSeconds;
            region.durationSeconds = clip->durationSeconds;
            auto updated = replaceCompRegion(compParent->compRegions,
                                             std::move(region));
            commands.push_back(std::make_unique<SetCompRegionsCommand>(
                compParent->id,
                compParent->compRegions,
                std::move(updated)));
        }
        perform(std::make_unique<BatchProjectCommand>(
            commands.size() > 1 ? "Comp linked takes" : "Change comp",
            std::move(commands)));
    };
    timeline.onClearComp = [this](const auto& parentTrackId)
    {
        const auto* parent = project.findTrack(parentTrackId);
        if (parent == nullptr || parent->compRegions.empty())
            return;
        const auto* group = project.editGroupForTrack(parentTrackId);
        const auto parentIds = group != nullptr && group->enabled
            ? group->trackIds
            : std::vector<juce::String> { parentTrackId };
        std::vector<std::unique_ptr<ProjectCommand>> commands;
        for (const auto& parentId : parentIds)
        {
            const auto* compParent = project.findTrack(parentId);
            if (compParent != nullptr && !compParent->compRegions.empty())
            {
                commands.push_back(std::make_unique<SetCompRegionsCommand>(
                    parentId,
                    compParent->compRegions,
                    std::vector<CompRegion> {}));
            }
        }
        if (!commands.empty())
        {
            perform(std::make_unique<BatchProjectCommand>(
                commands.size() > 1 ? "Clear linked comps" : "Clear comp",
                std::move(commands)));
        }
    };
    timeline.onAnalyseTransients = [this](const auto& clipId)
    {
        analyseClipTransients(clipId);
    };
    timeline.onSetStretchMode = [this](const auto& clipId, auto mode)
    {
        setClipStretchMode(clipId, mode);
    };
    timeline.onSetPlaybackRate = [this](const auto& clipId, double rate)
    {
        setClipPlaybackRate(clipId, rate);
    };
    timeline.onWarpTransientToTimeline = [this](const auto& clipId, double seconds)
    {
        warpClipTransient(clipId, seconds);
    };
    timeline.onSetFadeIn = [this](const auto& clipId, double seconds)
    {
        setClipFade(clipId, seconds, true);
    };
    timeline.onSetFadeOut = [this](const auto& clipId, double seconds)
    {
        setClipFade(clipId, seconds, false);
    };
    timeline.onCreateCrossfade = [this](const auto& clipId)
    {
        createClipCrossfade(clipId);
    };
    timeline.onToggleClipPolarity = [this](const auto& clipId)
    {
        toggleClipPolarity(clipId);
    };
    timeline.onToggleClipReverse = [this](const auto& clipId)
    {
        toggleClipReverse(clipId);
    };
    timeline.onConsolidateClip = [this](const auto& clipId)
    {
        consolidateClip(clipId);
    };

    mixer = std::make_unique<MixerPanel>();
    mixer->setProject(&project);
    mixer->onTrackSelected = [this](const auto& trackId) { selectTrack(trackId); };
    mixer->onEditTrack = [this](const auto& trackId, auto targetArea)
    {
        selectTrack(trackId);
        showTrackQuickEditor(trackId, targetArea);
    };
    mixer->onVolumeChanged = [this](const auto& trackId, float volume)
    {
        const auto* track = project.findTrack(trackId);
        if (track == nullptr)
            return;

        const auto before = TrackMixState::fromTrack(*track);
        auto after = before;
        after.volumeDecibels = juce::jlimit(-60.0f, 12.0f, volume);
        perform(std::make_unique<SetTrackMixCommand>(trackId, before, after));
    };
    mixer->onPanChanged = [this](const auto& trackId, float pan)
    {
        const auto* track = project.findTrack(trackId);
        if (track == nullptr)
            return;

        const auto before = TrackMixState::fromTrack(*track);
        auto after = before;
        after.pan = juce::jlimit(-1.0f, 1.0f, pan);
        perform(std::make_unique<SetTrackMixCommand>(trackId, before, after));
    };
    mixer->addKeyListener(this);
    addAndMakeVisible(*mixer);

    pluginBrowser = std::make_unique<PluginBrowserComponent>(pluginCatalog);
    pluginBrowser->addKeyListener(this);
    pluginBrowser->onPluginActivated = [this](const auto& entry)
    {
        addPluginToSelectedTrack(entry);
    };
    addAndMakeVisible(*pluginBrowser);

    routingPanel = std::make_unique<RoutingPanel>();
    routingPanel->setProject(&project);
    routingPanel->onAddConnection = [this](auto connection)
    {
        perform(std::make_unique<AddRoutingConnectionCommand>(
            std::move(connection)));
    };
    routingPanel->onUpdateConnection = [this](auto before, auto after)
    {
        perform(std::make_unique<UpdateRoutingConnectionCommand>(
            std::move(before),
            std::move(after)));
    };
    routingPanel->onRemoveConnection = [this](const auto& connectionId)
    {
        perform(std::make_unique<RemoveRoutingConnectionCommand>(
            connectionId));
    };
    routingPanel->onTrackRoutingChanged = [this](
                                              const auto& trackId,
                                              auto before,
                                              auto after)
    {
        perform(std::make_unique<SetTrackRoutingStateCommand>(
            trackId,
            std::move(before),
            std::move(after)));
    };
    routingPanel->addKeyListener(this);
    addAndMakeVisible(*routingPanel);

    insertPanel = std::make_unique<PluginInsertPanel>();
    insertPanel->setProject(&project);
    insertPanel->onBypass = [this](const auto& trackId, const auto& insertId, bool bypassed)
    {
        perform(std::make_unique<SetPluginBypassCommand>(trackId, insertId, bypassed));
    };
    insertPanel->onRemove = [this](const auto& trackId, const auto& insertId)
    {
        perform(std::make_unique<RemovePluginInsertCommand>(trackId, insertId));
    };
    insertPanel->onModeChange = [this](
                                    const auto& trackId,
                                    const auto& insertId,
                                    auto mode)
    {
        changePluginMode(trackId, insertId, mode);
    };
    insertPanel->onReplace = [this](
                                  const auto& trackId,
                                  const auto& insertId)
    {
        selectTrack(trackId);
        replacementInsertId = insertId;
        setStatus("Choose a catalog plugin to replace the missing insert.");
    };
    insertPanel->onReload = [this](const auto& trackId, const auto& insertId)
    {
        if (const auto* track = project.findTrack(trackId))
        {
            const auto insert = std::find_if(
                track->inserts.cbegin(),
                track->inserts.cend(),
                [&insertId](const auto& candidate)
                {
                    return candidate.id == insertId;
                });
            if (insert != track->inserts.cend()
                && insert->recoveryDisabled)
            {
                perform(std::make_unique<SetPluginBridgeModeCommand>(
                    trackId,
                    insertId,
                    insert->bridgeMode));
            }
        }
        audioEngine.forcePluginRuntimeReload(
            project,
            pluginRuntimeRequests(),
            insertId);
        setStatus("Reloading selected plugin runtime...");
    };
    insertPanel->addKeyListener(this);
    addAndMakeVisible(*insertPanel);

    addAndMakeVisible(statusLabel);
    statusLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    statusLabel.setJustificationType(juce::Justification::centredLeft);

    selectedTrackId = project.tracks.front().id;
    selectTrack(selectedTrackId);

    if (const auto result = audioEngine.initialise(deviceManager); result.failed())
        setStatus(result.getErrorMessage(), true);
    else
        setStatus("Ready. Import audio or arm a track and record.");

    projectChanged(false);
    startTimerHz(30);
    setSize(1480, 900);
}

MainComponent::~MainComponent()
{
    stopTimer();
    if (audioEngine.isRecording())
        audioEngine.stopRecording();
    audioEngine.shutdown();
    const auto recoveryPending = std::any_of(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [](const auto& track)
        {
            return std::any_of(
                track.inserts.cbegin(),
                track.inserts.cend(),
                [](const auto& insert)
                {
                    return insert.recoveryDisabled;
                });
        });
    if (projectPackage.exists() && !recoveryPending)
        ProjectFile::clearReducedIsolationMarker(projectPackage);
    timelineViewport.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));

    auto bounds = getLocalBounds();
    const auto header = bounds.removeFromTop(76);
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(header);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawHorizontalLine(header.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
    if (brandLogo != nullptr)
    {
        brandLogo->drawWithin(graphics,
                              juce::Rectangle<float>(16.0f, 12.0f, 52.0f, 52.0f),
                              juce::RectanglePlacement::centred,
                              1.0f);
    }

    const auto bodyTop = 76;
    const auto mixerTop = getHeight() - 248;
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, bodyTop, 286, mixerTop - bodyTop);
    graphics.fillRect(getWidth() - 250, bodyTop, 250, mixerTop - bodyTop);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawVerticalLine(285, static_cast<float>(bodyTop), static_cast<float>(mixerTop));
    graphics.drawVerticalLine(getWidth() - 251,
                              static_cast<float>(bodyTop),
                              static_cast<float>(mixerTop));
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(286, bodyTop, getWidth() - 536, 38);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawHorizontalLine(bodyTop + 37,
                                286.0f,
                                static_cast<float>(getWidth() - 250));

    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(10.5f);
    graphics.drawText("SESSION",
                      16,
                      bodyTop + 12,
                      180,
                      18,
                      juce::Justification::centredLeft);
    graphics.drawText("INSPECTOR",
                      getWidth() - 234,
                      bodyTop + 12,
                      200,
                      18,
                      juce::Justification::centredLeft);
    graphics.drawText("MIXER",
                      14,
                      mixerTop + 8,
                      120,
                      18,
                      juce::Justification::centredLeft);

    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
    graphics.drawText("STUDIO", 18, 11, 78, 18, juce::Justification::centredLeft);
    graphics.setColour(juce::Colour(StudioColours::orange));
    graphics.drawText("DUO", 74, 11, 48, 18, juce::Justification::centredLeft);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop(76);
    auto status = bounds.removeFromBottom(28);
    auto mixerBounds = bounds.removeFromBottom(220);
    auto left = bounds.removeFromLeft(286);
    auto right = bounds.removeFromRight(250);

    statusLabel.setBounds(status.reduced(10, 0));
    mixer->setBounds(mixerBounds);
    auto editToolbar = bounds.removeFromTop(38).reduced(7, 4);
    auto zoomControls = editToolbar.removeFromRight(132);
    zoomOutButton.setBounds(zoomControls.removeFromLeft(36).reduced(2));
    zoomResetButton.setBounds(zoomControls.removeFromLeft(60).reduced(2));
    zoomInButton.setBounds(zoomControls.removeFromLeft(36).reduced(2));
    trimClipStartButton.setBounds(editToolbar.removeFromLeft(108).reduced(2));
    splitClipButton.setBounds(editToolbar.removeFromLeft(118).reduced(2));
    trimClipEndButton.setBounds(editToolbar.removeFromLeft(108).reduced(2));
    deleteClipButton.setBounds(editToolbar.removeFromLeft(96).reduced(2));
    timelineViewport.setBounds(bounds);

    auto topRow = header.reduced(14, 8);
    auto brand = topRow.removeFromLeft(170);
    projectLabel.setBounds(brand.withTrimmedLeft(60).withTrimmedTop(24).withHeight(28));

    auto fileControls = topRow.removeFromLeft(320);
    newButton.setBounds(fileControls.removeFromLeft(58).reduced(3, 12));
    openButton.setBounds(fileControls.removeFromLeft(62).reduced(3, 12));
    saveButton.setBounds(fileControls.removeFromLeft(62).reduced(3, 12));
    exportButton.setBounds(fileControls.removeFromLeft(74).reduced(3, 12));
    audioSetupButton.setBounds(fileControls.removeFromLeft(48).reduced(3, 12));

    auto editControls = topRow.removeFromLeft(128);
    undoButton.setBounds(editControls.removeFromLeft(62).reduced(3, 12));
    redoButton.setBounds(editControls.removeFromLeft(62).reduced(3, 12));

    auto transport = topRow.removeFromLeft(270);
    playButton.setBounds(transport.removeFromLeft(66).reduced(3, 9));
    stopButton.setBounds(transport.removeFromLeft(62).reduced(3, 9));
    recordButton.setBounds(transport.removeFromLeft(58).reduced(3, 9));
    loopButton.setBounds(transport.removeFromLeft(76).reduced(3, 9));

    auto tempoArea = topRow.removeFromRight(180);
    tempoLabel.setBounds(tempoArea.removeFromRight(38).reduced(0, 10));
    tempoSlider.setBounds(tempoArea.reduced(3, 10));
    metronomeButton.setBounds(topRow.removeFromRight(78).reduced(3, 9));
    positionLabel.setBounds(topRow.reduced(6, 8));

    auto sessionPanel = left.reduced(14, 42);
    addTrackButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    addBusButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    importButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    duplicateTrackButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    deleteTrackButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    trackingButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(18);
    pluginBrowser->setBounds(sessionPanel);

    auto inspector = right.reduced(16, 42);
    inspectorName.setBounds(inspector.removeFromTop(28));
    inspectorDetails.setBounds(inspector.removeFromTop(24));
    inspector.removeFromTop(12);
    inputLabel.setBounds(inspector.removeFromTop(20));
    inputSelector.setBounds(inspector.removeFromTop(30));
    inspector.removeFromTop(6);
    auto inputToggles = inspector.removeFromTop(28);
    stereoInputButton.setBounds(inputToggles.removeFromLeft(94).reduced(2));
    monitorButton.setBounds(inputToggles.removeFromLeft(100).reduced(2));
    inspector.removeFromTop(8);
    outputLabel.setBounds(inspector.removeFromTop(20));
    outputSelector.setBounds(inspector.removeFromTop(30));
    inspector.removeFromTop(8);
    auto mixLabels = inspector.removeFromTop(20);
    volumeLabel.setBounds(mixLabels.removeFromLeft(109));
    panLabel.setBounds(mixLabels);
    auto mixControls = inspector.removeFromTop(96);
    volumeSlider.setBounds(mixControls.removeFromLeft(109).reduced(5, 0));
    panSlider.setBounds(mixControls.reduced(5, 0));
    inspector.removeFromTop(14);
    auto toggles = inspector.removeFromTop(34);
    muteButton.setBounds(toggles.removeFromLeft(52).reduced(2));
    soloButton.setBounds(toggles.removeFromLeft(52).reduced(2));
    armButton.setBounds(toggles.removeFromLeft(52).reduced(2));
    trackColourButton.setBounds(toggles.removeFromLeft(52).reduced(2));
    inspector.removeFromTop(10);
    routingPanel->setBounds(inspector.removeFromTop(154));
    inspector.removeFromTop(10);
    insertPanel->setBounds(inspector);

    updateTimelineSize();
    timeline.setViewportPosition(timelineViewport.getViewPositionX());
}

void MainComponent::timerCallback()
{
    if (auto calibration = audioEngine.takeLatencyCalibrationResult();
        calibration.has_value())
    {
        const auto routeId = calibratingReampRouteId;
        calibratingReampRouteId.clear();
        if (calibration->result.failed())
        {
            showError("Reamp calibration failed",
                      calibration->result.getErrorMessage());
        }
        else
        {
            changeReampRoutes([routeId, calibration](auto& routes)
            {
                const auto route = std::find_if(
                    routes.begin(),
                    routes.end(),
                    [&routeId](const auto& candidate)
                    {
                        return candidate.id == routeId;
                    });
                if (route != routes.end())
                    route->latencySamples = calibration->latencySamples;
            });
            setStatus("Measured reamp round-trip latency: "
                          + juce::String(calibration->latencySamples)
                          + " samples.");
        }
    }

    if (!activeRecordingTargets.empty()
        && !audioEngine.isPlaying()
        && !recordingFinalizationInProgress)
    {
        finishRecording();
        return;
    }

    const auto position = audioEngine.positionSeconds();
    timeline.setPlayheadSeconds(position);
    positionLabel.setText(positionText(position, project),
                          juce::dontSendNotification);
    const auto playing = audioEngine.isPlaying();
    playButton.setButtonText(playing ? "PAUSE" : "PLAY");
    const auto recording = !activeRecordingTargets.empty();
    recordButton.setButtonText(recording ? "STOP REC" : "REC");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           juce::Colour(recording ? StudioColours::orange
                                                 : StudioColours::raised));
    mixer->setPeaks(audioEngine.leftPeak(), audioEngine.rightPeak());
    mixer->setMeters(audioEngine.trackMeterSnapshots());
    auto runtimeStatuses = audioEngine.pluginRuntimeStatuses();
    auto runtimeMetadataChanged = false;
    for (const auto& status : runtimeStatuses)
    {
        for (auto& track : project.tracks)
        {
            const auto insert = std::find_if(track.inserts.begin(),
                                             track.inserts.end(),
                                             [&status](const auto& candidate)
            {
                return candidate.id == status.insertId;
            });
            if (insert == track.inserts.end())
                continue;

            if (status.state
                    == StudioAudioEngine::PluginRuntimeStatus::State::ready
                && (insert->latencySamples != status.latencySamples
                    || std::abs(insert->tailSeconds - status.tailSeconds)
                        > 0.000001))
            {
                insert->latencySamples = status.latencySamples;
                insert->tailSeconds = status.tailSeconds;
                runtimeMetadataChanged = true;
            }
            if (status.state
                == StudioAudioEngine::PluginRuntimeStatus::State::ready)
            {
                pluginCatalog.recordRuntimeReady(*insert);
            }
            else if (status.state
                     == StudioAudioEngine::PluginRuntimeStatus::State::missing)
            {
                pluginCatalog.recordRuntimeFailure(
                    *insert,
                    PluginFailureKind::missing,
                    status.message);
            }
            else if (status.state
                     == StudioAudioEngine::PluginRuntimeStatus::State::failed)
            {
                const auto failure =
                    status.message.containsIgnoreCase("timeout")
                        || status.message.containsIgnoreCase("respond")
                    ? PluginFailureKind::timeout
                    : PluginFailureKind::runtimeCrash;
                pluginCatalog.recordRuntimeFailure(
                    *insert,
                    failure,
                    status.message);
            }
            break;
        }
    }
    if (runtimeMetadataChanged)
        projectChanged();

    insertPanel->setRuntimeStatuses(std::move(runtimeStatuses),
                                    audioEngine.pluginLateBlockCount());
    if (recording)
    {
        auto progress = audioEngine.recordingProgress();
        std::vector<TimelineComponent::RecordingPreview> previews;
        previews.reserve(activeRecordingTargets.size());
        for (std::size_t index = 0; index < activeRecordingTargets.size(); ++index)
        {
            TimelineComponent::RecordingPreview preview;
            preview.trackId = activeRecordingTargets[index].parentTrackId;
            preview.startSeconds = recordingStartSeconds;
            if (index < progress.size())
            {
                preview.durationSeconds = progress[index].durationSeconds;
                preview.waveformPeaks = std::move(progress[index].waveform);
            }
            previews.push_back(std::move(preview));
        }
        const auto duration = progress.empty() ? 0.0 : progress.front().durationSeconds;
        timeline.setRecordingPreviews(std::move(previews));
        setStatus("Recording "
                      + juce::String(static_cast<int>(activeRecordingTargets.size()))
                      + (activeRecordingTargets.size() == 1 ? " track, " : " tracks, ")
                      + juce::String(duration, 1)
                      + " s. Press STOP REC or STOP to finish.");
        updateTimelineSize();
    }

    auto viewportX = timelineViewport.getViewPositionX();
    if (playing || recording)
    {
        const auto playheadX = static_cast<int>(timeline.xForSeconds(position));
        const auto headerWidth = static_cast<int>(timeline.xForSeconds(0.0));
        const auto visibleLeft = viewportX + headerWidth + 48;
        const auto visibleRight = viewportX + timelineViewport.getWidth() - 96;
        if (playheadX > visibleRight)
            viewportX = juce::jmax(0,
                                   playheadX
                                       - static_cast<int>(timelineViewport.getWidth() * 0.68));
        else if (playheadX < visibleLeft)
            viewportX = juce::jmax(0, playheadX - headerWidth - 48);
        timelineViewport.setViewPosition(viewportX,
                                         timelineViewport.getViewPositionY());
        viewportX = timelineViewport.getViewPositionX();
    }
    timeline.setViewportPosition(viewportX);

    if (playAfterRuntimeTransition
        && !audioEngine.pluginRuntimeTransitionPending()
        && !recordingFinalizationInProgress)
    {
        playAfterRuntimeTransition = false;
        audioEngine.play();
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    juce::AudioDeviceManager::AudioDeviceSetup audioSetup;
    deviceManager.getAudioDeviceSetup(audioSetup);
    const auto signature = device != nullptr
        ? audioSetup.inputDeviceName
            + ":"
            + device->getInputChannelNames().joinIntoString("|")
            + ":"
            + device->getActiveInputChannels().toString(16)
            + ":"
            + juce::String(device->getCurrentSampleRate(), 1)
            + ":"
            + juce::String(device->getCurrentBufferSizeSamples())
        : juce::String();
    if (signature != inputConfigurationSignature)
    {
        inputConfigurationSignature = signature;
        refreshInputControls();
        if (const auto result = audioEngine.updateProject(project,
                                                          pluginRuntimeRequests()); result.failed())
            setStatus(result.getErrorMessage(), true);
    }

    const auto catalogRevision = pluginCatalog.revision();
    if (!pluginCatalog.isScanning() && catalogRevision != lastRuntimeCatalogRevision)
    {
        lastRuntimeCatalogRevision = catalogRevision;
        if (const auto result = audioEngine.updateProject(project,
                                                          pluginRuntimeRequests()); result.failed())
            setStatus(result.getErrorMessage(), true);
    }
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    return keyPressed(key);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    const auto command = key.getModifiers().isCommandDown();
    const auto shift = key.getModifiers().isShiftDown();

    if (key == juce::KeyPress::spaceKey)
    {
        togglePlayback();
        return true;
    }

    if (command && key.getKeyCode() == 'S')
    {
        beginSaveProject();
        return true;
    }

    if (command && key.getKeyCode() == 'O')
    {
        beginOpenProject();
        return true;
    }

    if (command && key.getKeyCode() == 'I')
    {
        beginImportAudio();
        return true;
    }

    if (command && key.getKeyCode() == 'Z')
    {
        shift ? redo() : undo();
        return true;
    }

    if (command && key.getKeyCode() == '-')
    {
        zoomTimeline(1.0 / 1.25);
        return true;
    }

    if (command && (key.getKeyCode() == '+' || key.getKeyCode() == '='))
    {
        zoomTimeline(1.25);
        return true;
    }

    if (command && key.getKeyCode() == '0')
    {
        zoomTimeline(1.0, true);
        return true;
    }

    if (!command && key.getKeyCode() == 'S')
    {
        splitSelectedClip();
        return true;
    }

    if (!command && key.getKeyCode() == '[')
    {
        trimSelectedClipStartToPlayhead();
        return true;
    }

    if (!command && key.getKeyCode() == ']')
    {
        trimSelectedClipEndToPlayhead();
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::deleteKey
        || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        if (selectedClipId.isNotEmpty())
            deleteSelectedClip();
        else
            deleteSelectedTrack();
        return true;
    }

    return false;
}

void MainComponent::createNewProject()
{
    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize the current recording before creating a project.", true);
        return;
    }

    audioEngine.stop();
    if (projectPackage.exists())
        ProjectFile::clearReducedIsolationMarker(projectPackage);
    project = Project::createDefault();
    projectPackage = juce::File();
    reducedIsolationMarkerSignature.clear();
    commandStack.clear();
    selectedClipId.clear();
    selectedTrackId = project.tracks.front().id;
    tempoSlider.setValue(project.tempo, juce::dontSendNotification);
    loopButton.setToggleState(project.loopEnabled, juce::dontSendNotification);
    dirty = false;
    selectTrack(selectedTrackId);
    projectChanged(false, false);
    setStatus("New project created.");
}

void MainComponent::beginOpenProject()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Open Studio Duo project",
        projectPackage.exists() ? projectPackage.getParentDirectory()
                                : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.studioduo",
        true,
        true,
        this);

    const auto flags = juce::FileBrowserComponent::openMode
        | juce::FileBrowserComponent::canSelectFiles
        | juce::FileBrowserComponent::canSelectDirectories;
    fileChooser->launchAsync(flags, [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
    {
        if (safe == nullptr)
            return;

        const auto result = chooser.getResult();
        if (result.exists())
            safe->openProjectFrom(result);
        safe->fileChooser.reset();
    });
}

void MainComponent::beginSaveProject()
{
    if (projectPackage.exists())
    {
        saveProjectTo(projectPackage);
        return;
    }

    const auto initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile(project.name + ".studioduo");
    fileChooser = std::make_unique<juce::FileChooser>("Save Studio Duo project",
                                                      initial,
                                                      "*.studioduo",
                                                      true,
                                                      true,
                                                      this);
    const auto flags = juce::FileBrowserComponent::saveMode
        | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync(flags, [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
    {
        if (safe == nullptr)
            return;

        const auto result = chooser.getResult();
        if (result != juce::File())
            safe->saveProjectTo(result);
        safe->fileChooser.reset();
    });
}

void MainComponent::beginImportAudio()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Import audio",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.wav;*.wave;*.aif;*.aiff;*.flac;*.mp3",
        true,
        false,
        this);
    const auto flags = juce::FileBrowserComponent::openMode
        | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync(flags, [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
    {
        if (safe == nullptr)
            return;

        const auto result = chooser.getResult();
        if (result.existsAsFile())
            safe->importAudioFile(result);
        safe->fileChooser.reset();
    });
}

void MainComponent::beginExportMix()
{
    const auto initial = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
        .getChildFile(project.name + "-mix.wav");
    fileChooser = std::make_unique<juce::FileChooser>("Export stereo mix",
                                                      initial,
                                                      "*.wav",
                                                      true,
                                                      false,
                                                      this);
    const auto flags = juce::FileBrowserComponent::saveMode
        | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync(flags, [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
    {
        if (safe == nullptr)
            return;

        const auto result = chooser.getResult();
        if (result != juce::File())
            safe->exportMixTo(result.hasFileExtension("wav")
                                  ? result
                                  : result.withFileExtension("wav"));
        safe->fileChooser.reset();
    });
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        deviceManager,
        0,
        2,
        0,
        2,
        true,
        true,
        true,
        false);
    selector->setSize(560, 460);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle = "Studio Duo audio I/O";
    options.dialogBackgroundColour = juce::Colour(StudioColours::panel);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void MainComponent::saveProjectTo(const juce::File& package)
{
    const auto normalised = ProjectFile::normalisePackagePath(package);
    if (project.name == "Untitled")
        project.name = normalised.getFileNameWithoutExtension();

    const auto resumePlayback = audioEngine.isPlaying();
    if (resumePlayback)
        audioEngine.pause();
    auto stateWarning = juce::String();
    for (auto& capture : audioEngine.capturePluginStates(2000))
    {
        if (capture.insertId.isEmpty())
        {
            if (resumePlayback)
                audioEngine.play();
            showError("Project save failed",
                      capture.result.getErrorMessage());
            return;
        }
        auto* track = project.findTrack(capture.trackId);
        if (track == nullptr)
            continue;
        const auto insert = std::find_if(
            track->inserts.begin(),
            track->inserts.end(),
            [&capture](const auto& candidate)
            {
                return candidate.id == capture.insertId;
            });
        if (insert == track->inserts.end())
            continue;
        if (capture.result.failed())
        {
            stateWarning = capture.name
                + ": "
                + capture.result.getErrorMessage();
            continue;
        }
        if (capture.state.isEmpty())
            continue;

        juce::String stateError;
        const auto reference = PluginStateStore::store(
            normalised,
            capture.state,
            stateError);
        if (!reference.has_value())
        {
            if (resumePlayback)
                audioEngine.play();
            showError("Project save failed", stateError);
            return;
        }
        insert->stateFile = reference->relativePath;
        insert->stateHash = reference->hash;
    }

    const auto result = ProjectFile::save(project, normalised);
    if (result.failed())
    {
        if (resumePlayback)
            audioEngine.play();
        showError("Project save failed", result.getErrorMessage());
        return;
    }

    projectPackage = normalised;
    reducedIsolationMarkerSignature.clear();
    updateReducedIsolationMarker();
    dirty = false;
    projectLabel.setText(project.name, juce::dontSendNotification);
    if (resumePlayback)
        audioEngine.play();
    setStatus(
        "Saved "
            + projectPackage.getFullPathName()
            + (stateWarning.isNotEmpty()
                   ? " (preserved prior state: " + stateWarning + ")"
                   : juce::String()));
}

void MainComponent::openProjectFrom(const juce::File& package)
{
    juce::String error;
    auto loaded = ProjectFile::load(package, error);
    if (!loaded.has_value())
    {
        showError("Project open failed", error);
        return;
    }

    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize the current recording before opening a project.", true);
        return;
    }
    audioEngine.stop();
    projectPackage = ProjectFile::normalisePackagePath(package);
    reducedIsolationMarkerSignature.clear();
    const auto recoveryInsertIds =
        ProjectFile::reducedIsolationMarker(projectPackage);
    auto recoveredInProcess = false;
    for (auto& track : loaded->tracks)
    {
        for (auto& insert : track.inserts)
        {
            if (insert.bridgeMode != PluginBridgeMode::sandboxed
                && std::find(recoveryInsertIds.cbegin(),
                             recoveryInsertIds.cend(),
                             insert.id) != recoveryInsertIds.cend())
            {
                insert.recoveryDisabled = true;
                recoveredInProcess = true;
            }
        }
    }
    project = std::move(*loaded);
    commandStack.clear();
    selectedClipId.clear();
    selectedTrackId = project.tracks.empty() ? juce::String() : project.tracks.front().id;
    tempoSlider.setValue(project.tempo, juce::dontSendNotification);
    loopButton.setToggleState(project.loopEnabled, juce::dontSendNotification);
    dirty = false;
    selectTrack(selectedTrackId);
    projectChanged(false, false);
    setStatus(
        recoveredInProcess
            ? "Opened recovery-safe: in-process plugins are disabled until explicitly reloaded."
            : "Opened " + projectPackage.getFullPathName(),
        recoveredInProcess);
}

void MainComponent::importAudioFile(const juce::File& source)
{
    juce::String error;
    const auto duration = audioEngine.audioFileDuration(source, error);
    if (!duration.has_value())
    {
        showError("Audio import failed", error);
        return;
    }

    auto* destination = project.findTrack(selectedTrackId);
    if (destination == nullptr || destination->type != TrackType::audio)
        destination = recordingTrack();
    if (destination == nullptr)
    {
        showError("Audio import failed", "Add an audio track before importing audio.");
        return;
    }

    AudioClip clip;
    clip.name = source.getFileNameWithoutExtension();
    clip.sourceFile = source;
    clip.startSeconds = audioEngine.positionSeconds();
    clip.durationSeconds = *duration;
    clip.sourceLengthSeconds = *duration;
    clip.sourceRangeEndSeconds = *duration;
    clip.colour = destination->colour;

    const auto clipId = clip.id;
    if (perform(std::make_unique<AddClipCommand>(destination->id, clip)))
        selectClip(destination->id, clipId);
}

void MainComponent::exportMixTo(const juce::File& destination)
{
    setStatus("Rendering " + destination.getFileName() + "...");
    auto exportProject = project;
    for (auto& track : exportProject.tracks)
        for (auto& insert : track.inserts)
        {
            insert.missing = !pluginCatalog.descriptionForIdentifier(insert.pluginIdentifier).has_value();
            if (!insert.missing && insert.stateFile.isNotEmpty())
            {
                juce::MemoryBlock state;
                juce::String stateError;
                insert.missing = !projectPackage.exists()
                    || !PluginStateStore::load(
                        projectPackage,
                        { insert.stateFile, insert.stateHash },
                        state,
                        stateError);
            }
        }

    const auto result = audioEngine.renderToWav(exportProject, destination, 48000.0);
    if (result.failed())
    {
        showError("Export failed", result.getErrorMessage());
        return;
    }

    setStatus("Exported 48 kHz / 24-bit WAV to " + destination.getFullPathName());
}

void MainComponent::togglePlayback()
{
    if (recordingFinalizationInProgress)
    {
        setStatus("Wait for the recorded WAVs to finish saving.", true);
        return;
    }

    if (!activeRecordingTargets.empty())
    {
        stopTransportAndRecording();
        return;
    }

    if (audioEngine.isPlaying())
    {
        audioEngine.pause();
        return;
    }

    if (audioEngine.positionSeconds() >= project.lengthSeconds() - 0.001)
    {
        audioEngine.seekSeconds(0.0);
        timeline.setPlayheadSeconds(0.0);
        timelineViewport.setViewPosition(0, timelineViewport.getViewPositionY());
        timeline.setViewportPosition(0);
        if (project.hasActivePluginInserts())
        {
            playAfterRuntimeTransition = true;
            audioEngine.forcePluginRuntimeReload(project, pluginRuntimeRequests());
            setStatus("Rewinding and resetting sandbox pipelines...");
            return;
        }
    }

    if (audioEngine.pluginRuntimeTransitionPending())
    {
        playAfterRuntimeTransition = true;
        setStatus("Waiting for sandboxed plugins to become ready.", true);
        return;
    }

    audioEngine.play();
}

void MainComponent::toggleRecording()
{
    if (recordingFinalizationInProgress)
    {
        setStatus("Wait for the previous WAV to finish saving.", true);
        return;
    }

    if (!activeRecordingTargets.empty())
    {
        finishRecording();
        return;
    }

    auto parentIds = project.armedAudioParentTrackIds();
    if (parentIds.empty())
    {
        const auto* selected = project.findTrack(selectedTrackId);
        if (selected != nullptr && selected->type == TrackType::audio)
        {
            const auto parentId = selected->parentTrackId.isNotEmpty()
                ? selected->parentTrackId
                : selected->id;
            if (project.findTrack(parentId) != nullptr)
                parentIds.push_back(parentId);
        }
    }

    if (parentIds.empty())
    {
        showError("Recording unavailable", "Select an audio track or add a new one first.");
        return;
    }

    auto folder = projectPackage.exists()
        ? projectPackage.getChildFile("media")
        : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
              .getChildFile("Studio Duo Recordings");
    if (const auto folderResult = folder.createDirectory(); folderResult.failed())
    {
        showError("Recording unavailable", folderResult.getErrorMessage());
        return;
    }

    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    std::vector<ActiveRecordingTarget> targets;
    std::vector<StudioAudioEngine::RecordingRequest> requests;
    targets.reserve(parentIds.size());
    requests.reserve(parentIds.size());
    for (std::size_t index = 0; index < parentIds.size(); ++index)
    {
        const auto* parent = project.findTrack(parentIds[index]);
        if (parent == nullptr)
        {
            showError("Recording unavailable", "An armed recording track is no longer available.");
            return;
        }

        ActiveRecordingTarget target;
        target.parentTrackId = parent->id;
        target.versionTrack = makeRecordingVersionTrack(*parent);
        const auto* reampRoute = project.reampRouteForReturn(parent->id);
        const auto captureInput = reampRoute != nullptr
                && reampRoute->enabled
                && reampRoute->type == TonePathType::hardware
            ? reampRoute->inputChannel
            : parent->inputChannel;
        target.versionTrack.inputChannel = captureInput;
        const auto legalName = juce::File::createLegalFileName(parent->name);
        target.file = folder.getNonexistentChildFile(
            "Recording-"
                + timestamp
                + "-"
                + juce::String(static_cast<int>(index + 1))
                + "-"
                + legalName,
            ".wav",
            false);
        requests.push_back({
            target.file,
            captureInput,
            parent->stereoInput ? 2 : 1
        });
        targets.push_back(std::move(target));
    }

    activeRecordingPlan = project.recordingPlan(audioEngine.positionSeconds());
    recordingStartSeconds = activeRecordingPlan.captureStartSeconds;
    const auto result = audioEngine.startRecording(requests, activeRecordingPlan);
    if (result.failed())
    {
        showError("Recording unavailable", result.getErrorMessage());
        return;
    }

    activeRecordingTargets = std::move(targets);
    std::vector<TimelineComponent::RecordingPreview> previews;
    previews.reserve(activeRecordingTargets.size());
    for (const auto& target : activeRecordingTargets)
        previews.push_back({ target.parentTrackId, recordingStartSeconds, 0.0, { 0.0f } });
    timeline.setRecordingPreviews(std::move(previews));
    setStatus("Recording "
                  + juce::String(static_cast<int>(activeRecordingTargets.size()))
                  + (activeRecordingTargets.size() == 1 ? " track." : " synchronized tracks.")
                  + " Press REC or STOP to finish.");
}

void MainComponent::stopTransportAndRecording()
{
    playAfterRuntimeTransition = false;
    if (!activeRecordingTargets.empty() || audioEngine.isRecording())
        finishRecording();
    else
        audioEngine.stop();

    recordButton.setButtonText("REC");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           juce::Colour(StudioColours::raised));
    timeline.clearRecordingPreviews();
    if (project.hasActivePluginInserts() && !recordingFinalizationInProgress)
        audioEngine.forcePluginRuntimeReload(project, pluginRuntimeRequests());
}

void MainComponent::finishRecording()
{
    if (recordingFinalizationInProgress)
        return;

    if (activeRecordingTargets.empty() && !audioEngine.isRecording())
        return;

    auto pendingTargets = std::move(activeRecordingTargets);
    activeRecordingTargets.clear();
    recordingFinalizationInProgress = true;
    recordButton.setButtonText("REC");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           juce::Colour(StudioColours::raised));
    timeline.clearRecordingPreviews();
    setStatus("Stopping capture and finalizing "
                  + juce::String(static_cast<int>(pendingTargets.size()))
                  + (pendingTargets.size() == 1 ? " WAV..." : " synchronized WAVs..."));

    audioEngine.stopRecordingAsync(
        [safe = juce::Component::SafePointer<MainComponent>(this),
         targets = std::move(pendingTargets)](auto recordings) mutable
        {
            if (safe != nullptr)
                safe->completeRecording(std::move(targets), std::move(recordings));
        });
}

void MainComponent::completeRecording(
    std::vector<ActiveRecordingTarget> targets,
    std::vector<StudioAudioEngine::RecordingResult> recordings)
{
    recordingFinalizationInProgress = false;
    if (targets.size() != recordings.size() || recordings.empty())
    {
        showError("Recording failed", "The recording engine returned an incomplete multitrack take.");
        return;
    }

    const auto expectedDuration = recordings.front().durationSeconds;
    const auto durationTolerance = 1.0 / audioEngine.currentSampleRate();
    juce::String warning;
    std::vector<Track> completedTracks;
    juce::String firstClipId;
    for (std::size_t index = 0; index < targets.size(); ++index)
    {
        auto& target = targets[index];
        auto& recording = recordings[index];
        const auto* parent = project.findTrack(target.parentTrackId);
        if (recording.result.failed())
        {
            showError("Recording failed",
                      recording.result.getErrorMessage()
                          + " No take lanes were added; captured files remain in "
                          + target.file.getParentDirectory().getFullPathName());
            return;
        }
        if (parent == nullptr
            || recording.durationSeconds <= 0.0
            || !recording.file.existsAsFile()
            || recording.file.getSize() <= 44)
        {
            showError("Recording failed",
                      "A synchronized WAV is missing or empty. No take lanes were added; "
                      "captured files remain in "
                          + target.file.getParentDirectory().getFullPathName());
            return;
        }
        if (std::abs(recording.durationSeconds - expectedDuration) > durationTolerance)
        {
            showError("Recording failed",
                      "The captured WAV lengths do not match. No take lanes were added; "
                      "captured files remain in "
                          + target.file.getParentDirectory().getFullPathName());
            return;
        }

        const auto passes = recordingPasses(recording.durationSeconds,
                                            activeRecordingPlan);
        const auto* reampRoute = project.reampRouteForReturn(
            target.parentTrackId);
        const auto hardwareReturn = reampRoute != nullptr
            && reampRoute->enabled
            && reampRoute->type == TonePathType::hardware;
        const auto alignmentSeconds = hardwareReturn
            ? static_cast<double>(reampRoute->latencySamples
                                  + reampRoute->alignmentOffsetSamples)
                / audioEngine.currentSampleRate()
            : 0.0;
        for (std::size_t passIndex = 0; passIndex < passes.size(); ++passIndex)
        {
            auto take = target.versionTrack;
            if (passIndex > 0)
            {
                take.id = juce::Uuid().toString();
                take.versionNumber += static_cast<int>(passIndex);
                take.name = "v" + juce::String(take.versionNumber);
                take.clips.clear();
            }

            const auto& pass = passes[passIndex];
            AudioClip clip;
            clip.name = recording.file.getFileNameWithoutExtension()
                + (passes.size() > 1
                       ? " pass " + juce::String(static_cast<int>(passIndex + 1))
                       : juce::String());
            clip.sourceFile = recording.file;
            clip.startSeconds = std::max(0.0,
                                         pass.timelineStartSeconds
                                             - alignmentSeconds);
            clip.sourceOffsetSeconds = pass.sourceOffsetSeconds;
            clip.durationSeconds = pass.durationSeconds;
            clip.sourceLengthSeconds = recording.durationSeconds;
            clip.sourceRangeStartSeconds = pass.sourceOffsetSeconds;
            clip.sourceRangeEndSeconds = pass.sourceOffsetSeconds
                + pass.durationSeconds;
            clip.polarityInverted = hardwareReturn
                && reampRoute->polarityInverted;
            clip.colour = take.colour;
            if (firstClipId.isEmpty())
                firstClipId = clip.id;
            take.clips.push_back(std::move(clip));
            completedTracks.push_back(std::move(take));
        }

        if (warning.isEmpty() && recording.warning.isNotEmpty())
            warning = recording.warning;
    }

    const auto firstTrackId = completedTracks.front().id;
    const auto completedTakeCount = completedTracks.size();
    if (!perform(std::make_unique<AddRecordingTakeCommand>(std::move(completedTracks))))
        return;
    selectClip(firstTrackId, firstClipId);
    const auto savedMessage = "Saved "
        + juce::String(static_cast<int>(completedTakeCount))
        + (completedTakeCount == 1 ? " take (" : " synchronized takes (")
        + juce::String(expectedDuration, 2)
        + " s).";
    setStatus(warning.isNotEmpty()
                  ? savedMessage + " " + warning
                  : savedMessage,
              warning.isNotEmpty());
    if (project.hasActivePluginInserts())
        audioEngine.forcePluginRuntimeReload(project, pluginRuntimeRequests());
}

void MainComponent::addAudioTrack()
{
    addTrack(TrackType::audio);
}

void MainComponent::addBusTrack()
{
    addTrack(TrackType::bus);
}

void MainComponent::addTrack(TrackType type)
{
    if (type == TrackType::controlRoom
        && std::any_of(
            project.tracks.cbegin(),
            project.tracks.cend(),
            [](const auto& track)
            {
                return track.type == TrackType::controlRoom;
            }))
    {
        setStatus("The project already has a control-room track.", true);
        return;
    }

    Track track;
    track.type = type;
    const auto trackCount = static_cast<int>(std::count_if(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [type](const auto& candidate)
    {
        return candidate.type == type;
    }));
    const auto typeName = [&]
    {
        switch (type)
        {
            case TrackType::audio: return juce::String("Audio");
            case TrackType::instrument: return juce::String("Instrument");
            case TrackType::midi: return juce::String("MIDI");
            case TrackType::aux: return juce::String("Aux");
            case TrackType::bus: return juce::String("Bus");
            case TrackType::folder: return juce::String("Folder");
            case TrackType::vca: return juce::String("VCA");
            case TrackType::controlRoom: return juce::String("Control Room");
            case TrackType::master: return juce::String("Master");
        }
        return juce::String("Track");
    }();
    track.name = type == TrackType::controlRoom
        ? typeName
        : typeName + " " + juce::String(trackCount + 1);
    track.armed = type == TrackType::audio;
    const std::array colours {
        juce::Colour(0xffdd5b3f),
        juce::Colour(0xffd99a42),
        juce::Colour(0xff78c6a3),
        juce::Colour(0xff7da9d9),
        juce::Colour(0xffb47ac4)
    };
    track.colour = colours[static_cast<std::size_t>(trackCount)
                           % colours.size()];
    const auto trackId = track.id;
    if (type == TrackType::controlRoom)
    {
        RoutingConnection monitorRoute;
        monitorRoute.name = "Master monitor";
        monitorRoute.kind = RouteKind::controlRoom;
        monitorRoute.sourceTrackId = project.masterTrackId();
        monitorRoute.destination.type = RouteEndpointType::track;
        monitorRoute.destination.trackId = trackId;
        std::vector<std::unique_ptr<ProjectCommand>> commands;
        commands.push_back(std::make_unique<AddTrackCommand>(track));
        commands.push_back(std::make_unique<AddRoutingConnectionCommand>(
            std::move(monitorRoute)));
        if (perform(std::make_unique<BatchProjectCommand>(
                "Add control room",
                std::move(commands))))
            selectTrack(trackId);
        return;
    }

    if (perform(std::make_unique<AddTrackCommand>(track)))
        selectTrack(trackId);
}

void MainComponent::showAddTrackMenu()
{
    juce::PopupMenu menu;
    const auto add = [this](TrackType type)
    {
        return [this, type] { addTrack(type); };
    };
    menu.addItem("Audio track", add(TrackType::audio));
    menu.addItem("Aux track", add(TrackType::aux));
    menu.addItem("Bus track", add(TrackType::bus));
    menu.addSeparator();
    menu.addItem("Folder track", add(TrackType::folder));
    menu.addItem("VCA track", add(TrackType::vca));
    const auto hasControlRoom = std::any_of(
        project.tracks.cbegin(),
        project.tracks.cend(),
        [](const auto& track)
        {
            return track.type == TrackType::controlRoom;
        });
    menu.addItem("Control-room track",
                 !hasControlRoom,
                 false,
                 add(TrackType::controlRoom));
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(addTrackButton));
}

void MainComponent::duplicateSelectedTrack()
{
    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize recording before duplicating tracks.", true);
        return;
    }

    const auto* selected = project.findTrack(selectedTrackId);
    if (selected == nullptr || selected->type == TrackType::master)
    {
        setStatus("Select a non-master track to duplicate.", true);
        return;
    }

    auto command = std::make_unique<DuplicateTrackCommand>(selectedTrackId);
    auto* commandPointer = command.get();
    if (perform(std::move(command)))
        selectTrack(commandPointer->duplicatedTrackId());
}

void MainComponent::deleteSelectedTrack()
{
    const auto* selected = project.findTrack(selectedTrackId);
    if (selected == nullptr || selected->type == TrackType::master)
    {
        setStatus("Select a non-master track to delete.", true);
        return;
    }
    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize recording before deleting tracks.", true);
        return;
    }

    const auto trackToDelete = selectedTrackId;
    const auto preferredSelection = selected->parentTrackId;
    if (!perform(std::make_unique<RemoveTrackCommand>(trackToDelete)))
        return;

    if (preferredSelection.isNotEmpty() && project.findTrack(preferredSelection) != nullptr)
    {
        selectTrack(preferredSelection);
        return;
    }

    const auto next = std::find_if(project.tracks.cbegin(), project.tracks.cend(), [](const auto& track)
    {
        return track.type != TrackType::master;
    });
    selectTrack(next != project.tracks.cend() ? next->id
                                              : project.tracks.back().id);
}

void MainComponent::addPluginToSelectedTrack(const PluginCatalogEntry& entry)
{
    const auto* track = project.findTrack(selectedTrackId);
    if (track == nullptr)
    {
        setStatus("Select a track before adding a plugin.", true);
        return;
    }

    PluginInsert insert;
    insert.pluginIdentifier = entry.identifier;
    insert.name = entry.name;
    insert.manufacturer = entry.manufacturer;
    insert.format = entry.format;
    insert.version = entry.version;
    insert.architecture = entry.architecture;
    insert.fileOrIdentifier = entry.fileOrIdentifier;
    insert.araCapable = entry.araCapable;
    insert.bridgeMode = PluginBridgeMode::sandboxed;

    if (replacementInsertId.isNotEmpty())
    {
        const auto replacing = replacementInsertId;
        replacementInsertId.clear();
        if (perform(std::make_unique<ReplacePluginInsertCommand>(
                track->id,
                replacing,
                insert)))
            setStatus(entry.name + " replaced the missing insert.");
        return;
    }

    if (perform(std::make_unique<AddPluginInsertCommand>(track->id, insert)))
        setStatus(entry.name + " added as a sandboxed insert model.");
}

void MainComponent::changePluginMode(const juce::String& trackId,
                                     const juce::String& insertId,
                                     PluginBridgeMode mode)
{
    if (mode != PluginBridgeMode::araCompatibility)
    {
        perform(std::make_unique<SetPluginBridgeModeCommand>(
            trackId,
            insertId,
            mode));
        return;
    }

    if (!projectPackage.exists())
    {
        showError(
            "Save required",
            "Save the project before enabling ARA 2 compatibility mode so Studio Duo can create a recovery point.");
        return;
    }
    if (const auto result = ProjectFile::writeRecoveryPoint(
            project,
            projectPackage);
        result.failed())
    {
        showError("ARA recovery point failed", result.getErrorMessage());
        return;
    }

    auto* dialog = new juce::AlertWindow(
        "Enable ARA 2 compatibility mode?",
        "ARA 2 needs synchronous access to the project document and runs this plugin in the Studio Duo process. Crash isolation is reduced. A recovery point has been written.",
        juce::MessageBoxIconType::WarningIcon);
    dialog->addButton(
        "Enable ARA 2",
        1,
        juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton(
        "Cancel",
        0,
        juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->centreAroundComponent(insertPanel.get(), 520, 230);
    const juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe, trackId, insertId](int result)
            {
                if (safe == nullptr || result != 1)
                    return;
                safe->perform(std::make_unique<SetPluginBridgeModeCommand>(
                    trackId,
                    insertId,
                    PluginBridgeMode::araCompatibility));
            }),
        true);
}

void MainComponent::splitSelectedClip()
{
    if (selectedClipId.isEmpty())
    {
        setStatus("Select a clip before splitting.", true);
        return;
    }

    const auto cursor = audioEngine.positionSeconds();
    const auto clipIds = linkedClipIdsAt(selectedClipId, cursor);
    const auto* group = project.editGroupForTrack(selectedTrackId);
    if (group != nullptr
        && group->enabled
        && clipIds.size() != group->trackIds.size())
    {
        setStatus("Every linked track needs an active clip at the split position.", true);
        return;
    }

    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& clipId : clipIds)
        commands.push_back(std::make_unique<SplitClipCommand>(clipId, cursor));
    perform(std::make_unique<BatchProjectCommand>(
        clipIds.size() > 1 ? "Split linked clips" : "Split clip",
        std::move(commands)));
}

void MainComponent::trimSelectedClipStartToPlayhead()
{
    const auto* clip = project.findClip(selectedClipId);
    if (clip == nullptr)
    {
        setStatus("Select a clip before trimming.", true);
        return;
    }

    const auto cursor = audioEngine.positionSeconds();
    if (cursor <= clip->startSeconds + 0.001 || cursor >= clip->endSeconds() - 0.001)
    {
        setStatus("Place the playhead inside the selected clip before trimming its start.", true);
        return;
    }

    const auto removedDuration = cursor - clip->startSeconds;
    trimClip(clip->id,
             cursor,
             clip->sourceOffsetSeconds + removedDuration,
             clip->durationSeconds - removedDuration);
}

void MainComponent::trimSelectedClipEndToPlayhead()
{
    const auto* clip = project.findClip(selectedClipId);
    if (clip == nullptr)
    {
        setStatus("Select a clip before trimming.", true);
        return;
    }

    const auto cursor = audioEngine.positionSeconds();
    if (cursor <= clip->startSeconds + 0.001 || cursor >= clip->endSeconds() - 0.001)
    {
        setStatus("Place the playhead inside the selected clip before trimming its end.", true);
        return;
    }

    trimClip(clip->id,
             clip->startSeconds,
             clip->sourceOffsetSeconds,
             cursor - clip->startSeconds);
}

void MainComponent::deleteSelectedClip()
{
    if (selectedClipId.isEmpty())
        return;

    const auto* selected = project.findClip(selectedClipId);
    const auto reference = selected != nullptr
        ? selected->startSeconds + selected->durationSeconds * 0.5
        : audioEngine.positionSeconds();
    const auto clipIds = linkedClipIdsAt(selectedClipId, reference);
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& clipId : clipIds)
        commands.push_back(std::make_unique<DeleteClipCommand>(clipId));
    if (perform(std::make_unique<BatchProjectCommand>(
            clipIds.size() > 1 ? "Delete linked clips" : "Delete clip",
            std::move(commands))))
    {
        selectedClipId.clear();
        timeline.setSelection(selectedTrackId, {});
        updateInspector();
    }
}

void MainComponent::moveClip(const juce::String& clipId,
                             const juce::String& destinationTrackId,
                             double startSeconds)
{
    const auto* clip = project.findClip(clipId);
    const auto* sourceTrack = project.findTrackContainingClip(clipId);
    if (clip == nullptr || sourceTrack == nullptr)
        return;

    const auto* group = project.editGroupForTrack(sourceTrack->id);
    if (group != nullptr
        && group->enabled
        && project.rootTrackId(sourceTrack->id)
            != project.rootTrackId(destinationTrackId))
    {
        setStatus("Unlink this track before moving a phase-locked clip to another track.", true);
        return;
    }

    const auto reference = clip->startSeconds + clip->durationSeconds * 0.5;
    const auto linkedIds = linkedClipIdsAt(clipId, reference);
    if (group != nullptr
        && group->enabled
        && linkedIds.size() != group->trackIds.size())
    {
        setStatus("Every linked track needs an active clip for a phase-locked move.", true);
        return;
    }

    const auto delta = startSeconds - clip->startSeconds;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& linkedId : linkedIds)
    {
        const auto* linkedClip = project.findClip(linkedId);
        const auto* linkedTrack = project.findTrackContainingClip(linkedId);
        if (linkedClip == nullptr || linkedTrack == nullptr)
            continue;
        commands.push_back(std::make_unique<MoveClipCommand>(
            linkedId,
            std::max(0.0, linkedClip->startSeconds + delta),
            linkedId == clipId ? destinationTrackId : linkedTrack->id));
    }
    if (perform(std::make_unique<BatchProjectCommand>(
            linkedIds.size() > 1 ? "Move linked clips" : "Move clip",
            std::move(commands))))
        selectClip(destinationTrackId, clipId);
}

void MainComponent::trimClip(const juce::String& clipId,
                             double startSeconds,
                             double sourceOffsetSeconds,
                             double durationSeconds)
{
    const auto* clip = project.findClip(clipId);
    if (clip == nullptr)
        return;

    const auto reference = clip->startSeconds + clip->durationSeconds * 0.5;
    const auto linkedIds = linkedClipIdsAt(clipId, reference);
    const auto* group = project.editGroupForTrack(
        project.findTrackContainingClip(clipId)->id);
    if (group != nullptr && linkedIds.size() != group->trackIds.size())
    {
        setStatus("Every linked track needs an active clip for a phase-locked trim.", true);
        return;
    }

    const auto startDelta = startSeconds - clip->startSeconds;
    const auto sourceDelta = sourceOffsetSeconds - clip->sourceOffsetSeconds;
    const auto durationDelta = durationSeconds - clip->durationSeconds;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& linkedId : linkedIds)
    {
        const auto* linkedClip = project.findClip(linkedId);
        if (linkedClip == nullptr)
            continue;

        if (std::abs(startDelta) > 0.0001)
        {
            commands.push_back(std::make_unique<TrimClipCommand>(
                linkedId,
                linkedClip->startSeconds + startDelta,
                linkedClip->sourceOffsetSeconds + sourceDelta,
                linkedClip->durationSeconds - startDelta));
        }
        else
        {
            commands.push_back(std::make_unique<TrimClipCommand>(
                linkedId,
                linkedClip->startSeconds,
                linkedClip->sourceOffsetSeconds,
                linkedClip->durationSeconds + durationDelta));
        }
    }
    perform(std::make_unique<BatchProjectCommand>(
        linkedIds.size() > 1 ? "Trim linked clips" : "Trim clip",
        std::move(commands)));
}

void MainComponent::quantizeSelectedGroup()
{
    const auto* clip = project.findClip(selectedClipId);
    const auto* track = project.findTrackContainingClip(selectedClipId);
    const auto* group = track != nullptr ? project.editGroupForTrack(track->id) : nullptr;
    if (clip == nullptr || track == nullptr || group == nullptr || !group->enabled)
    {
        setStatus("Select a clip on a linked track before quantizing.", true);
        return;
    }
    if (std::any_of(group->protectedAnchorsSeconds.cbegin(),
                    group->protectedAnchorsSeconds.cend(),
                    [clip](double anchor)
                    {
                        return anchor >= clip->startSeconds
                            && anchor <= clip->endSeconds();
                    }))
    {
        setStatus("This edit is protected by an anchor inside the selected clip.", true);
        return;
    }

    const auto referenceTime = clip->startSeconds + clip->durationSeconds * 0.5;
    const auto* timingClip = activeClipAt(group->timingReferenceTrackId,
                                          referenceTime);
    auto timingEventSeconds = timingClip != nullptr
        ? timingClip->startSeconds
        : clip->startSeconds;
    if (timingClip != nullptr && !timingClip->transientSourceSeconds.empty())
    {
        const auto expectedSource = timingClip->sourceSecondsAt(
            juce::jlimit(0.0,
                         timingClip->durationSeconds,
                         referenceTime - timingClip->startSeconds));
        const auto transient = std::min_element(
            timingClip->transientSourceSeconds.cbegin(),
            timingClip->transientSourceSeconds.cend(),
            [expectedSource](double left, double right)
            {
                return std::abs(left - expectedSource)
                    < std::abs(right - expectedSource);
            });
        timingEventSeconds = timingClip->startSeconds
            + timingClip->timelineOffsetForSourceSeconds(*transient);
    }

    const auto meter = project.meterAt(timingEventSeconds);
    const auto beatStep = 4.0 / static_cast<double>(meter.denominator);
    const auto currentBeat = project.beatsAt(timingEventSeconds);
    const auto targetBeat = std::round(currentBeat / beatStep) * beatStep;
    const auto targetSeconds = project.secondsAtBeat(targetBeat);
    const auto destination = clip->startSeconds
        + (targetSeconds - timingEventSeconds) * group->quantizeStrength;
    if (std::abs(destination - clip->startSeconds) < 0.0001)
    {
        setStatus("The linked edit is already on the selected grid.");
        return;
    }
    moveClip(clip->id, track->id, destination);
}

void MainComponent::analyseClipTransients(const juce::String& clipId)
{
    updateLinkedClips(
        clipId,
        "Detect transients",
        [this](AudioClip& after, const AudioClip& before, juce::String& error)
        {
            after.transientSourceSeconds = audioEngine.analyseTransients(before,
                                                                         error);
            return error.isEmpty();
        });
}

void MainComponent::setClipStretchMode(const juce::String& clipId,
                                       StretchMode mode)
{
    updateLinkedClips(
        clipId,
        "Change stretch mode",
        [mode](AudioClip& after, const AudioClip&, juce::String&)
        {
            after.stretchMode = mode;
            return true;
        });
}

void MainComponent::setClipPlaybackRate(const juce::String& clipId, double rate)
{
    updateLinkedClips(
        clipId,
        "Change playback rate",
        [rate](AudioClip& after, const AudioClip&, juce::String&)
        {
            after.playbackRate = juce::jlimit(0.25, 4.0, rate);
            return true;
        });
}

void MainComponent::warpClipTransient(const juce::String& clipId,
                                      double timelineSeconds)
{
    const auto* selected = project.findClip(clipId);
    if (selected == nullptr
        || timelineSeconds <= selected->startSeconds
        || timelineSeconds >= selected->endSeconds())
    {
        setStatus("Place the playhead inside the clip before adding a warp marker.", true);
        return;
    }
    const auto timelineOffset = timelineSeconds - selected->startSeconds;
    updateLinkedClips(
        clipId,
        "Warp linked transients",
        [timelineOffset](AudioClip& after,
                         const AudioClip& before,
                         juce::String& error)
        {
            if (before.transientSourceSeconds.empty())
            {
                error = "Detect transients on every linked clip before warping.";
                return false;
            }
            const auto currentSource = before.sourceSecondsAt(timelineOffset);
            const auto nearest = std::min_element(
                before.transientSourceSeconds.cbegin(),
                before.transientSourceSeconds.cend(),
                [currentSource](double left, double right)
                {
                    return std::abs(left - currentSource)
                        < std::abs(right - currentSource);
                });
            after.warpMarkers.erase(
                std::remove_if(after.warpMarkers.begin(),
                               after.warpMarkers.end(),
                               [timelineOffset](const auto& marker)
                               {
                                   return std::abs(marker.timelineOffsetSeconds
                                                   - timelineOffset)
                                       < 0.001;
                               }),
                after.warpMarkers.end());
            after.warpMarkers.push_back({ timelineOffset, *nearest });
            std::stable_sort(after.warpMarkers.begin(),
                             after.warpMarkers.end(),
                             [](const auto& left, const auto& right)
                             {
                                 return left.timelineOffsetSeconds
                                     < right.timelineOffsetSeconds;
                             });
            return true;
        });
}

void MainComponent::setClipFade(const juce::String& clipId,
                                double timelineSeconds,
                                bool fadeIn)
{
    const auto* selected = project.findClip(clipId);
    if (selected == nullptr)
        return;
    const auto offset = juce::jlimit(0.0,
                                     selected->durationSeconds,
                                     timelineSeconds - selected->startSeconds);
    updateLinkedClips(
        clipId,
        fadeIn ? "Set linked fade in" : "Set linked fade out",
        [offset, fadeIn](AudioClip& after,
                         const AudioClip& before,
                         juce::String&)
        {
            if (fadeIn)
                after.fadeInSeconds = juce::jlimit(0.0,
                                                   before.durationSeconds,
                                                   offset);
            else
                after.fadeOutSeconds = juce::jlimit(
                    0.0,
                    before.durationSeconds,
                    before.durationSeconds - offset);
            return true;
        });
}

void MainComponent::createClipCrossfade(const juce::String& clipId)
{
    const auto* selected = project.findClip(clipId);
    if (selected == nullptr)
        return;
    const auto linkedIds = linkedClipIdsAt(
        clipId,
        selected->startSeconds + selected->durationSeconds * 0.5);
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& linkedId : linkedIds)
    {
        const auto* current = project.findClip(linkedId);
        const auto* track = project.findTrackContainingClip(linkedId);
        if (current == nullptr || track == nullptr)
            continue;
        const AudioClip* next = nullptr;
        for (const auto& candidate : track->clips)
        {
            if (candidate.startSeconds <= current->startSeconds)
                continue;
            if (next == nullptr || candidate.startSeconds < next->startSeconds)
                next = &candidate;
        }
        if (next == nullptr)
        {
            setStatus("Crossfades require a following clip on every linked track.", true);
            return;
        }

        const auto existingOverlap = current->endSeconds() - next->startSeconds;
        const auto overlap = existingOverlap > 0.0
            ? existingOverlap
            : std::min({ 0.01,
                         current->durationSeconds * 0.5,
                         next->durationSeconds * 0.5 });
        auto currentAfter = *current;
        currentAfter.fadeOutSeconds = std::max(currentAfter.fadeOutSeconds,
                                               overlap);
        auto nextAfter = *next;
        if (existingOverlap <= 0.0)
            nextAfter.startSeconds = current->endSeconds() - overlap;
        nextAfter.fadeInSeconds = std::max(nextAfter.fadeInSeconds,
                                          overlap);
        commands.push_back(std::make_unique<SetClipStateCommand>(
            track->id,
            *current,
            currentAfter,
            "Create crossfade"));
        commands.push_back(std::make_unique<SetClipStateCommand>(
            track->id,
            *next,
            nextAfter,
            "Create crossfade"));
    }
    perform(std::make_unique<BatchProjectCommand>(
        linkedIds.size() > 1 ? "Create linked crossfades" : "Create crossfade",
        std::move(commands)));
}

void MainComponent::toggleClipPolarity(const juce::String& clipId)
{
    updateLinkedClips(
        clipId,
        "Invert clip polarity",
        [](AudioClip& after, const AudioClip& before, juce::String&)
        {
            after.polarityInverted = !before.polarityInverted;
            return true;
        });
}

void MainComponent::toggleClipReverse(const juce::String& clipId)
{
    updateLinkedClips(
        clipId,
        "Reverse clip",
        [](AudioClip& after, const AudioClip& before, juce::String&)
        {
            after.reversed = !before.reversed;
            return true;
        });
}

void MainComponent::consolidateClip(const juce::String& clipId)
{
    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize recording before consolidating clips.", true);
        return;
    }
    const auto* selected = project.findClip(clipId);
    if (selected == nullptr)
        return;
    const auto linkedIds = linkedClipIdsAt(
        clipId,
        selected->startSeconds + selected->durationSeconds * 0.5);
    auto folder = projectPackage.exists()
        ? projectPackage.getChildFile("media")
        : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
              .getChildFile("Studio Duo Consolidated");
    if (const auto result = folder.createDirectory(); result.failed())
    {
        showError("Consolidation failed", result.getErrorMessage());
        return;
    }

    std::vector<juce::File> createdFiles;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& linkedId : linkedIds)
    {
        const auto* current = project.findClip(linkedId);
        const auto* track = project.findTrackContainingClip(linkedId);
        if (current == nullptr || track == nullptr)
            continue;
        const auto destination = folder.getNonexistentChildFile(
            juce::File::createLegalFileName(current->name) + "-consolidated",
            ".wav",
            false);
        const auto renderResult = audioEngine.renderClipToWav(
            *current,
            destination,
            audioEngine.currentSampleRate());
        if (renderResult.failed())
        {
            for (const auto& file : createdFiles)
                file.deleteFile();
            showError("Consolidation failed", renderResult.getErrorMessage());
            return;
        }
        createdFiles.push_back(destination);

        auto after = *current;
        after.name += " consolidated";
        after.sourceFile = destination;
        after.sourceOffsetSeconds = 0.0;
        after.sourceLengthSeconds = current->durationSeconds;
        after.sourceRangeStartSeconds = 0.0;
        after.sourceRangeEndSeconds = current->durationSeconds;
        after.playbackRate = 1.0;
        after.fadeInSeconds = 0.0;
        after.fadeOutSeconds = 0.0;
        after.polarityInverted = false;
        after.reversed = false;
        after.warpMarkers.clear();
        after.transientSourceSeconds.clear();
        after.gainDecibels = 0.0f;
        commands.push_back(std::make_unique<SetClipStateCommand>(
            track->id,
            *current,
            after,
            "Consolidate clip"));
    }
    if (!perform(std::make_unique<BatchProjectCommand>(
            linkedIds.size() > 1 ? "Consolidate linked clips" : "Consolidate clip",
            std::move(commands))))
    {
        for (const auto& file : createdFiles)
            file.deleteFile();
    }
}

void MainComponent::undo()
{
    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize recording before undoing edits.", true);
        return;
    }

    if (!commandStack.undo(project))
        return;

    selectedClipId.clear();
    projectChanged();
}

void MainComponent::redo()
{
    if (!activeRecordingTargets.empty() || recordingFinalizationInProgress)
    {
        setStatus("Stop and finalize recording before redoing edits.", true);
        return;
    }

    juce::String error;
    if (!commandStack.redo(project, error))
    {
        if (error.isNotEmpty())
            showError("Redo failed", error);
        return;
    }

    selectedClipId.clear();
    projectChanged();
}

void MainComponent::selectTrack(const juce::String& trackId)
{
    if (trackId != selectedTrackId)
        replacementInsertId.clear();
    selectedTrackId = trackId;
    selectedClipId.clear();
    timeline.setSelection(selectedTrackId, selectedClipId);
    mixer->setSelection(selectedTrackId);
    routingPanel->setTrack(selectedTrackId);
    insertPanel->setTrack(selectedTrackId);
    updateInspector();
}

void MainComponent::selectClip(const juce::String& trackId, const juce::String& clipId)
{
    selectedTrackId = trackId;
    selectedClipId = clipId;
    timeline.setSelection(selectedTrackId, selectedClipId);
    mixer->setSelection(selectedTrackId);
    routingPanel->setTrack(selectedTrackId);
    insertPanel->setTrack(selectedTrackId);
    updateInspector();
}

void MainComponent::updateInspector()
{
    const auto* track = project.findTrack(selectedTrackId);
    const auto* clip = project.findClip(selectedClipId);

    if (track == nullptr)
    {
        updatingTrackName = true;
        inspectorName.setText("No selection", juce::dontSendNotification);
        updatingTrackName = false;
        inspectorName.setEditable(false, false, false);
        inspectorName.setTooltip({});
        inspectorDetails.setText({}, juce::dontSendNotification);
        volumeSlider.setEnabled(false);
        panSlider.setEnabled(false);
        muteButton.setEnabled(false);
        soloButton.setEnabled(false);
        armButton.setEnabled(false);
        trackColourButton.setEnabled(false);
        splitClipButton.setEnabled(false);
        deleteClipButton.setEnabled(false);
        trimClipStartButton.setEnabled(false);
        trimClipEndButton.setEnabled(false);
        inputSelector.setEnabled(false);
        stereoInputButton.setEnabled(false);
        monitorButton.setEnabled(false);
        outputSelector.setEnabled(false);
        refreshOutputControls();
        return;
    }

    const auto canRenameTrack = clip == nullptr && track->type != TrackType::master;
    updatingTrackName = true;
    inspectorName.setText(clip != nullptr ? clip->name : track->name, juce::dontSendNotification);
    updatingTrackName = false;
    inspectorName.setEditable(false, canRenameTrack, false);
    inspectorName.setTooltip(canRenameTrack
                                 ? "Double-click to rename this track"
                                 : juce::String());
    auto details = clip != nullptr
        ? juce::String(clip->durationSeconds, 2)
            + " s  |  "
            + clip->sourceFile.getFileName()
        : trackTypeToString(track->type).toUpperCase();
    if (clip == nullptr)
    {
        if (const auto* route = project.reampRouteForReturn(track->id))
        {
            const auto* source = project.findTrack(route->sourceTrackId);
            details << "  |  "
                    << (route->type == TonePathType::hardware
                            ? "REAMP RETURN"
                            : "PLUGIN TONE")
                    << " FROM "
                    << (source != nullptr ? source->name : juce::String("MISSING"));
        }
        const auto toneCount = static_cast<int>(std::count_if(
            project.reampRoutes.cbegin(),
            project.reampRoutes.cend(),
            [track](const auto& route)
            {
                return route.sourceTrackId == track->id;
            }));
        if (toneCount > 0)
            details << "  |  DI SOURCE " << juce::String(toneCount) << " PATHS";
    }
    inspectorDetails.setText(details, juce::dontSendNotification);
    volumeSlider.setEnabled(true);
    panSlider.setEnabled(true);
    muteButton.setEnabled(track->type != TrackType::master || !track->muted);
    soloButton.setEnabled(track->type != TrackType::master);
    armButton.setEnabled(track->type == TrackType::audio);
    trackColourButton.setEnabled(track->type != TrackType::master);
    splitClipButton.setEnabled(clip != nullptr);
    deleteClipButton.setEnabled(clip != nullptr);
    trimClipStartButton.setEnabled(clip != nullptr);
    trimClipEndButton.setEnabled(clip != nullptr);
    inputSelector.setEnabled(track->type == TrackType::audio);
    stereoInputButton.setEnabled(track->type == TrackType::audio
                                 && track->inputChannel + 1 < inputSelector.getNumItems());
    monitorButton.setEnabled(track->type == TrackType::audio);
    refreshOutputControls();
    volumeSlider.setValue(track->volumeDecibels, juce::dontSendNotification);
    panSlider.setValue(track->pan, juce::dontSendNotification);
    muteButton.setColour(juce::TextButton::buttonColourId,
                         juce::Colour(track->muted ? StudioColours::amber : StudioColours::raised));
    soloButton.setColour(juce::TextButton::buttonColourId,
                         juce::Colour(track->solo ? StudioColours::green : StudioColours::raised));
    armButton.setColour(juce::TextButton::buttonColourId,
                        juce::Colour(track->armed ? StudioColours::orange : StudioColours::raised));
    armButton.setButtonText(track->armed ? "ARMED" : "ARM");
    const auto colourButtonBackground = track->type != TrackType::master
        ? track->colour
        : juce::Colour(StudioColours::raised);
    trackColourButton.setColour(juce::TextButton::buttonColourId,
                                colourButtonBackground);
    trackColourButton.setColour(juce::TextButton::textColourOffId,
                                colourButtonBackground.contrasting());
    stereoInputButton.setToggleState(track->stereoInput, juce::dontSendNotification);
    monitorButton.setToggleState(track->inputMonitoring, juce::dontSendNotification);
    monitorButton.setButtonText(track->inputMonitoring ? "MONITOR ON" : "MONITOR");
    updatingInputControls = true;
    inputSelector.setSelectedItemIndex(track->inputChannel, juce::dontSendNotification);
    updatingInputControls = false;
}

void MainComponent::refreshOutputControls()
{
    updatingOutputControls = true;
    outputSelector.clear(juce::dontSendNotification);
    outputTrackIds.clear();

    const auto* track = project.findTrack(selectedTrackId);
    if (track == nullptr)
    {
        outputSelector.addItem("No track selected", 1);
        outputSelector.setSelectedItemIndex(0, juce::dontSendNotification);
        outputSelector.setEnabled(false);
        updatingOutputControls = false;
        return;
    }
    if (track->type == TrackType::master)
    {
        outputSelector.addItem("Hardware output 1-2", 1);
        outputSelector.setSelectedItemIndex(0, juce::dontSendNotification);
        outputSelector.setEnabled(false);
        updatingOutputControls = false;
        return;
    }
    if (track->type == TrackType::controlRoom)
    {
        outputSelector.addItem(
            "Monitor hardware "
                + juce::String(track->hardwareOutputChannel + 1)
                + "-"
                + juce::String(track->hardwareOutputChannel + 2),
            1);
        outputSelector.setSelectedItemIndex(0, juce::dontSendNotification);
        outputSelector.setEnabled(false);
        updatingOutputControls = false;
        return;
    }
    if (track->type == TrackType::folder
        || track->type == TrackType::vca
        || track->type == TrackType::midi)
    {
        outputSelector.addItem("No audio output", 1);
        outputSelector.setSelectedItemIndex(0, juce::dontSendNotification);
        outputSelector.setEnabled(false);
        updatingOutputControls = false;
        return;
    }
    if (track->parentTrackId.isNotEmpty())
    {
        const auto* parent = project.findTrack(track->parentTrackId);
        outputSelector.addItem("Follows "
                                   + (parent != nullptr ? parent->name
                                                        : juce::String("parent")),
                               1);
        outputSelector.setSelectedItemIndex(0, juce::dontSendNotification);
        outputSelector.setEnabled(false);
        updatingOutputControls = false;
        return;
    }

    outputSelector.addItem("Master", 1);
    outputTrackIds.emplace_back();
    auto selectedIndex = 0;
    for (const auto& candidate : project.tracks)
    {
        if (candidate.type != TrackType::bus
            || candidate.parentTrackId.isNotEmpty()
            || candidate.id == track->id)
            continue;

        juce::String error;
        if (!project.validateTrackOutput(track->id, candidate.id, error))
            continue;
        outputSelector.addItem("Bus: " + candidate.name,
                               outputSelector.getNumItems() + 1);
        outputTrackIds.push_back(candidate.id);
        if (project.resolvedOutputTrackId(*track) == candidate.id)
            selectedIndex = static_cast<int>(outputTrackIds.size()) - 1;
    }

    outputSelector.setSelectedItemIndex(selectedIndex,
                                        juce::dontSendNotification);
    outputSelector.setEnabled(true);
    updatingOutputControls = false;
}

void MainComponent::refreshInputControls()
{
    const auto selectedIndex = inputSelector.getSelectedItemIndex();
    updatingInputControls = true;
    inputSelector.clear(juce::dontSendNotification);
    juce::StringArray outputNames;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto names = device->getInputChannelNames();
        juce::AudioDeviceManager::AudioDeviceSetup audioSetup;
        deviceManager.getAudioDeviceSetup(audioSetup);
        const auto configuredInputName = audioSetup.inputDeviceName.trim();
        const auto deviceName = configuredInputName.isNotEmpty()
            ? configuredInputName
            : device->getName().trim();
        for (int index = 0; index < names.size(); ++index)
        {
            auto channelName = names[index].trim();
            if (channelName.isEmpty() || channelName.containsOnly("0123456789"))
                channelName = "Input " + juce::String(index + 1);

            const auto displayName = deviceName.isNotEmpty()
                    && !channelName.containsIgnoreCase(deviceName)
                ? deviceName + " - " + channelName
                : channelName;
            inputSelector.addItem(displayName, index + 1);
        }
        outputNames = device->getOutputChannelNames();
        for (int index = 0; index < outputNames.size(); ++index)
        {
            outputNames.set(
                index,
                outputNames[index].trim().isNotEmpty()
                    ? outputNames[index].trim()
                    : "Output " + juce::String(index + 1));
        }
    }
    routingPanel->setHardwareOutputs(std::move(outputNames));

    if (inputSelector.getNumItems() == 0)
        inputSelector.addItem("No active input", 1);

    const auto* track = project.findTrack(selectedTrackId);
    const auto requested = track != nullptr ? track->inputChannel : selectedIndex;
    inputSelector.setSelectedItemIndex(juce::jlimit(0,
                                                   inputSelector.getNumItems() - 1,
                                                   requested),
                                       juce::dontSendNotification);
    updatingInputControls = false;
    updateInspector();
}

void MainComponent::showTrackColourMenu()
{
    const auto* track = project.findTrack(selectedTrackId);
    if (track == nullptr || track->type == TrackType::master)
        return;

    struct ColourChoice
    {
        const char* name;
        juce::Colour colour;
    };
    const std::array choices {
        ColourChoice { "Ember", juce::Colour(0xffdd5b3f) },
        ColourChoice { "Amber", juce::Colour(0xffd99a42) },
        ColourChoice { "Mint", juce::Colour(0xff78c6a3) },
        ColourChoice { "Steel", juce::Colour(0xff7da9d9) },
        ColourChoice { "Violet", juce::Colour(0xffb47ac4) },
        ColourChoice { "Rose", juce::Colour(0xffd9799b) },
        ColourChoice { "Cyan", juce::Colour(0xff67c7d4) },
        ColourChoice { "Slate", juce::Colour(0xff8f969c) }
    };

    juce::PopupMenu menu;
    for (const auto& choice : choices)
    {
        juce::PopupMenu::Item item(choice.name);
        item.colour = choice.colour;
        item.isTicked = track->colour == choice.colour;
        item.action = [this, trackId = track->id, colour = choice.colour]
        {
            const auto* current = project.findTrack(trackId);
            if (current == nullptr || current->colour == colour)
                return;

            const auto before = TrackMixState::fromTrack(*current);
            auto after = before;
            after.colour = colour;
            perform(std::make_unique<SetTrackMixCommand>(trackId, before, after));
        };
        menu.addItem(std::move(item));
    }

    menu.addSeparator();
    juce::PopupMenu::Item customItem("Custom colour...");
    customItem.action = [safe = juce::Component::SafePointer<MainComponent>(this),
                         trackId = track->id,
                         initialColour = track->colour]
    {
        if (safe == nullptr)
            return;

        auto selector = std::make_unique<TrackColourSelector>(
            initialColour,
            [safe, trackId](juce::Colour previewColour)
            {
                if (safe == nullptr)
                    return;
                auto* previewTrack = safe->project.findTrack(trackId);
                if (previewTrack == nullptr)
                    return;

                previewTrack->colour = previewColour;
                safe->timeline.repaint();
                safe->mixer->repaint();
                if (safe->selectedTrackId == trackId)
                {
                    safe->trackColourButton.setColour(juce::TextButton::buttonColourId,
                                                      previewColour);
                    safe->trackColourButton.setColour(juce::TextButton::textColourOffId,
                                                      previewColour.contrasting());
                }
            },
            [safe, trackId](juce::Colour initial, juce::Colour selected)
            {
                if (safe == nullptr)
                    return;
                const auto* current = safe->project.findTrack(trackId);
                if (current == nullptr)
                    return;

                auto before = TrackMixState::fromTrack(*current);
                before.colour = initial;
                auto after = before;
                after.colour = selected;
                safe->perform(std::make_unique<SetTrackMixCommand>(trackId, before, after));
            });
        juce::CallOutBox::launchAsynchronously(std::move(selector),
                                               safe->trackColourButton.getScreenBounds(),
                                               nullptr);
    };
    menu.addItem(std::move(customItem));

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(trackColourButton));
}

void MainComponent::showTrackQuickEditor(const juce::String& trackId,
                                         juce::Rectangle<int> targetScreenArea)
{
    const auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return;

    auto editor = std::make_unique<TrackQuickEditor>(
        track->name,
        track->colour,
        [safe = juce::Component::SafePointer<MainComponent>(this),
         trackId](juce::String name, juce::Colour colour)
        {
            if (safe == nullptr)
                return;
            const auto* current = safe->project.findTrack(trackId);
            if (current == nullptr)
                return;

            std::vector<std::unique_ptr<ProjectCommand>> commands;
            if (name != current->name)
            {
                commands.push_back(std::make_unique<RenameTrackCommand>(
                    trackId,
                    std::move(name)));
            }
            if (colour != current->colour)
            {
                const auto before = TrackMixState::fromTrack(*current);
                auto after = before;
                after.colour = colour;
                commands.push_back(std::make_unique<SetTrackMixCommand>(
                    trackId,
                    before,
                    after));
            }
            if (commands.empty())
                return;
            safe->perform(std::make_unique<BatchProjectCommand>(
                "Edit track appearance",
                std::move(commands)));
        });
    auto* editorPointer = editor.get();
    juce::CallOutBox::launchAsynchronously(std::move(editor),
                                           targetScreenArea,
                                           nullptr);
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<TrackQuickEditor>(editorPointer)]
        {
            if (safe != nullptr)
                safe->focusName();
        });
}

void MainComponent::showTrackingMenu()
{
    const auto position = audioEngine.positionSeconds();
    juce::PopupMenu menu;
    menu.addSectionHeader("Tempo and meter");
    menu.addItem("Add tempo change at playhead...", [this] { promptTempoChange(); });
    menu.addItem("Add meter change at playhead...", [this] { promptMeterChange(); });
    menu.addItem("Remove nearby tempo change", [this, position]
    {
        changeTransportState([this, position](auto& state)
        {
            const auto iterator = std::min_element(
                state.tempoChanges.begin(),
                state.tempoChanges.end(),
                [position](const auto& left, const auto& right)
                {
                    return std::abs(left.timeSeconds - position)
                        < std::abs(right.timeSeconds - position);
                });
            if (iterator == state.tempoChanges.end()
                || std::abs(iterator->timeSeconds - position) > 0.25)
            {
                setStatus("No tempo change is close to the playhead.", true);
                return;
            }
            state.tempoChanges.erase(iterator);
        });
    });
    menu.addItem("Remove nearby meter change", [this, position]
    {
        changeTransportState([this, position](auto& state)
        {
            const auto iterator = std::min_element(
                state.meterChanges.begin(),
                state.meterChanges.end(),
                [position](const auto& left, const auto& right)
                {
                    return std::abs(left.timeSeconds - position)
                        < std::abs(right.timeSeconds - position);
                });
            if (iterator == state.meterChanges.end()
                || std::abs(iterator->timeSeconds - position) > 0.25)
            {
                setStatus("No meter change is close to the playhead.", true);
                return;
            }
            state.meterChanges.erase(iterator);
        });
    });

    menu.addSeparator();
    menu.addSectionHeader("Punch and ranges");
    menu.addItem("Punch recording",
                 true,
                 project.punchEnabled,
                 [this]
                 {
                     changeTransportState([](auto& state)
                     {
                         state.punchEnabled = !state.punchEnabled;
                     });
                 });
    menu.addItem("Set punch in to playhead", [this, position]
    {
        changeTransportState([position](auto& state)
        {
            state.punchInSeconds = position;
            state.punchOutSeconds = std::max(state.punchOutSeconds, position + 0.01);
        });
    });
    menu.addItem("Set punch out to playhead", [this, position]
    {
        changeTransportState([position](auto& state)
        {
            state.punchOutSeconds = position;
        });
    });
    menu.addItem("Set loop start to playhead", [this, position]
    {
        changeTransportState([position](auto& state)
        {
            state.loopStartSeconds = position;
            state.loopEndSeconds = std::max(state.loopEndSeconds, position + 0.01);
        });
    });
    menu.addItem("Set loop end to playhead", [this, position]
    {
        changeTransportState([position](auto& state)
        {
            state.loopEndSeconds = position;
        });
    });

    const auto addIntegerChoices = [this](const juce::String& title,
                                          int current,
                                          std::initializer_list<int> values,
                                          const std::function<void(ProjectTransportState&, int)>& set)
    {
        juce::PopupMenu submenu;
        for (const auto value : values)
        {
            submenu.addItem(juce::String(value),
                            true,
                            value == current,
                            [this, value, set]
                            {
                                changeTransportState([value, set](auto& state)
                                {
                                    set(state, value);
                                });
                            });
        }
        return std::pair { title, submenu };
    };

    auto countIn = addIntegerChoices(
        "Count-in bars",
        project.countInBars,
        { 0, 1, 2, 4, 8 },
        [](auto& state, int value) { state.countInBars = value; });
    menu.addSubMenu(countIn.first, countIn.second);
    auto subdivision = addIntegerChoices(
        "Click subdivision",
        project.metronomeSubdivision,
        { 1, 2, 4, 8 },
        [](auto& state, int value) { state.metronomeSubdivision = value; });
    menu.addSubMenu(subdivision.first, subdivision.second);

    const auto addRollChoices = [this](const juce::String& title,
                                       double current,
                                       const std::function<void(ProjectTransportState&, double)>& set)
    {
        juce::PopupMenu submenu;
        for (const auto value : { 0.0, 0.5, 1.0, 2.0, 4.0 })
        {
            submenu.addItem(juce::String(value, 1) + " s",
                            true,
                            std::abs(value - current) < 0.0001,
                            [this, value, set]
                            {
                                changeTransportState([value, set](auto& state)
                                {
                                    set(state, value);
                                });
                            });
        }
        return std::pair { title, submenu };
    };

    auto preRoll = addRollChoices(
        "Pre-roll",
        project.preRollSeconds,
        [](auto& state, double value) { state.preRollSeconds = value; });
    menu.addSubMenu(preRoll.first, preRoll.second);
    auto postRoll = addRollChoices(
        "Post-roll",
        project.postRollSeconds,
        [](auto& state, double value) { state.postRollSeconds = value; });
    menu.addSubMenu(postRoll.first, postRoll.second);

    juce::PopupMenu clickOutputMenu;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto names = device->getOutputChannelNames();
        for (int index = 0; index < names.size(); ++index)
        {
            auto name = names[index].trim();
            if (name.isEmpty())
                name = "Output " + juce::String(index + 1);
            clickOutputMenu.addItem(name,
                                    true,
                                    project.metronomeOutputChannel == index,
                                    [this, index]
                                    {
                                        changeTransportState([index](auto& state)
                                        {
                                            state.metronomeOutputChannel = index;
                                        });
                                    });
        }
    }
    if (clickOutputMenu.getNumItems() == 0)
        clickOutputMenu.addItem("No audio outputs", false, false, [] {});
    menu.addSubMenu("Click output", clickOutputMenu);

    menu.addSeparator();
    menu.addSectionHeader("Linked performance editing");
    menu.addItem("Link armed parent tracks", [this]
    {
        const auto trackIds = project.armedAudioParentTrackIds();
        if (trackIds.size() < 2)
        {
            setStatus("Arm at least two parent tracks before linking them.", true);
            return;
        }

        changeEditGroups([trackIds](auto& groups)
        {
            for (auto& group : groups)
            {
                group.trackIds.erase(
                    std::remove_if(group.trackIds.begin(),
                                   group.trackIds.end(),
                                   [&trackIds](const auto& trackId)
                                   {
                                       return std::find(trackIds.cbegin(),
                                                        trackIds.cend(),
                                                        trackId)
                                           != trackIds.cend();
                                   }),
                    group.trackIds.end());
                if (std::find(group.trackIds.cbegin(),
                              group.trackIds.cend(),
                              group.timingReferenceTrackId)
                    == group.trackIds.cend()
                    && !group.trackIds.empty())
                    group.timingReferenceTrackId = group.trackIds.front();
            }
            groups.erase(std::remove_if(groups.begin(),
                                        groups.end(),
                                        [](const auto& group)
                                        {
                                            return group.trackIds.size() < 2;
                                        }),
                         groups.end());

            EditGroup group;
            group.name = "Edit group " + juce::String(static_cast<int>(groups.size() + 1));
            group.trackIds = trackIds;
            group.timingReferenceTrackId = trackIds.front();
            groups.push_back(std::move(group));
        });
    });

    const auto selectedRootId = project.rootTrackId(selectedTrackId);
    const auto* selectedGroup = project.editGroupForTrack(selectedRootId);
    if (selectedGroup != nullptr)
    {
        const auto groupId = selectedGroup->id;
        menu.addItem("Linked edits enabled",
                     true,
                     selectedGroup->enabled,
                     [this, groupId]
                     {
                         changeEditGroups([groupId](auto& groups)
                         {
                             const auto group = std::find_if(
                                 groups.begin(),
                                 groups.end(),
                                 [&groupId](const auto& candidate)
                                 {
                                     return candidate.id == groupId;
                                 });
                             if (group != groups.end())
                                 group->enabled = !group->enabled;
                         });
                     });
        menu.addItem("Use selected track as timing reference",
                     selectedGroup->timingReferenceTrackId != selectedRootId,
                     false,
                     [this, groupId, selectedRootId]
                     {
                         changeEditGroups([groupId, selectedRootId](auto& groups)
                         {
                             const auto group = std::find_if(
                                 groups.begin(),
                                 groups.end(),
                                 [&groupId](const auto& candidate)
                                 {
                                     return candidate.id == groupId;
                                 });
                             if (group != groups.end())
                                 group->timingReferenceTrackId = selectedRootId;
                         });
                     });

        juce::PopupMenu strengthMenu;
        for (const auto strength : { 0.25, 0.5, 0.75, 1.0 })
        {
            strengthMenu.addItem(juce::String(static_cast<int>(strength * 100.0)) + "%",
                                 true,
                                 std::abs(selectedGroup->quantizeStrength - strength) < 0.0001,
                                 [this, groupId, strength]
                                 {
                                     changeEditGroups([groupId, strength](auto& groups)
                                     {
                                         const auto group = std::find_if(
                                             groups.begin(),
                                             groups.end(),
                                             [&groupId](const auto& candidate)
                                             {
                                                 return candidate.id == groupId;
                                             });
                                         if (group != groups.end())
                                             group->quantizeStrength = strength;
                                     });
                                 });
        }
        menu.addSubMenu("Quantize strength", strengthMenu);
        menu.addItem("Quantize selected linked edit", [this]
        {
            quantizeSelectedGroup();
        });
        menu.addItem("Add protected anchor at playhead", [this, groupId, position]
        {
            changeEditGroups([groupId, position](auto& groups)
            {
                const auto group = std::find_if(
                    groups.begin(),
                    groups.end(),
                    [&groupId](const auto& candidate)
                    {
                        return candidate.id == groupId;
                    });
                if (group == groups.end())
                    return;
                group->protectedAnchorsSeconds.push_back(position);
                std::sort(group->protectedAnchorsSeconds.begin(),
                          group->protectedAnchorsSeconds.end());
                group->protectedAnchorsSeconds.erase(
                    std::unique(group->protectedAnchorsSeconds.begin(),
                                group->protectedAnchorsSeconds.end(),
                                [](double left, double right)
                                {
                                    return std::abs(left - right) < 0.0001;
                                }),
                    group->protectedAnchorsSeconds.end());
            });
        });
        menu.addItem("Remove nearby protected anchor", [this, groupId, position]
        {
            changeEditGroups([groupId, position](auto& groups)
            {
                const auto group = std::find_if(
                    groups.begin(),
                    groups.end(),
                    [&groupId](const auto& candidate)
                    {
                        return candidate.id == groupId;
                    });
                if (group == groups.end() || group->protectedAnchorsSeconds.empty())
                    return;
                const auto anchor = std::min_element(
                    group->protectedAnchorsSeconds.begin(),
                    group->protectedAnchorsSeconds.end(),
                    [position](double left, double right)
                    {
                        return std::abs(left - position) < std::abs(right - position);
                    });
                if (std::abs(*anchor - position) <= 0.25)
                    group->protectedAnchorsSeconds.erase(anchor);
            });
        });
        menu.addItem("Unlink selected track", [this, groupId, selectedRootId]
        {
            changeEditGroups([groupId, selectedRootId](auto& groups)
            {
                const auto group = std::find_if(
                    groups.begin(),
                    groups.end(),
                    [&groupId](const auto& candidate)
                    {
                        return candidate.id == groupId;
                    });
                if (group == groups.end())
                    return;
                group->trackIds.erase(std::remove(group->trackIds.begin(),
                                                  group->trackIds.end(),
                                                  selectedRootId),
                                      group->trackIds.end());
                if (!group->trackIds.empty()
                    && group->timingReferenceTrackId == selectedRootId)
                    group->timingReferenceTrackId = group->trackIds.front();
                if (group->trackIds.size() < 2)
                    groups.erase(group);
            });
        });
    }

    menu.addSeparator();
    menu.addSectionHeader("DI and reamp tone paths");
    const auto* selectedRoot = project.findTrack(selectedRootId);
    if (selectedRoot != nullptr && selectedRoot->type == TrackType::audio)
    {
        juce::PopupMenu hardwareReturns;
        for (const auto& candidate : project.tracks)
        {
            if (candidate.parentTrackId.isNotEmpty()
                || candidate.type != TrackType::audio
                || candidate.id == selectedRootId)
                continue;
            const auto returnInUse = project.reampRouteForReturn(candidate.id)
                != nullptr;
            hardwareReturns.addItem(
                candidate.name,
                !returnInUse,
                false,
                [this, sourceTrackId = selectedRootId, returnTrackId = candidate.id]
                {
                    const auto* source = project.findTrack(sourceTrackId);
                    const auto* returnTrack = project.findTrack(returnTrackId);
                    if (source == nullptr || returnTrack == nullptr)
                        return;
                    changeReampRoutes([source, returnTrack](auto& routes)
                    {
                        ReampRoute route;
                        route.name = source->name + " -> " + returnTrack->name;
                        route.type = TonePathType::hardware;
                        route.sourceTrackId = source->id;
                        route.returnTrackId = returnTrack->id;
                        route.inputChannel = returnTrack->inputChannel;
                        routes.push_back(std::move(route));
                    });
                });
        }
        if (hardwareReturns.getNumItems() == 0)
            hardwareReturns.addItem("No available return tracks", false, false, [] {});
        menu.addSubMenu("Create hardware path to", hardwareReturns);
        menu.addItem("Create plugin tone path", [this, sourceTrackId = selectedRootId]
        {
            createPluginTonePath(sourceTrackId);
        });
    }

    const auto route = std::find_if(
        project.reampRoutes.cbegin(),
        project.reampRoutes.cend(),
        [&selectedRootId](const auto& candidate)
        {
            return candidate.sourceTrackId == selectedRootId
                || candidate.returnTrackId == selectedRootId;
        });
    if (route != project.reampRoutes.cend())
    {
        const auto routeId = route->id;
        menu.addItem("Tone path enabled",
                     true,
                     route->enabled,
                     [this, routeId]
                     {
                         changeReampRoutes([routeId](auto& routes)
                         {
                             const auto current = std::find_if(
                                 routes.begin(),
                                 routes.end(),
                                 [&routeId](const auto& candidate)
                                 {
                                     return candidate.id == routeId;
                                 });
                             if (current != routes.end())
                                 current->enabled = !current->enabled;
                         });
                     });
        menu.addItem("Invert return polarity",
                     true,
                     route->polarityInverted,
                     [this, routeId]
                     {
                         changeReampRoutes([routeId](auto& routes)
                         {
                             const auto current = std::find_if(
                                 routes.begin(),
                                 routes.end(),
                                 [&routeId](const auto& candidate)
                                 {
                                     return candidate.id == routeId;
                                 });
                             if (current != routes.end())
                                 current->polarityInverted
                                     = !current->polarityInverted;
                         });
                     });

        juce::PopupMenu alignmentMenu;
        for (const auto adjustment : { -128, -32, -1, 0, 1, 32, 128 })
        {
            alignmentMenu.addItem(
                (adjustment > 0 ? "+" : "") + juce::String(adjustment) + " samples",
                true,
                route->alignmentOffsetSamples == adjustment,
                [this, routeId, adjustment]
                {
                    changeReampRoutes([routeId, adjustment](auto& routes)
                    {
                        const auto current = std::find_if(
                            routes.begin(),
                            routes.end(),
                            [&routeId](const auto& candidate)
                            {
                                return candidate.id == routeId;
                            });
                        if (current != routes.end())
                            current->alignmentOffsetSamples = adjustment;
                    });
                });
        }
        menu.addSubMenu("Fine alignment", alignmentMenu);

        if (route->type == TonePathType::hardware)
        {
            juce::PopupMenu outputMenu;
            juce::PopupMenu inputMenu;
            if (auto* device = deviceManager.getCurrentAudioDevice())
            {
                const auto outputs = device->getOutputChannelNames();
                for (int index = 0; index < outputs.size(); ++index)
                {
                    auto name = outputs[index].trim();
                    if (name.isEmpty())
                        name = "Output " + juce::String(index + 1);
                    outputMenu.addItem(name,
                                       true,
                                       route->outputChannel == index,
                                       [this, routeId, index]
                                       {
                                           changeReampRoutes([routeId, index](auto& routes)
                                           {
                                               const auto current = std::find_if(
                                                   routes.begin(),
                                                   routes.end(),
                                                   [&routeId](const auto& candidate)
                                                   {
                                                       return candidate.id == routeId;
                                                   });
                                               if (current != routes.end())
                                                   current->outputChannel = index;
                                           });
                                       });
                }
                const auto inputs = device->getInputChannelNames();
                for (int index = 0; index < inputs.size(); ++index)
                {
                    auto name = inputs[index].trim();
                    if (name.isEmpty())
                        name = "Input " + juce::String(index + 1);
                    inputMenu.addItem(name,
                                      true,
                                      route->inputChannel == index,
                                      [this, routeId, index]
                                      {
                                          changeReampRoutes([routeId, index](auto& routes)
                                          {
                                              const auto current = std::find_if(
                                                  routes.begin(),
                                                  routes.end(),
                                                  [&routeId](const auto& candidate)
                                                  {
                                                      return candidate.id == routeId;
                                                  });
                                              if (current != routes.end())
                                                  current->inputChannel = index;
                                          });
                                      });
                }
            }
            menu.addSubMenu("Reamp output", outputMenu);
            menu.addSubMenu("Return input", inputMenu);
            menu.addItem("Calibrate round-trip latency", [this, routeId]
            {
                const auto current = std::find_if(
                    project.reampRoutes.cbegin(),
                    project.reampRoutes.cend(),
                    [&routeId](const auto& candidate)
                    {
                        return candidate.id == routeId;
                    });
                if (current == project.reampRoutes.cend())
                    return;
                const auto result = audioEngine.startLatencyCalibration(
                    current->outputChannel,
                    current->inputChannel);
                if (result.failed())
                {
                    showError("Reamp calibration unavailable",
                              result.getErrorMessage());
                    return;
                }
                calibratingReampRouteId = routeId;
                setStatus("Sending reamp calibration pulse...");
            });
        }
        menu.addItem("Remove tone path", [this, routeId]
        {
            changeReampRoutes([routeId](auto& routes)
            {
                routes.erase(std::remove_if(routes.begin(),
                                            routes.end(),
                                            [&routeId](const auto& candidate)
                                            {
                                                return candidate.id == routeId;
                                            }),
                             routes.end());
            });
        });
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(trackingButton));
}

void MainComponent::promptTempoChange()
{
    const auto position = audioEngine.positionSeconds();
    auto* dialog = new juce::AlertWindow("Tempo change",
                                         "Set the tempo at the current playhead.",
                                         juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("bpm",
                          juce::String(project.tempoAt(position), 2),
                          "BPM");
    dialog->addComboBox("transition",
                        { "Jump", "Ramp from previous point" },
                        "Transition");
    dialog->getComboBoxComponent("transition")->setSelectedItemIndex(0);
    dialog->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->centreAroundComponent(&trackingButton, 380, 210);
    const juce::Component::SafePointer<juce::AlertWindow> dialogSafe(dialog);
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe = juce::Component::SafePointer<MainComponent>(this),
             dialogSafe,
             position](int result)
            {
                if (result != 1 || safe == nullptr || dialogSafe == nullptr)
                    return;

                const auto bpm = dialogSafe->getTextEditorContents("bpm").getDoubleValue();
                if (bpm < 20.0 || bpm > 400.0)
                {
                    safe->showError("Tempo change unavailable",
                                    "Enter a tempo from 20 to 400 BPM.");
                    return;
                }
                const auto rampFromPrevious
                    = dialogSafe->getComboBoxComponent("transition")
                          ->getSelectedItemIndex()
                    == 1;
                safe->changeTransportState([position, bpm, rampFromPrevious](auto& state)
                {
                    if (state.tempoChanges.empty() && position > 0.0001)
                        state.tempoChanges.push_back({ 0.0, state.tempo, false });

                    const auto existing = std::find_if(
                        state.tempoChanges.begin(),
                        state.tempoChanges.end(),
                        [position](const auto& change)
                        {
                            return std::abs(change.timeSeconds - position) < 0.0001;
                        });
                    if (existing != state.tempoChanges.end())
                        existing->bpm = bpm;
                    else
                        state.tempoChanges.push_back({ position, bpm, false });

                    std::stable_sort(state.tempoChanges.begin(),
                                     state.tempoChanges.end(),
                                     [](const auto& left, const auto& right)
                                     {
                                         return left.timeSeconds < right.timeSeconds;
                                     });
                    const auto inserted = std::find_if(
                        state.tempoChanges.begin(),
                        state.tempoChanges.end(),
                        [position](const auto& change)
                        {
                            return std::abs(change.timeSeconds - position) < 0.0001;
                        });
                    if (inserted != state.tempoChanges.begin()
                        && inserted != state.tempoChanges.end())
                        (inserted - 1)->rampToNext = rampFromPrevious;
                    if (position <= 0.0001)
                        state.tempo = bpm;
                });
            }),
        true);
}

void MainComponent::promptMeterChange()
{
    const auto position = audioEngine.positionSeconds();
    const auto current = project.meterAt(position);
    auto* dialog = new juce::AlertWindow("Meter change",
                                         "Set the time signature at the current playhead.",
                                         juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("numerator",
                          juce::String(current.numerator),
                          "Numerator");
    dialog->addComboBox("denominator",
                        { "1", "2", "4", "8", "16", "32" },
                        "Denominator");
    const std::array denominators { 1, 2, 4, 8, 16, 32 };
    const auto denominator = std::find(denominators.cbegin(),
                                       denominators.cend(),
                                       current.denominator);
    dialog->getComboBoxComponent("denominator")->setSelectedItemIndex(
        denominator == denominators.cend()
            ? 2
            : static_cast<int>(std::distance(denominators.cbegin(), denominator)));
    dialog->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->centreAroundComponent(&trackingButton, 380, 210);
    const juce::Component::SafePointer<juce::AlertWindow> dialogSafe(dialog);
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe = juce::Component::SafePointer<MainComponent>(this),
             dialogSafe,
             position,
             denominators](int result)
            {
                if (result != 1 || safe == nullptr || dialogSafe == nullptr)
                    return;

                const auto numerator
                    = dialogSafe->getTextEditorContents("numerator").getIntValue();
                const auto denominatorIndex
                    = dialogSafe->getComboBoxComponent("denominator")
                          ->getSelectedItemIndex();
                if (numerator < 1
                    || numerator > 32
                    || denominatorIndex < 0
                    || denominatorIndex >= static_cast<int>(denominators.size()))
                {
                    safe->showError("Meter change unavailable",
                                    "Enter a numerator from 1 to 32.");
                    return;
                }
                const auto selectedDenominator
                    = denominators[static_cast<std::size_t>(denominatorIndex)];
                safe->changeTransportState(
                    [position, numerator, selectedDenominator](auto& state)
                    {
                        if (state.meterChanges.empty() && position > 0.0001)
                        {
                            state.meterChanges.push_back({
                                0.0,
                                state.timeSignatureNumerator,
                                state.timeSignatureDenominator
                            });
                        }

                        const auto existing = std::find_if(
                            state.meterChanges.begin(),
                            state.meterChanges.end(),
                            [position](const auto& change)
                            {
                                return std::abs(change.timeSeconds - position) < 0.0001;
                            });
                        if (existing != state.meterChanges.end())
                        {
                            existing->numerator = numerator;
                            existing->denominator = selectedDenominator;
                        }
                        else
                        {
                            state.meterChanges.push_back({
                                position,
                                numerator,
                                selectedDenominator
                            });
                        }
                        std::stable_sort(state.meterChanges.begin(),
                                         state.meterChanges.end(),
                                         [](const auto& left, const auto& right)
                                         {
                                             return left.timeSeconds < right.timeSeconds;
                                         });
                        if (position <= 0.0001)
                        {
                            state.timeSignatureNumerator = numerator;
                            state.timeSignatureDenominator = selectedDenominator;
                        }
                    });
            }),
        true);
}

void MainComponent::createPluginTonePath(const juce::String& sourceTrackId)
{
    const auto* source = project.findTrack(sourceTrackId);
    if (source == nullptr
        || source->type != TrackType::audio
        || source->parentTrackId.isNotEmpty())
        return;

    const auto activeTakeId = project.activeTakeTrackId(sourceTrackId);
    const auto* clipSource = activeTakeId.isNotEmpty()
        ? project.findTrack(activeTakeId)
        : source;
    if (clipSource == nullptr || clipSource->clips.empty())
    {
        setStatus("The DI source needs audio before creating a plugin tone path.", true);
        return;
    }

    Track toneTrack;
    toneTrack.name = source->name + " Tone";
    toneTrack.type = TrackType::audio;
    toneTrack.colour = source->colour.brighter(0.15f);
    toneTrack.clips = clipSource->clips;
    for (auto& clip : toneTrack.clips)
    {
        clip.id = juce::Uuid().toString();
        clip.name = source->name + " tone";
        clip.colour = toneTrack.colour;
    }

    ReampRoute route;
    route.name = toneTrack.name;
    route.type = TonePathType::plugin;
    route.sourceTrackId = sourceTrackId;
    route.returnTrackId = toneTrack.id;
    route.ownsReturnTrack = true;
    auto routes = project.reampRoutes;
    routes.push_back(route);

    const auto toneTrackId = toneTrack.id;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    commands.push_back(std::make_unique<AddTrackCommand>(std::move(toneTrack)));
    commands.push_back(std::make_unique<SetReampRoutesCommand>(project.reampRoutes,
                                                               std::move(routes)));
    if (perform(std::make_unique<BatchProjectCommand>(
            "Create plugin tone path",
            std::move(commands))))
    {
        selectTrack(toneTrackId);
        setStatus("Plugin tone path created. Add VST3 inserts to the new tone track.");
    }
}

void MainComponent::updateInputMonitoring()
{
    const Track* monitored = nullptr;
    if (const auto* selected = project.findTrack(selectedTrackId);
        selected != nullptr && selected->type == TrackType::audio && selected->inputMonitoring)
        monitored = selected;

    if (monitored == nullptr)
    {
        const auto iterator = std::find_if(project.tracks.cbegin(), project.tracks.cend(), [](const auto& track)
        {
            return track.type == TrackType::audio && track.inputMonitoring;
        });
        if (iterator != project.tracks.cend())
            monitored = &*iterator;
    }

    audioEngine.setInputMonitoring(monitored != nullptr,
                                   monitored != nullptr ? monitored->inputChannel : 0,
                                   monitored != nullptr && monitored->stereoInput ? 2 : 1);
}

void MainComponent::updateTimelineSize()
{
    if (timelineViewport.getWidth() <= 0 || timelineViewport.getHeight() <= 0)
        return;

    timeline.setSize(timeline.preferredWidth(timelineViewport.getWidth()),
                     timeline.preferredHeight(timelineViewport.getHeight()));
}

void MainComponent::zoomTimeline(double factor, bool reset)
{
    const auto newPixelsPerSecond = reset
        ? 96.0
        : timeline.getPixelsPerSecond() * factor;
    timeline.setPixelsPerSecond(newPixelsPerSecond);
    const auto percentage = static_cast<int>(std::round(timeline.getPixelsPerSecond() / 96.0 * 100.0));
    zoomResetButton.setButtonText(juce::String(percentage) + "%");
    updateTimelineSize();

    const auto playheadX = static_cast<int>(timeline.xForSeconds(audioEngine.positionSeconds()));
    timelineViewport.setViewPosition(juce::jmax(0,
                                               playheadX - timelineViewport.getWidth() / 2),
                                     timelineViewport.getViewPositionY());
    timeline.setViewportPosition(timelineViewport.getViewPositionX());
}

void MainComponent::projectChanged(bool writeRecovery, bool markDirty)
{
    if (markDirty)
        dirty = true;

    timeline.setProject(&project);
    mixer->setProject(&project);
    routingPanel->setProject(&project);
    insertPanel->setProject(&project);
    projectLabel.setText(project.name + (dirty ? " *" : ""), juce::dontSendNotification);
    loopButton.setToggleState(project.loopEnabled, juce::dontSendNotification);
    metronomeButton.setToggleState(project.metronomeEnabled, juce::dontSendNotification);
    undoButton.setEnabled(commandStack.canUndo());
    redoButton.setEnabled(commandStack.canRedo());
    updateInspector();
    updateTimelineSize();
    updateReducedIsolationMarker();

    if (const auto result = audioEngine.updateProject(project,
                                                      pluginRuntimeRequests()); result.failed())
        setStatus(result.getErrorMessage(), true);
    updateInputMonitoring();

    if (writeRecovery && projectPackage.exists())
        if (const auto result = ProjectFile::writeRecoveryPoint(project, projectPackage); result.failed())
            setStatus(result.getErrorMessage(), true);
}

void MainComponent::updateReducedIsolationMarker()
{
    if (!projectPackage.exists())
        return;
    std::vector<juce::String> insertIds;
    for (const auto& track : project.tracks)
        for (const auto& insert : track.inserts)
            if (insert.bridgeMode != PluginBridgeMode::sandboxed)
                insertIds.push_back(insert.id);
    std::sort(insertIds.begin(), insertIds.end());
    juce::StringArray signatureParts;
    for (const auto& insertId : insertIds)
        signatureParts.add(insertId);
    const auto signature = signatureParts.joinIntoString("|");
    if (signature == reducedIsolationMarkerSignature)
        return;
    reducedIsolationMarkerSignature = signature;
    const auto result = insertIds.empty()
        ? ProjectFile::clearReducedIsolationMarker(projectPackage)
        : ProjectFile::writeReducedIsolationMarker(
              projectPackage,
              insertIds);
    if (result.failed())
        setStatus(result.getErrorMessage(), true);
}

std::vector<StudioAudioEngine::PluginRuntimeRequest> MainComponent::pluginRuntimeRequests() const
{
    std::vector<StudioAudioEngine::PluginRuntimeRequest> requests;
    for (const auto& track : project.tracks)
    {
        for (const auto& insert : track.inserts)
        {
            StudioAudioEngine::PluginRuntimeRequest request;
            request.trackId = track.id;
            request.insertId = insert.id;
            request.name = insert.name;
            request.bypassed = insert.bypassed;
            request.missing = false;
            request.bridgeMode = insert.bridgeMode;
            request.recoveryDisabled = insert.recoveryDisabled;
            request.latencySamples = insert.latencySamples;
            request.tailSeconds = insert.tailSeconds;
            request.catalogRevision = pluginCatalog.revision();
            request.description = pluginCatalog.descriptionForIdentifier(insert.pluginIdentifier);
            if (!request.description.has_value())
                request.missing = true;

            if (insert.stateFile.isNotEmpty())
            {
                juce::String stateError;
                if (!projectPackage.exists()
                    || !PluginStateStore::load(
                        projectPackage,
                        { insert.stateFile, insert.stateHash },
                        request.state,
                        stateError))
                    request.missing = true;
            }

            requests.push_back(std::move(request));
        }
    }
    return requests;
}

bool MainComponent::perform(std::unique_ptr<ProjectCommand> command)
{
    juce::String error;
    if (!commandStack.perform(std::move(command), project, error))
    {
        showError("Edit failed", error);
        return false;
    }

    projectChanged();
    return true;
}

void MainComponent::changeSelectedTrackState(const std::function<void(TrackMixState&)>& change)
{
    const auto* track = project.findTrack(selectedTrackId);
    if (track == nullptr)
        return;

    const auto before = TrackMixState::fromTrack(*track);
    auto after = before;
    change(after);
    perform(std::make_unique<SetTrackMixCommand>(track->id, before, after));
}

void MainComponent::changeTransportState(
    const std::function<void(ProjectTransportState&)>& change)
{
    const auto before = ProjectTransportState::fromProject(project);
    auto after = before;
    change(after);
    const auto same = [](double left, double right)
    {
        return std::abs(left - right) < 0.0000001;
    };
    if (same(before.tempo, after.tempo)
        && before.timeSignatureNumerator == after.timeSignatureNumerator
        && before.timeSignatureDenominator == after.timeSignatureDenominator
        && before.tempoChanges == after.tempoChanges
        && before.meterChanges == after.meterChanges
        && before.metronomeEnabled == after.metronomeEnabled
        && before.metronomeSubdivision == after.metronomeSubdivision
        && before.metronomeOutputChannel == after.metronomeOutputChannel
        && same(before.metronomeLevel, after.metronomeLevel)
        && same(before.metronomeAccentLevel, after.metronomeAccentLevel)
        && before.punchEnabled == after.punchEnabled
        && same(before.punchInSeconds, after.punchInSeconds)
        && same(before.punchOutSeconds, after.punchOutSeconds)
        && before.countInBars == after.countInBars
        && same(before.preRollSeconds, after.preRollSeconds)
        && same(before.postRollSeconds, after.postRollSeconds)
        && before.loopEnabled == after.loopEnabled
        && same(before.loopStartSeconds, after.loopStartSeconds)
        && same(before.loopEndSeconds, after.loopEndSeconds))
        return;
    perform(std::make_unique<SetProjectTransportCommand>(before, after));
}

void MainComponent::changeEditGroups(
    const std::function<void(std::vector<EditGroup>&)>& change)
{
    auto updated = project.editGroups;
    change(updated);
    perform(std::make_unique<SetEditGroupsCommand>(project.editGroups,
                                                   std::move(updated)));
}

void MainComponent::changeReampRoutes(
    const std::function<void(std::vector<ReampRoute>&)>& change)
{
    auto updated = project.reampRoutes;
    change(updated);
    perform(std::make_unique<SetReampRoutesCommand>(project.reampRoutes,
                                                    std::move(updated)));
}

const AudioClip* MainComponent::activeClipAt(const juce::String& parentTrackId,
                                             double seconds) const
{
    const auto* parent = project.findTrack(parentTrackId);
    if (parent == nullptr)
        return nullptr;

    juce::String sourceTrackId;
    const auto compRegion = std::find_if(
        parent->compRegions.cbegin(),
        parent->compRegions.cend(),
        [seconds](const auto& region)
        {
            return seconds >= region.startSeconds - 0.0001
                && seconds < region.endSeconds() + 0.0001;
        });
    if (compRegion != parent->compRegions.cend())
        sourceTrackId = compRegion->sourceTrackId;
    else
        sourceTrackId = project.activeTakeTrackId(parentTrackId);
    if (sourceTrackId.isEmpty())
        sourceTrackId = parentTrackId;

    const auto* source = project.findTrack(sourceTrackId);
    if (source == nullptr)
        return nullptr;
    const auto clip = std::find_if(source->clips.cbegin(),
                                   source->clips.cend(),
                                   [seconds](const auto& candidate)
    {
        return seconds >= candidate.startSeconds - 0.0001
            && seconds < candidate.endSeconds() + 0.0001;
    });
    return clip == source->clips.cend() ? nullptr : &*clip;
}

std::vector<juce::String> MainComponent::linkedClipIdsAt(
    const juce::String& clipId,
    double seconds) const
{
    const auto* selectedTrack = project.findTrackContainingClip(clipId);
    const auto* group = selectedTrack != nullptr
        ? project.editGroupForTrack(selectedTrack->id)
        : nullptr;
    if (group == nullptr || !group->enabled)
        return { clipId };

    const auto selectedRoot = project.rootTrackId(selectedTrack->id);
    std::vector<juce::String> clipIds;
    for (const auto& rootTrackId : group->trackIds)
    {
        if (rootTrackId == selectedRoot)
        {
            clipIds.push_back(clipId);
            continue;
        }
        if (const auto* clip = activeClipAt(rootTrackId, seconds))
            clipIds.push_back(clip->id);
    }
    return clipIds;
}

bool MainComponent::updateLinkedClips(
    const juce::String& clipId,
    const juce::String& commandName,
    const std::function<bool(AudioClip&, const AudioClip&, juce::String&)>& update)
{
    const auto* selected = project.findClip(clipId);
    const auto* selectedTrack = project.findTrackContainingClip(clipId);
    if (selected == nullptr || selectedTrack == nullptr)
        return false;
    const auto reference = selected->startSeconds + selected->durationSeconds * 0.5;
    const auto clipIds = linkedClipIdsAt(clipId, reference);
    const auto* group = project.editGroupForTrack(selectedTrack->id);
    if (group != nullptr
        && group->enabled
        && clipIds.size() != group->trackIds.size())
    {
        setStatus("Every linked track needs an active clip for this operation.", true);
        return false;
    }

    juce::String error;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    for (const auto& linkedId : clipIds)
    {
        const auto* before = project.findClip(linkedId);
        const auto* track = project.findTrackContainingClip(linkedId);
        if (before == nullptr || track == nullptr)
            continue;
        auto after = *before;
        if (!update(after, *before, error))
        {
            showError(commandName + " failed",
                      error.isNotEmpty() ? error : "The clip could not be updated.");
            return false;
        }
        commands.push_back(std::make_unique<SetClipStateCommand>(
            track->id,
            *before,
            std::move(after),
            commandName));
    }
    return perform(std::make_unique<BatchProjectCommand>(
        clipIds.size() > 1 ? commandName + " on linked clips" : commandName,
        std::move(commands)));
}

Track MainComponent::makeRecordingVersionTrack(const Track& parent) const
{
    const auto versionNumber = std::accumulate(
        project.tracks.cbegin(),
        project.tracks.cend(),
        0,
        [&parent](int maximum, const auto& track)
        {
            return track.parentTrackId == parent.id
                ? std::max(maximum, track.versionNumber)
                : maximum;
        })
        + 1;

    Track version;
    version.name = "v" + juce::String(versionNumber);
    version.parentTrackId = parent.id;
    version.versionNumber = versionNumber;
    version.type = TrackType::audio;
    version.volumeDecibels = 0.0f;
    version.pan = 0.0f;
    version.armed = false;
    version.inputChannel = parent.inputChannel;
    version.stereoInput = parent.stereoInput;
    version.inputMonitoring = parent.inputMonitoring;
    version.colour = parent.colour;
    return version;
}

Track* MainComponent::recordingTrack()
{
    if (auto* selected = project.findTrack(selectedTrackId);
        selected != nullptr && selected->type == TrackType::audio && selected->armed)
        return selected;

    const auto iterator = std::find_if(project.tracks.begin(), project.tracks.end(), [](const auto& track)
    {
        return track.type == TrackType::audio && track.armed;
    });
    return iterator == project.tracks.end() ? nullptr : &*iterator;
}

void MainComponent::setStatus(const juce::String& message, bool error)
{
    statusIsError = error;
    statusLabel.setColour(juce::Label::textColourId,
                          juce::Colour(error ? StudioColours::orange : StudioColours::secondaryText));
    statusLabel.setText(message, juce::dontSendNotification);
}

void MainComponent::showError(const juce::String& title, const juce::String& message)
{
    setStatus(message, true);
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message);
}

juce::String MainComponent::positionText(double seconds, const Project& project)
{
    const auto musical = project.musicalPositionAt(seconds);
    const auto minutes = static_cast<int>(seconds) / 60;
    const auto wholeSeconds = static_cast<int>(seconds) % 60;
    const auto milliseconds = static_cast<int>(std::fmod(seconds, 1.0) * 1000.0);

    return juce::String(musical.bar).paddedLeft('0', 3)
        + " | "
        + juce::String(musical.beat).paddedLeft('0', 2)
        + " | "
        + juce::String(musical.ticks).paddedLeft('0', 3)
        + "    "
        + juce::String(minutes).paddedLeft('0', 2)
        + ":"
        + juce::String(wholeSeconds).paddedLeft('0', 2)
        + "."
        + juce::String(milliseconds).paddedLeft('0', 3);
}
}
