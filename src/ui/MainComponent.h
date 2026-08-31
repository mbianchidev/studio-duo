#pragma once

#include "StudioTheme.h"
#include "TimelineComponent.h"
#include "audio/StudioAudioEngine.h"
#include "model/ProjectCommands.h"
#include "plugin_host/PluginBrowserComponent.h"
#include "plugin_host/PluginCatalog.h"
#include "project_io/ProjectFile.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <vector>

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
    class InsertPanel;

    struct ActiveRecordingTarget
    {
        juce::String parentTrackId;
        Track versionTrack;
        juce::File file;
    };

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
    void stopTransportAndRecording();
    void finishRecording();
    void completeRecording(std::vector<ActiveRecordingTarget> targets,
                           std::vector<StudioAudioEngine::RecordingResult> recordings);
    void addAudioTrack();
    void duplicateSelectedTrack();
    void deleteSelectedTrack();
    void addPluginToSelectedTrack(const PluginCatalogEntry& entry);
    void splitSelectedClip();
    void trimSelectedClipStartToPlayhead();
    void trimSelectedClipEndToPlayhead();
    void deleteSelectedClip();
    void undo();
    void redo();
    void selectTrack(const juce::String& trackId);
    void selectClip(const juce::String& trackId, const juce::String& clipId);
    void updateInspector();
    void refreshInputControls();
    void updateInputMonitoring();
    void showTrackColourMenu();
    void updateTimelineSize();
    void zoomTimeline(double factor, bool reset = false);
    void projectChanged(bool writeRecovery = true, bool markDirty = true);
    [[nodiscard]] std::vector<StudioAudioEngine::PluginRuntimeRequest> pluginRuntimeRequests() const;
    bool perform(std::unique_ptr<ProjectCommand> command);
    void changeSelectedTrackState(const std::function<void(TrackMixState&)>& change);
    [[nodiscard]] Track makeRecordingVersionTrack(const Track& parent) const;
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
    std::vector<ActiveRecordingTarget> activeRecordingTargets;
    double recordingStartSeconds = 0.0;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    bool dirty = false;
    bool statusIsError = false;
    bool recordingFinalizationInProgress = false;
    bool playAfterRuntimeTransition = false;
    bool updatingInputControls = false;
    bool updatingTrackName = false;
    juce::String inputConfigurationSignature;
    std::uint64_t lastRuntimeCatalogRevision = 0;

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
    juce::TextButton duplicateTrackButton { "DUPLICATE TRACK" };
    juce::TextButton deleteTrackButton { "DELETE TRACK" };

    juce::Label inspectorName;
    juce::Label inspectorDetails;
    juce::Label inputLabel;
    juce::ComboBox inputSelector;
    juce::ToggleButton stereoInputButton { "STEREO" };
    juce::ToggleButton monitorButton { "MONITOR" };
    juce::Label volumeLabel;
    juce::Label panLabel;
    juce::Slider volumeSlider;
    juce::Slider panSlider;
    juce::TextButton muteButton { "MUTE" };
    juce::TextButton soloButton { "SOLO" };
    juce::TextButton armButton { "ARM" };
    juce::TextButton trackColourButton { "COLOR" };
    juce::TextButton splitClipButton { "SPLIT @ PLAYHEAD" };
    juce::TextButton deleteClipButton { "DELETE CLIP" };
    juce::TextButton trimClipStartButton { "TRIM LEFT [" };
    juce::TextButton trimClipEndButton { "TRIM RIGHT ]" };
    juce::TextButton zoomOutButton { "-" };
    juce::TextButton zoomResetButton { "100%" };
    juce::TextButton zoomInButton { "+" };

    juce::Viewport timelineViewport;
    TimelineComponent timeline;
    std::unique_ptr<MixerPanel> mixer;
    PluginCatalog pluginCatalog;
    std::unique_ptr<PluginBrowserComponent> pluginBrowser;
    std::unique_ptr<InsertPanel> insertPanel;
    juce::Label statusLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::TooltipWindow tooltipWindow { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
