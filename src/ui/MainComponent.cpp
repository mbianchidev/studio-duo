#include "MainComponent.h"

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
}

class MainComponent::MixerPanel final : public juce::Component
{
public:
    void setProject(const Project* value)
    {
        project = value;
        repaint();
    }

    void setSelection(const juce::String& value)
    {
        selectedTrack = value;
        repaint();
    }

    void setPeaks(float left, float right)
    {
        leftPeak = left;
        rightPeak = right;
        repaint();
    }

    std::function<void(const juce::String&)> onTrackSelected;
    std::function<void(const juce::String&, float)> onPanChanged;

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(StudioColours::panel));
        if (project == nullptr || project->tracks.empty())
            return;

        constexpr auto stripWidth = 112;
        constexpr auto gap = 8;
        auto x = 14;

        for (const auto& track : project->tracks)
        {
            if (track.parentTrackId.isNotEmpty())
                continue;

            const juce::Rectangle<int> strip(x, 34, stripWidth, getHeight() - 44);
            const auto selected = track.id == selectedTrack;
            graphics.setColour(juce::Colour(selected ? 0xff292e32 : StudioColours::raised));
            graphics.fillRoundedRectangle(strip.toFloat(), 5.0f);
            graphics.setColour(selected ? track.colour : juce::Colour(StudioColours::border));
            graphics.drawRoundedRectangle(strip.toFloat(), 5.0f, selected ? 1.5f : 1.0f);

            graphics.setColour(track.colour);
            graphics.fillRect(strip.getX(), strip.getY(), strip.getWidth(), 4);
            graphics.setColour(juce::Colour(StudioColours::text));
            graphics.setFont(12.0f);
            graphics.drawFittedText(track.name,
                                    strip.reduced(8).withHeight(24),
                                    juce::Justification::centred,
                                    1);

            const auto faderTop = strip.getY() + 46;
            const auto faderHeight = strip.getHeight() - 112;
            graphics.setColour(juce::Colour(StudioColours::window));
            graphics.fillRoundedRectangle(static_cast<float>(strip.getCentreX() - 3),
                                          static_cast<float>(faderTop),
                                          6.0f,
                                          static_cast<float>(faderHeight),
                                          3.0f);

            const auto normalised = juce::jmap(juce::jlimit(-60.0f, 12.0f, track.volumeDecibels),
                                               -60.0f,
                                               12.0f,
                                               1.0f,
                                               0.0f);
            const auto knobY = faderTop + static_cast<int>(normalised * static_cast<float>(faderHeight));
            graphics.setColour(juce::Colour(StudioColours::text));
            graphics.fillRoundedRectangle(static_cast<float>(strip.getCentreX() - 12),
                                          static_cast<float>(knobY - 3),
                                          24.0f,
                                          7.0f,
                                          3.5f);

            juce::String state;
            if (track.muted) state << "M ";
            if (track.solo) state << "S ";
            if (track.armed) state << "R";
            graphics.setColour(track.armed ? juce::Colour(StudioColours::orange)
                                           : juce::Colour(StudioColours::secondaryText));
            graphics.setFont(10.5f);
            graphics.drawText(state.trimEnd(),
                              strip.getX() + 8,
                              strip.getY() + 25,
                              strip.getWidth() - 16,
                              18,
                              juce::Justification::centred);

            const auto panValue = track.id == draggingPanTrack
                ? dragPreviewPan
                : track.pan;
            const auto panText = std::abs(panValue) < 0.005f
                ? juce::String("C")
                : juce::String(static_cast<int>(std::round(std::abs(panValue) * 100.0f)))
                    + (panValue < 0.0f ? "% L" : "% R");
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
            graphics.drawText(juce::String(track.volumeDecibels, 1) + " dB",
                              strip.getX() + 6,
                              faderTop + faderHeight + 2,
                              strip.getWidth() - 12,
                              16,
                              juce::Justification::centred);

            const juce::Point<float> panCentre(static_cast<float>(strip.getCentreX()),
                                               static_cast<float>(strip.getBottom() - 35));
            constexpr auto panRadius = 12.0f;
            graphics.setColour(juce::Colour(StudioColours::window));
            graphics.fillEllipse(panCentre.x - panRadius,
                                 panCentre.y - panRadius,
                                 panRadius * 2.0f,
                                 panRadius * 2.0f);
            graphics.setColour(std::abs(panValue) < 0.005f
                                   ? juce::Colour(StudioColours::secondaryText)
                                   : track.colour);
            graphics.drawEllipse(panCentre.x - panRadius,
                                 panCentre.y - panRadius,
                                 panRadius * 2.0f,
                                 panRadius * 2.0f,
                                 1.5f);
            const auto angle = juce::jmap(panValue,
                                          -1.0f,
                                          1.0f,
                                          -juce::MathConstants<float>::pi * 0.75f,
                                          juce::MathConstants<float>::pi * 0.75f);
            const juce::Point<float> marker(
                panCentre.x + std::sin(angle) * 8.0f,
                panCentre.y - std::cos(angle) * 8.0f);
            graphics.drawLine(panCentre.x, panCentre.y, marker.x, marker.y, 2.0f);
            graphics.setFont(juce::Font(juce::FontOptions(8.0f, juce::Font::bold)));
            graphics.drawText("L",
                              static_cast<int>(panCentre.x - 30.0f),
                              static_cast<int>(panCentre.y - 8.0f),
                              12,
                              16,
                              juce::Justification::centred);
            graphics.drawText("R",
                              static_cast<int>(panCentre.x + 18.0f),
                              static_cast<int>(panCentre.y - 8.0f),
                              12,
                              16,
                              juce::Justification::centred);
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.drawText(panText,
                              strip.getX() + 6,
                              strip.getBottom() - 18,
                              strip.getWidth() - 12,
                              14,
                              juce::Justification::centred);

            x += stripWidth + gap;
        }

        const auto meterX = getWidth() - 32;
        const auto meterHeight = getHeight() - 58;
        graphics.setColour(juce::Colour(StudioColours::window));
        graphics.fillRoundedRectangle(static_cast<float>(meterX),
                                      38.0f,
                                      7.0f,
                                      static_cast<float>(meterHeight),
                                      3.5f);
        graphics.fillRoundedRectangle(static_cast<float>(meterX + 11),
                                      38.0f,
                                      7.0f,
                                      static_cast<float>(meterHeight),
                                      3.5f);
        graphics.setColour(juce::Colour(StudioColours::green));
        graphics.fillRect(meterX,
                          38 + static_cast<int>((1.0f - leftPeak) * static_cast<float>(meterHeight)),
                          7,
                          static_cast<int>(leftPeak * static_cast<float>(meterHeight)));
        graphics.fillRect(meterX + 11,
                          38 + static_cast<int>((1.0f - rightPeak) * static_cast<float>(meterHeight)),
                          7,
                          static_cast<int>(rightPeak * static_cast<float>(meterHeight)));
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (project == nullptr || event.position.y < 34.0f)
            return;

        constexpr auto stripWidth = 112;
        constexpr auto gap = 8;
        const auto index = static_cast<int>((event.position.x - 14.0f) / (stripWidth + gap));
        std::vector<const Track*> mixerTracks;
        for (const auto& track : project->tracks)
            if (track.parentTrackId.isEmpty())
                mixerTracks.push_back(&track);

        if (index < 0 || index >= static_cast<int>(mixerTracks.size()))
            return;

        const auto* track = mixerTracks[static_cast<std::size_t>(index)];
        if (onTrackSelected)
            onTrackSelected(track->id);

        const juce::Rectangle<int> strip(14 + index * (stripWidth + gap),
                                         34,
                                         stripWidth,
                                         getHeight() - 44);
        if (event.position.y >= static_cast<float>(strip.getBottom() - 60))
        {
            draggingPanTrack = track->id;
            dragStartY = event.position.y;
            dragStartPan = track->pan;
            dragPreviewPan = track->pan;
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (draggingPanTrack.isEmpty())
            return;

        dragPreviewPan = juce::jlimit(-1.0f,
                                      1.0f,
                                      dragStartPan + (dragStartY - event.position.y) / 80.0f);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (draggingPanTrack.isEmpty())
            return;

        const auto trackId = draggingPanTrack;
        const auto value = dragPreviewPan;
        draggingPanTrack.clear();
        if (onPanChanged)
            onPanChanged(trackId, value);
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (project == nullptr || event.position.y < 34.0f)
            return;

        constexpr auto stripWidth = 112;
        constexpr auto gap = 8;
        const auto index = static_cast<int>((event.position.x - 14.0f) / (stripWidth + gap));
        std::vector<const Track*> mixerTracks;
        for (const auto& track : project->tracks)
            if (track.parentTrackId.isEmpty())
                mixerTracks.push_back(&track);
        if (index < 0 || index >= static_cast<int>(mixerTracks.size()))
            return;

        const juce::Rectangle<int> strip(14 + index * (stripWidth + gap),
                                         34,
                                         stripWidth,
                                         getHeight() - 44);
        if (event.position.y >= static_cast<float>(strip.getBottom() - 60)
            && onPanChanged)
            onPanChanged(mixerTracks[static_cast<std::size_t>(index)]->id, 0.0f);
    }

private:
    const Project* project = nullptr;
    juce::String selectedTrack;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;
    juce::String draggingPanTrack;
    float dragStartY = 0.0f;
    float dragStartPan = 0.0f;
    float dragPreviewPan = 0.0f;
};

class MainComponent::InsertPanel final : public juce::Component
{
public:
    void setProject(const Project* value)
    {
        project = value;
        repaint();
    }

    void setTrack(const juce::String& value)
    {
        trackId = value;
        repaint();
    }

    void setRuntimeStatuses(std::vector<StudioAudioEngine::PluginRuntimeStatus> value,
                            std::uint64_t lateBlocks)
    {
        runtimeStatuses = std::move(value);
        lateBlockCount = lateBlocks;
        repaint();
    }

    std::function<void(const juce::String&, const juce::String&, bool)> onBypass;
    std::function<void(const juce::String&, const juce::String&)> onRemove;
    std::function<void()> onReload;

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colour(StudioColours::panel));
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        graphics.drawText("INSERTS",
                          0,
                          0,
                          getWidth(),
                          20,
                          juce::Justification::centredLeft);

        const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
        if (track == nullptr || track->inserts.empty())
        {
            graphics.setColour(juce::Colour(StudioColours::secondaryText));
            graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
            graphics.drawFittedText("Double-click a catalog plugin to add a sandboxed insert.",
                                    0,
                                    26,
                                    getWidth(),
                                    42,
                                    juce::Justification::topLeft,
                                    2);
            return;
        }

        auto y = 24;
        for (std::size_t index = 0; index < track->inserts.size(); ++index)
        {
            const auto& insert = track->inserts[index];
            const juce::Rectangle<int> row(0, y, getWidth(), 48);
            graphics.setColour(juce::Colour(insert.bypassed ? 0xff191c1f : StudioColours::raised));
            graphics.fillRoundedRectangle(row.toFloat(), 4.0f);
            graphics.setColour(juce::Colour(insert.missing ? StudioColours::orange
                                                           : StudioColours::border));
            graphics.drawRoundedRectangle(row.toFloat(), 4.0f, 1.0f);

            graphics.setColour(juce::Colour(insert.bypassed ? StudioColours::secondaryText
                                                            : StudioColours::text));
            graphics.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
            graphics.drawFittedText(juce::String(static_cast<int>(index + 1)) + "  " + insert.name,
                                    8,
                                    y + 4,
                                    getWidth() - 76,
                                    18,
                                    juce::Justification::centredLeft,
                                    1);

            const auto status = std::find_if(runtimeStatuses.cbegin(),
                                             runtimeStatuses.cend(),
                                             [&insert](const auto& candidate)
            {
                return candidate.insertId == insert.id;
            });
            auto stateText = juce::String("SANDBOX");
            auto stateColour = juce::Colour(StudioColours::green);
            if (status != runtimeStatuses.cend())
            {
                switch (status->state)
                {
                    case StudioAudioEngine::PluginRuntimeStatus::State::bypassed:
                        stateText = "BYPASSED";
                        stateColour = juce::Colour(StudioColours::amber);
                        break;
                    case StudioAudioEngine::PluginRuntimeStatus::State::missing:
                        stateText = "MISSING";
                        stateColour = juce::Colour(StudioColours::orange);
                        break;
                    case StudioAudioEngine::PluginRuntimeStatus::State::loading:
                        stateText = "LOADING";
                        stateColour = juce::Colour(StudioColours::amber);
                        break;
                    case StudioAudioEngine::PluginRuntimeStatus::State::ready:
                        stateText = "READY";
                        break;
                    case StudioAudioEngine::PluginRuntimeStatus::State::failed:
                        stateText = "CRASHED - CLICK TO RELOAD";
                        stateColour = juce::Colour(StudioColours::orange);
                        break;
                }
            }

            const auto detail = stateText + "  |  " + insert.format;
            graphics.setColour(stateColour);
            graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
            graphics.drawFittedText(detail,
                                    8,
                                    y + 24,
                                    getWidth() - 76,
                                    15,
                                    juce::Justification::centredLeft,
                                    1);

            graphics.setColour(juce::Colour(insert.bypassed ? StudioColours::amber
                                                            : StudioColours::secondaryText));
            graphics.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            graphics.drawText("BYP",
                              getWidth() - 64,
                              y,
                              32,
                              48,
                              juce::Justification::centred);
            graphics.setColour(juce::Colour(StudioColours::orange));
            graphics.drawText("X",
                              getWidth() - 30,
                              y,
                              30,
                              48,
                              juce::Justification::centred);
            y += 54;
        }

        if (lateBlockCount > 0)
        {
            graphics.setColour(juce::Colour(StudioColours::amber));
            graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
            graphics.drawText(juce::String(lateBlockCount) + " late sandbox blocks",
                              0,
                              getHeight() - 18,
                              getWidth(),
                              18,
                              juce::Justification::centredLeft);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
        if (track == nullptr || event.position.y < 24.0f)
            return;

        const auto index = static_cast<int>((event.position.y - 24.0f) / 54.0f);
        if (index < 0 || index >= static_cast<int>(track->inserts.size()))
            return;

        const auto& insert = track->inserts[static_cast<std::size_t>(index)];
        const auto failed = std::any_of(runtimeStatuses.cbegin(),
                                        runtimeStatuses.cend(),
                                        [&insert](const auto& status)
        {
            return status.insertId == insert.id
                && status.state == StudioAudioEngine::PluginRuntimeStatus::State::failed;
        });
        if (event.position.x >= static_cast<float>(getWidth() - 30))
        {
            if (onRemove)
                onRemove(track->id, insert.id);
        }
        else if (event.position.x >= static_cast<float>(getWidth() - 64))
        {
            if (onBypass)
                onBypass(track->id, insert.id, !insert.bypassed);
        }
        else if (failed && onReload)
        {
            onReload();
        }
    }

private:
    const Project* project = nullptr;
    juce::String trackId;
    std::vector<StudioAudioEngine::PluginRuntimeStatus> runtimeStatuses;
    std::uint64_t lateBlockCount = 0;
};

MainComponent::MainComponent()
{
    setLookAndFeel(&theme);
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addKeyListener(this);

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
    configureButton(addTrackButton, "Add an audio track");
    configureButton(importButton, "Import WAV, AIFF, FLAC, or MP3 audio");
    configureButton(duplicateTrackButton, "Duplicate the selected track and its edits");
    configureButton(deleteTrackButton, "Delete the selected track");
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
    addTrackButton.onClick = [this] { addAudioTrack(); };
    importButton.onClick = [this] { beginImportAudio(); };
    duplicateTrackButton.onClick = [this] { duplicateSelectedTrack(); };
    deleteTrackButton.onClick = [this] { deleteSelectedTrack(); };
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
        project.loopEnabled = loopButton.getToggleState();
        projectChanged();
    };

    metronomeButton.setToggleState(true, juce::dontSendNotification);
    metronomeButton.onClick = [this]
    {
        audioEngine.setMetronomeEnabled(metronomeButton.getToggleState());
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
    tempoSlider.onValueChange = [this]
    {
        project.tempo = tempoSlider.getValue();
        projectChanged();
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
        if (track == nullptr || track->type != TrackType::audio)
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
        if (perform(std::make_unique<MoveClipCommand>(clipId,
                                                      start,
                                                      destinationTrackId)))
            selectClip(destinationTrackId, clipId);
    };
    timeline.onClipTrimmed = [this](const auto& clipId,
                                    double start,
                                    double sourceOffset,
                                    double duration)
    {
        perform(std::make_unique<TrimClipCommand>(clipId,
                                                  start,
                                                  sourceOffset,
                                                  duration));
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

    mixer = std::make_unique<MixerPanel>();
    mixer->setProject(&project);
    mixer->onTrackSelected = [this](const auto& trackId) { selectTrack(trackId); };
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

    insertPanel = std::make_unique<InsertPanel>();
    insertPanel->setProject(&project);
    insertPanel->onBypass = [this](const auto& trackId, const auto& insertId, bool bypassed)
    {
        perform(std::make_unique<SetPluginBypassCommand>(trackId, insertId, bypassed));
    };
    insertPanel->onRemove = [this](const auto& trackId, const auto& insertId)
    {
        perform(std::make_unique<RemovePluginInsertCommand>(trackId, insertId));
    };
    insertPanel->onReload = [this]
    {
        audioEngine.forcePluginRuntimeReload(project, pluginRuntimeRequests());
        setStatus("Reloading sandboxed plugin workers...");
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
    auto brand = topRow.removeFromLeft(120);
    projectLabel.setBounds(brand.withTrimmedTop(24).withHeight(28));

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
    importButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    duplicateTrackButton.setBounds(sessionPanel.removeFromTop(34));
    sessionPanel.removeFromTop(8);
    deleteTrackButton.setBounds(sessionPanel.removeFromTop(34));
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
    inspector.removeFromTop(12);
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
    inspector.removeFromTop(20);
    insertPanel->setBounds(inspector);

    updateTimelineSize();
    timeline.setViewportPosition(timelineViewport.getViewPositionX());
}

void MainComponent::timerCallback()
{
    if (!activeRecordingTargets.empty()
        && !audioEngine.isPlaying()
        && !recordingFinalizationInProgress)
    {
        finishRecording();
        return;
    }

    const auto position = audioEngine.positionSeconds();
    timeline.setPlayheadSeconds(position);
    positionLabel.setText(positionText(position,
                                       project.tempo,
                                       project.timeSignatureNumerator),
                          juce::dontSendNotification);
    const auto playing = audioEngine.isPlaying();
    playButton.setButtonText(playing ? "PAUSE" : "PLAY");
    const auto recording = !activeRecordingTargets.empty();
    recordButton.setButtonText(recording ? "STOP REC" : "REC");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           juce::Colour(recording ? StudioColours::orange
                                                 : StudioColours::raised));
    mixer->setPeaks(audioEngine.leftPeak(), audioEngine.rightPeak());
    auto runtimeStatuses = audioEngine.pluginRuntimeStatuses();
    auto runtimeMetadataChanged = false;
    for (const auto& status : runtimeStatuses)
    {
        if (status.state != StudioAudioEngine::PluginRuntimeStatus::State::ready)
            continue;

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

            if (insert->latencySamples != status.latencySamples
                || std::abs(insert->tailSeconds - status.tailSeconds) > 0.000001)
            {
                insert->latencySamples = status.latencySamples;
                insert->tailSeconds = status.tailSeconds;
                runtimeMetadataChanged = true;
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
    project = Project::createDefault();
    projectPackage = juce::File();
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

    const auto result = ProjectFile::save(project, normalised);
    if (result.failed())
    {
        showError("Project save failed", result.getErrorMessage());
        return;
    }

    projectPackage = normalised;
    dirty = false;
    projectLabel.setText(project.name, juce::dontSendNotification);
    setStatus("Saved " + projectPackage.getFullPathName());
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
    project = std::move(*loaded);
    projectPackage = ProjectFile::normalisePackagePath(package);
    commandStack.clear();
    selectedClipId.clear();
    selectedTrackId = project.tracks.empty() ? juce::String() : project.tracks.front().id;
    tempoSlider.setValue(project.tempo, juce::dontSendNotification);
    loopButton.setToggleState(project.loopEnabled, juce::dontSendNotification);
    dirty = false;
    selectTrack(selectedTrackId);
    projectChanged(false, false);
    setStatus("Opened " + projectPackage.getFullPathName());
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
    if (destination == nullptr || destination->type == TrackType::master)
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
                const auto stateFile = projectPackage.getChildFile(insert.stateFile);
                insert.missing = !projectPackage.exists()
                    || !stateFile.isAChildOf(projectPackage)
                    || !stateFile.existsAsFile();
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
            parent->inputChannel,
            parent->stereoInput ? 2 : 1
        });
        targets.push_back(std::move(target));
    }

    recordingStartSeconds = audioEngine.positionSeconds();
    const auto result = audioEngine.startRecording(requests);
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
    completedTracks.reserve(targets.size());
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

        AudioClip clip;
        clip.name = recording.file.getFileNameWithoutExtension();
        clip.sourceFile = recording.file;
        clip.startSeconds = recordingStartSeconds;
        clip.durationSeconds = recording.durationSeconds;
        clip.sourceLengthSeconds = recording.durationSeconds;
        clip.sourceRangeEndSeconds = recording.durationSeconds;
        clip.colour = target.versionTrack.colour;
        if (firstClipId.isEmpty())
            firstClipId = clip.id;
        target.versionTrack.clips.push_back(std::move(clip));
        completedTracks.push_back(std::move(target.versionTrack));

        if (warning.isEmpty() && recording.warning.isNotEmpty())
            warning = recording.warning;
    }

    const auto firstTrackId = completedTracks.front().id;
    if (!perform(std::make_unique<AddRecordingTakeCommand>(std::move(completedTracks))))
        return;
    selectClip(firstTrackId, firstClipId);
    const auto savedMessage = "Saved "
        + juce::String(static_cast<int>(recordings.size()))
        + (recordings.size() == 1 ? " track (" : " synchronized tracks (")
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
    Track track;
    const auto audioTrackCount = static_cast<int>(std::count_if(project.tracks.cbegin(),
                                                                project.tracks.cend(),
                                                                [](const auto& candidate)
    {
        return candidate.type == TrackType::audio;
    }));
    track.name = "Audio " + juce::String(audioTrackCount + 1);
    track.armed = true;
    const std::array colours {
        juce::Colour(0xffdd5b3f),
        juce::Colour(0xffd99a42),
        juce::Colour(0xff78c6a3),
        juce::Colour(0xff7da9d9),
        juce::Colour(0xffb47ac4)
    };
    track.colour = colours[static_cast<std::size_t>(audioTrackCount) % colours.size()];
    const auto trackId = track.id;
    if (perform(std::make_unique<AddTrackCommand>(track)))
        selectTrack(trackId);
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
    insert.fileOrIdentifier = entry.fileOrIdentifier;
    insert.bridgeMode = PluginBridgeMode::sandboxed;

    if (perform(std::make_unique<AddPluginInsertCommand>(track->id, insert)))
        setStatus(entry.name + " added as a sandboxed insert model.");
}

void MainComponent::splitSelectedClip()
{
    if (selectedClipId.isEmpty())
    {
        setStatus("Select a clip before splitting.", true);
        return;
    }

    perform(std::make_unique<SplitClipCommand>(selectedClipId, audioEngine.positionSeconds()));
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
    perform(std::make_unique<TrimClipCommand>(clip->id,
                                              cursor,
                                              clip->sourceOffsetSeconds + removedDuration,
                                              clip->durationSeconds - removedDuration));
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

    perform(std::make_unique<TrimClipCommand>(clip->id,
                                              clip->startSeconds,
                                              clip->sourceOffsetSeconds,
                                              cursor - clip->startSeconds));
}

void MainComponent::deleteSelectedClip()
{
    if (selectedClipId.isEmpty())
        return;

    if (perform(std::make_unique<DeleteClipCommand>(selectedClipId)))
    {
        selectedClipId.clear();
        timeline.setSelection(selectedTrackId, {});
        updateInspector();
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
    selectedTrackId = trackId;
    selectedClipId.clear();
    timeline.setSelection(selectedTrackId, selectedClipId);
    mixer->setSelection(selectedTrackId);
    insertPanel->setTrack(selectedTrackId);
    updateInspector();
}

void MainComponent::selectClip(const juce::String& trackId, const juce::String& clipId)
{
    selectedTrackId = trackId;
    selectedClipId = clipId;
    timeline.setSelection(selectedTrackId, selectedClipId);
    mixer->setSelection(selectedTrackId);
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
        return;
    }

    const auto canRenameTrack = clip == nullptr && track->type == TrackType::audio;
    updatingTrackName = true;
    inspectorName.setText(clip != nullptr ? clip->name : track->name, juce::dontSendNotification);
    updatingTrackName = false;
    inspectorName.setEditable(false, canRenameTrack, false);
    inspectorName.setTooltip(canRenameTrack
                                 ? "Double-click to rename this audio track"
                                 : juce::String());
    inspectorDetails.setText(clip != nullptr
                                 ? juce::String(clip->durationSeconds, 2)
                                     + " s  |  "
                                     + clip->sourceFile.getFileName()
                                 : trackTypeToString(track->type).toUpperCase(),
                             juce::dontSendNotification);
    volumeSlider.setEnabled(true);
    panSlider.setEnabled(true);
    muteButton.setEnabled(track->type != TrackType::master || !track->muted);
    soloButton.setEnabled(track->type != TrackType::master);
    armButton.setEnabled(track->type == TrackType::audio);
    trackColourButton.setEnabled(track->type == TrackType::audio);
    splitClipButton.setEnabled(clip != nullptr);
    deleteClipButton.setEnabled(clip != nullptr);
    trimClipStartButton.setEnabled(clip != nullptr);
    trimClipEndButton.setEnabled(clip != nullptr);
    inputSelector.setEnabled(track->type == TrackType::audio);
    stereoInputButton.setEnabled(track->type == TrackType::audio
                                 && track->inputChannel + 1 < inputSelector.getNumItems());
    monitorButton.setEnabled(track->type == TrackType::audio);
    volumeSlider.setValue(track->volumeDecibels, juce::dontSendNotification);
    panSlider.setValue(track->pan, juce::dontSendNotification);
    muteButton.setColour(juce::TextButton::buttonColourId,
                         juce::Colour(track->muted ? StudioColours::amber : StudioColours::raised));
    soloButton.setColour(juce::TextButton::buttonColourId,
                         juce::Colour(track->solo ? StudioColours::green : StudioColours::raised));
    armButton.setColour(juce::TextButton::buttonColourId,
                        juce::Colour(track->armed ? StudioColours::orange : StudioColours::raised));
    armButton.setButtonText(track->armed ? "ARMED" : "ARM");
    const auto colourButtonBackground = track->type == TrackType::audio
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

void MainComponent::refreshInputControls()
{
    const auto selectedIndex = inputSelector.getSelectedItemIndex();
    updatingInputControls = true;
    inputSelector.clear(juce::dontSendNotification);

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
    }

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
    if (track == nullptr || track->type != TrackType::audio)
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
    insertPanel->setProject(&project);
    projectLabel.setText(project.name + (dirty ? " *" : ""), juce::dontSendNotification);
    loopButton.setToggleState(project.loopEnabled, juce::dontSendNotification);
    undoButton.setEnabled(commandStack.canUndo());
    redoButton.setEnabled(commandStack.canRedo());
    updateInspector();
    updateTimelineSize();

    if (const auto result = audioEngine.updateProject(project,
                                                      pluginRuntimeRequests()); result.failed())
        setStatus(result.getErrorMessage(), true);
    updateInputMonitoring();

    if (writeRecovery && projectPackage.exists())
        if (const auto result = ProjectFile::writeRecoveryPoint(project, projectPackage); result.failed())
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
            request.latencySamples = insert.latencySamples;
            request.tailSeconds = insert.tailSeconds;
            request.catalogRevision = pluginCatalog.revision();
            request.description = pluginCatalog.descriptionForIdentifier(insert.pluginIdentifier);
            if (!request.description.has_value())
                request.missing = true;

            if (insert.stateFile.isNotEmpty())
            {
                const auto stateFile = projectPackage.getChildFile(insert.stateFile);
                if (!projectPackage.exists()
                    || !stateFile.isAChildOf(projectPackage)
                    || !stateFile.existsAsFile()
                    || !stateFile.loadFileAsData(request.state))
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

juce::String MainComponent::positionText(double seconds, double tempo, int beatsPerBar)
{
    const auto totalBeats = seconds * tempo / 60.0;
    const auto bar = static_cast<int>(totalBeats / beatsPerBar) + 1;
    const auto beat = static_cast<int>(totalBeats) % beatsPerBar + 1;
    const auto ticks = static_cast<int>(std::fmod(totalBeats, 1.0) * 960.0);
    const auto minutes = static_cast<int>(seconds) / 60;
    const auto wholeSeconds = static_cast<int>(seconds) % 60;
    const auto milliseconds = static_cast<int>(std::fmod(seconds, 1.0) * 1000.0);

    return juce::String(bar).paddedLeft('0', 3)
        + " | "
        + juce::String(beat).paddedLeft('0', 2)
        + " | "
        + juce::String(ticks).paddedLeft('0', 3)
        + "    "
        + juce::String(minutes).paddedLeft('0', 2)
        + ":"
        + juce::String(wholeSeconds).paddedLeft('0', 2)
        + "."
        + juce::String(milliseconds).paddedLeft('0', 3);
}
}
