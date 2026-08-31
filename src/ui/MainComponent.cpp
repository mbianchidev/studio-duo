#include "MainComponent.h"

#include <algorithm>
#include <cmath>

namespace studio
{
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

            const auto faderTop = strip.getY() + 44;
            const auto faderHeight = strip.getHeight() - 82;
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
                              strip.getBottom() - 26,
                              strip.getWidth() - 16,
                              18,
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
        if (index >= 0 && index < static_cast<int>(project->tracks.size()) && onTrackSelected)
            onTrackSelected(project->tracks[static_cast<std::size_t>(index)].id);
    }

private:
    const Project* project = nullptr;
    juce::String selectedTrack;
    float leftPeak = 0.0f;
    float rightPeak = 0.0f;
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

    std::function<void(const juce::String&, const juce::String&, bool)> onBypass;
    std::function<void(const juce::String&, const juce::String&)> onRemove;

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

            juce::String detail = insert.missing ? "MISSING" : "SANDBOX";
            detail += "  |  " + insert.format;
            graphics.setColour(juce::Colour(insert.missing ? StudioColours::orange
                                                           : StudioColours::green));
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
    }

private:
    const Project* project = nullptr;
    juce::String trackId;
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
    configureButton(stereoInputButton, "Capture this input and the following input as stereo");
    configureButton(monitorButton, "Monitor the selected track input through Studio Duo");

    newButton.onClick = [this] { createNewProject(); };
    openButton.onClick = [this] { beginOpenProject(); };
    saveButton.onClick = [this] { beginSaveProject(); };
    exportButton.onClick = [this] { beginExportMix(); };
    audioSetupButton.onClick = [this] { showAudioSettings(); };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    playButton.onClick = [this] { togglePlayback(); };
    stopButton.onClick = [this]
    {
        if (audioEngine.isRecording())
            finishRecording();
        audioEngine.stop();
    };
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
    stereoInputButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.stereoInput = !state.stereoInput; });
    };
    monitorButton.onClick = [this]
    {
        changeSelectedTrackState([](auto& state) { state.inputMonitoring = !state.inputMonitoring; });
    };

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
    volumeSlider.setTextValueSuffix(" dB");
    configureInspectorSlider(panSlider, -1.0, 1.0, 0.01);

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
    timeline.onAddTrack = [this] { addAudioTrack(); };
    timeline.onClipSelected = [this](const auto& trackId, const auto& clipId)
    {
        selectClip(trackId, clipId);
    };
    timeline.onClipMoved = [this](const auto& clipId, double start)
    {
        perform(std::make_unique<MoveClipCommand>(clipId, start));
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
        audioEngine.seekSeconds(seconds);
        timeline.setPlayheadSeconds(seconds);
    };

    mixer = std::make_unique<MixerPanel>();
    mixer->setProject(&project);
    mixer->onTrackSelected = [this](const auto& trackId) { selectTrack(trackId); };
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
    const auto mixerTop = getHeight() - 204;
    graphics.setColour(juce::Colour(StudioColours::panel));
    graphics.fillRect(0, bodyTop, 286, mixerTop - bodyTop);
    graphics.fillRect(getWidth() - 250, bodyTop, 250, mixerTop - bodyTop);
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawVerticalLine(285, static_cast<float>(bodyTop), static_cast<float>(mixerTop));
    graphics.drawVerticalLine(getWidth() - 251,
                              static_cast<float>(bodyTop),
                              static_cast<float>(mixerTop));

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
    auto mixerBounds = bounds.removeFromBottom(176);
    auto left = bounds.removeFromLeft(286);
    auto right = bounds.removeFromRight(250);

    statusLabel.setBounds(status.reduced(10, 0));
    mixer->setBounds(mixerBounds);
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
    volumeLabel.setBounds(inspector.removeFromTop(20));
    volumeSlider.setBounds(inspector.removeFromTop(36));
    inspector.removeFromTop(12);
    panLabel.setBounds(inspector.removeFromTop(20));
    panSlider.setBounds(inspector.removeFromTop(36));
    inspector.removeFromTop(20);
    auto toggles = inspector.removeFromTop(34);
    muteButton.setBounds(toggles.removeFromLeft(68).reduced(2));
    soloButton.setBounds(toggles.removeFromLeft(68).reduced(2));
    armButton.setBounds(toggles.removeFromLeft(68).reduced(2));
    inspector.removeFromTop(20);
    insertPanel->setBounds(inspector);

    updateTimelineSize();
}

void MainComponent::timerCallback()
{
    const auto position = audioEngine.positionSeconds();
    timeline.setPlayheadSeconds(position);
    positionLabel.setText(positionText(position,
                                       project.tempo,
                                       project.timeSignatureNumerator),
                          juce::dontSendNotification);
    playButton.setButtonText(audioEngine.isPlaying() ? "PAUSE" : "PLAY");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           juce::Colour(audioEngine.isRecording() ? StudioColours::orange
                                                                 : StudioColours::raised));
    mixer->setPeaks(audioEngine.leftPeak(), audioEngine.rightPeak());

    auto* device = deviceManager.getCurrentAudioDevice();
    juce::AudioDeviceManager::AudioDeviceSetup audioSetup;
    deviceManager.getAudioDeviceSetup(audioSetup);
    const auto signature = device != nullptr
        ? audioSetup.inputDeviceName
            + ":"
            + device->getInputChannelNames().joinIntoString("|")
            + ":"
            + device->getActiveInputChannels().toString(16)
        : juce::String();
    if (signature != inputConfigurationSignature)
    {
        inputConfigurationSignature = signature;
        refreshInputControls();
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

    if (!command && key.getKeyCode() == 'S')
    {
        splitSelectedClip();
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::deleteKey
        || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        deleteSelectedClip();
        return true;
    }

    return false;
}

void MainComponent::createNewProject()
{
    if (audioEngine.isRecording())
        finishRecording();

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

    if (audioEngine.isRecording())
        finishRecording();
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
    clip.colour = destination->colour;

    const auto clipId = clip.id;
    if (perform(std::make_unique<AddClipCommand>(destination->id, clip)))
        selectClip(destination->id, clipId);
}

void MainComponent::exportMixTo(const juce::File& destination)
{
    setStatus("Rendering " + destination.getFileName() + "...");
    const auto result = audioEngine.renderToWav(project, destination, 48000.0);
    if (result.failed())
    {
        showError("Export failed", result.getErrorMessage());
        return;
    }

    setStatus("Exported 48 kHz / 24-bit WAV to " + destination.getFullPathName());
}

void MainComponent::togglePlayback()
{
    audioEngine.isPlaying() ? audioEngine.pause() : audioEngine.play();
}

void MainComponent::toggleRecording()
{
    if (audioEngine.isRecording())
    {
        finishRecording();
        return;
    }

    auto* track = recordingTrack();
    if (track == nullptr)
    {
        const auto* selected = project.findTrack(selectedTrackId);
        if (selected != nullptr && selected->type == TrackType::audio)
        {
            const auto before = TrackMixState::fromTrack(*selected);
            auto after = before;
            after.armed = true;
            if (perform(std::make_unique<SetTrackMixCommand>(selected->id, before, after)))
                track = project.findTrack(selectedTrackId);
        }
    }

    if (track == nullptr)
    {
        showError("Recording unavailable", "Select an audio track or add a new one first.");
        return;
    }

    auto folder = projectPackage.exists()
        ? projectPackage.getChildFile("media")
        : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
              .getChildFile("Studio Duo Recordings");
    folder.createDirectory();
    activeRecording = folder.getNonexistentChildFile(
        "Recording-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S"),
        ".wav",
        false);

    const auto result = audioEngine.startRecording(activeRecording,
                                                   track->inputChannel,
                                                   track->stereoInput ? 2 : 1);
    if (result.failed())
    {
        showError("Recording unavailable", result.getErrorMessage());
        return;
    }

    activeRecordingTrackId = track->id;
    recordingStartSeconds = audioEngine.positionSeconds();
    audioEngine.play();
    setStatus("Recording " + track->name + ". Press REC or STOP to finish.");
}

void MainComponent::finishRecording()
{
    auto* track = project.findTrack(activeRecordingTrackId);
    const auto recording = audioEngine.stopRecording();
    audioEngine.pause();
    activeRecordingTrackId.clear();
    if (recording.result.failed())
    {
        showError("Recording warning", recording.result.getErrorMessage());
        return;
    }

    if (track == nullptr || recording.durationSeconds <= 0.0)
    {
        setStatus("Recording stopped without audio.", true);
        return;
    }

    AudioClip clip;
    clip.name = recording.file.getFileNameWithoutExtension();
    clip.sourceFile = recording.file;
    clip.startSeconds = recordingStartSeconds;
    clip.durationSeconds = recording.durationSeconds;
    clip.sourceLengthSeconds = recording.durationSeconds;
    clip.colour = track->colour;
    const auto clipId = clip.id;

    if (perform(std::make_unique<AddClipCommand>(track->id, clip)))
        selectClip(track->id, clipId);
    setStatus("Recorded " + juce::String(recording.durationSeconds, 2) + " seconds.");
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
    if (audioEngine.isRecording() && activeRecordingTrackId == selectedTrackId)
    {
        setStatus("Stop recording before deleting its track.", true);
        return;
    }

    const auto trackToDelete = selectedTrackId;
    if (!perform(std::make_unique<RemoveTrackCommand>(trackToDelete)))
        return;

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
    if (!commandStack.undo(project))
        return;

    selectedClipId.clear();
    projectChanged();
}

void MainComponent::redo()
{
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
        inspectorName.setText("No selection", juce::dontSendNotification);
        inspectorDetails.setText({}, juce::dontSendNotification);
        volumeSlider.setEnabled(false);
        panSlider.setEnabled(false);
        muteButton.setEnabled(false);
        soloButton.setEnabled(false);
        armButton.setEnabled(false);
        inputSelector.setEnabled(false);
        stereoInputButton.setEnabled(false);
        monitorButton.setEnabled(false);
        return;
    }

    inspectorName.setText(clip != nullptr ? clip->name : track->name, juce::dontSendNotification);
    inspectorDetails.setText(clip != nullptr
                                 ? juce::String(clip->durationSeconds, 2) + " s  |  " + track->name
                                 : trackTypeToString(track->type).toUpperCase(),
                             juce::dontSendNotification);
    volumeSlider.setEnabled(true);
    panSlider.setEnabled(true);
    muteButton.setEnabled(track->type != TrackType::master || !track->muted);
    soloButton.setEnabled(track->type != TrackType::master);
    armButton.setEnabled(track->type == TrackType::audio);
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

    if (const auto result = audioEngine.updateProject(project); result.failed())
        setStatus(result.getErrorMessage(), true);
    updateInputMonitoring();

    if (writeRecovery && projectPackage.exists())
        if (const auto result = ProjectFile::writeRecoveryPoint(project, projectPackage); result.failed())
            setStatus(result.getErrorMessage(), true);
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
