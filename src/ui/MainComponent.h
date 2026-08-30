#pragma once

#include "StudioTheme.h"
#include "TimelineComponent.h"
#include "audio/StudioAudioEngine.h"
#include "model/ProjectCommands.h"
#include "project_io/ProjectFile.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

namespace studio
{
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::KeyListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class MixerPanel;

    void timerCallback() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override;

    void createNewProject();
    void beginOpenProject();
    void beginSaveProject();
    void beginImportAudio();
    void beginExportMix();
    void showAudioSettings();
    void saveProjectTo(const juce::File& package);
    void openProjectFrom(const juce::File& package);
    void importAudioFile(const juce::File& source);
    void exportMixTo(const juce::File& destination);
    void togglePlayback();
    void toggleRecording();
    void finishRecording();
    void addAudioTrack();
    void splitSelectedClip();
    void deleteSelectedClip();
    void undo();
    void redo();
    void selectTrack(const juce::String& trackId);
    void selectClip(const juce::String& trackId, const juce::String& clipId);
    void updateInspector();
    void updateTimelineSize();
    void projectChanged(bool writeRecovery = true, bool markDirty = true);
    bool perform(std::unique_ptr<ProjectCommand> command);
    void changeSelectedTrackState(const std::function<void(TrackMixState&)>& change);
    Track* recordingTrack();
    void setStatus(const juce::String& message, bool error = false);
    void showError(const juce::String& title, const juce::String& message);
    static juce::String positionText(double seconds, double tempo, int beatsPerBar);

    StudioTheme theme;
    juce::AudioDeviceManager deviceManager;
    StudioAudioEngine audioEngine;
    Project project { Project::createDefault() };
    CommandStack commandStack;
    juce::File projectPackage;
    juce::File activeRecording;
    double recordingStartSeconds = 0.0;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    bool dirty = false;
    bool statusIsError = false;

    juce::TextButton newButton { "NEW" };
    juce::TextButton openButton { "OPEN" };
    juce::TextButton saveButton { "SAVE" };
    juce::TextButton exportButton { "EXPORT" };
    juce::TextButton audioSetupButton { "I/O" };
    juce::TextButton undoButton { "UNDO" };
    juce::TextButton redoButton { "REDO" };
    juce::TextButton playButton { "PLAY" };
    juce::TextButton stopButton { "STOP" };
    juce::TextButton recordButton { "REC" };
    juce::ToggleButton loopButton { "LOOP" };
    juce::ToggleButton metronomeButton { "CLICK" };
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::Label positionLabel;
    juce::Label projectLabel;

    juce::TextButton addTrackButton { "+ AUDIO TRACK" };
    juce::TextButton importButton { "IMPORT AUDIO" };

    juce::Label inspectorName;
    juce::Label inspectorDetails;
    juce::Label volumeLabel;
    juce::Label panLabel;
    juce::Slider volumeSlider;
    juce::Slider panSlider;
    juce::TextButton muteButton { "MUTE" };
    juce::TextButton soloButton { "SOLO" };
    juce::TextButton armButton { "ARM" };

    juce::Viewport timelineViewport;
    TimelineComponent timeline;
    std::unique_ptr<MixerPanel> mixer;
    juce::Label statusLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::TooltipWindow tooltipWindow { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
