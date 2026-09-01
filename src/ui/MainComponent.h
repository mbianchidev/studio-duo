#pragma once

#include "StudioTheme.h"
#include "MixerPanel.h"
#include "PluginInsertPanel.h"
#include "RoutingPanel.h"
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
    void addBusTrack();
    void addTrack(TrackType type);
    void showAddTrackMenu();
    void duplicateSelectedTrack();
    void deleteSelectedTrack();
    void addPluginToSelectedTrack(const PluginCatalogEntry& entry);
    void splitSelectedClip();
    void trimSelectedClipStartToPlayhead();
    void trimSelectedClipEndToPlayhead();
    void deleteSelectedClip();
    void moveClip(const juce::String& clipId,
                  const juce::String& destinationTrackId,
                  double startSeconds);
    void trimClip(const juce::String& clipId,
                  double startSeconds,
                  double sourceOffsetSeconds,
                  double durationSeconds);
    void quantizeSelectedGroup();
    void analyseClipTransients(const juce::String& clipId);
    void setClipStretchMode(const juce::String& clipId, StretchMode mode);
    void setClipPlaybackRate(const juce::String& clipId, double rate);
    void warpClipTransient(const juce::String& clipId, double timelineSeconds);
    void setClipFade(const juce::String& clipId, double timelineSeconds, bool fadeIn);
    void createClipCrossfade(const juce::String& clipId);
    void toggleClipPolarity(const juce::String& clipId);
    void toggleClipReverse(const juce::String& clipId);
    void consolidateClip(const juce::String& clipId);
    void undo();
    void redo();
    void selectTrack(const juce::String& trackId);
    void selectClip(const juce::String& trackId, const juce::String& clipId);
    void updateInspector();
    void refreshInputControls();
    void refreshOutputControls();
    void updateInputMonitoring();
    void showTrackColourMenu();
    void showTrackQuickEditor(const juce::String& trackId,
                              juce::Rectangle<int> targetScreenArea);
    void showTrackingMenu();
    void promptTempoChange();
    void promptMeterChange();
    void createPluginTonePath(const juce::String& sourceTrackId);
    void updateTimelineSize();
    void zoomTimeline(double factor, bool reset = false);
    void projectChanged(bool writeRecovery = true, bool markDirty = true);
    [[nodiscard]] std::vector<StudioAudioEngine::PluginRuntimeRequest> pluginRuntimeRequests() const;
    bool perform(std::unique_ptr<ProjectCommand> command);
    void changeSelectedTrackState(const std::function<void(TrackMixState&)>& change);
    void changeTransportState(const std::function<void(ProjectTransportState&)>& change);
    void changeEditGroups(const std::function<void(std::vector<EditGroup>&)>& change);
    void changeReampRoutes(const std::function<void(std::vector<ReampRoute>&)>& change);
    [[nodiscard]] const AudioClip* activeClipAt(const juce::String& parentTrackId,
                                                double seconds) const;
    [[nodiscard]] std::vector<juce::String> linkedClipIdsAt(
        const juce::String& clipId,
        double seconds) const;
    bool updateLinkedClips(
        const juce::String& clipId,
        const juce::String& commandName,
        const std::function<bool(AudioClip&, const AudioClip&, juce::String&)>& update);
    [[nodiscard]] Track makeRecordingVersionTrack(const Track& parent) const;
    Track* recordingTrack();
    void setStatus(const juce::String& message, bool error = false);
    void showError(const juce::String& title, const juce::String& message);
    static juce::String positionText(double seconds, const Project& project);

    StudioTheme theme;
    std::unique_ptr<juce::Drawable> brandLogo;
    juce::AudioDeviceManager deviceManager;
    StudioAudioEngine audioEngine;
    Project project { Project::createDefault() };
    CommandStack commandStack;
    juce::File projectPackage;
    std::vector<ActiveRecordingTarget> activeRecordingTargets;
    RecordingPlan activeRecordingPlan;
    double recordingStartSeconds = 0.0;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    bool dirty = false;
    bool statusIsError = false;
    bool recordingFinalizationInProgress = false;
    bool playAfterRuntimeTransition = false;
    bool updatingInputControls = false;
    bool updatingOutputControls = false;
    bool updatingTrackName = false;
    bool tempoEditActive = false;
    ProjectTransportState tempoEditStart;
    juce::String inputConfigurationSignature;
    juce::String calibratingReampRouteId;
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

    juce::TextButton addTrackButton { "+ TRACK" };
    juce::TextButton addBusButton { "+ BUS TRACK" };
    juce::TextButton importButton { "IMPORT AUDIO" };
    juce::TextButton duplicateTrackButton { "DUPLICATE TRACK" };
    juce::TextButton deleteTrackButton { "DELETE TRACK" };
    juce::TextButton trackingButton { "TRACKING SETUP" };

    juce::Label inspectorName;
    juce::Label inspectorDetails;
    juce::Label inputLabel;
    juce::ComboBox inputSelector;
    juce::ToggleButton stereoInputButton { "STEREO" };
    juce::ToggleButton monitorButton { "MONITOR" };
    juce::Label outputLabel;
    juce::ComboBox outputSelector;
    std::vector<juce::String> outputTrackIds;
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
    std::unique_ptr<RoutingPanel> routingPanel;
    std::unique_ptr<PluginInsertPanel> insertPanel;
    juce::Label statusLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::TooltipWindow tooltipWindow { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
