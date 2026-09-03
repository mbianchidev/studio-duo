#pragma once

#include "StudioTheme.h"
#include "AutomationPanel.h"
#include "MixerPanel.h"
#include "PluginInsertPanel.h"
#include "PluginParameterPanel.h"
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

    bool prepareForShutdown();
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class PanelResizer;

    class ExportInputBlocker final : public juce::Component
    {
    public:
        bool keyPressed(const juce::KeyPress&) override
        {
            return true;
        }
    };

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
    bool captureCurrentPluginStates(
        const std::vector<juce::String>& trackIds,
        juce::String& error);
    bool materializePluginStateReferences(
        Project& projectToSave,
        const juce::File& sourcePackage,
        const juce::File& destinationPackage,
        juce::String& warning,
        juce::String& error) const;
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
    void openPluginEditor(const juce::String& trackId,
                          const juce::String& insertId);
    void validatePlugin(const PluginCatalogEntry& entry);
    void validateScreamForge();
    void changePluginMode(const juce::String& trackId,
                          const juce::String& insertId,
                          PluginBridgeMode mode);
    void showPluginParameters(const juce::String& trackId,
                              const juce::String& insertId);
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
    void setClipGain(const juce::String& clipId, float gainDecibels);
    void setClipFadeGesture(const juce::String& clipId,
                            bool fadeIn,
                            double durationSeconds,
                            float curve);
    void toggleClipMute(const juce::String& clipId);
    void createClipCrossfade(const juce::String& clipId);
    void toggleClipPolarity(const juce::String& clipId);
    void toggleClipReverse(const juce::String& clipId);
    void consolidateClip(const juce::String& clipId);
    void undo();
    void redo();
    void toggleExclusiveSolo(const juce::String& trackId);
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
    void showAutomationPanel();
    void promptTempoChange();
    void promptMeterChange();
    void createPluginTonePath(const juce::String& sourceTrackId);
    void captureToneSnapshot(const juce::String& routeId);
    void recallToneSnapshot(const juce::String& snapshotId);
    void renderToneSnapshots(const juce::String& routeId,
                             bool allSnapshots,
                             bool freeze,
                             bool print);
    void unfreezeToneSnapshot(const juce::String& snapshotId);
    void captureMixerSnapshot();
    void recallMixerSnapshot(const juce::String& snapshotId);
    void updateTimelineSize();
    void zoomTimeline(double factor, bool reset = false);
    void setLeftPanelCollapsed(bool collapsed);
    void setInspectorPanelVisible(bool visible);
    void setMixerPanelVisible(bool visible);
    void projectChanged(bool writeRecovery = true, bool markDirty = true);
    void updateReducedIsolationMarker();
    [[nodiscard]] std::vector<StudioAudioEngine::PluginRuntimeRequest> pluginRuntimeRequests() const;
    [[nodiscard]] std::vector<StudioAudioEngine::PluginRuntimeRequest>
        pluginRuntimeRequests(const Project& sourceProject) const;
    bool perform(std::unique_ptr<ProjectCommand> command);
    void changeSelectedTrackState(const std::function<void(TrackMixState&)>& change);
    void changeTransportState(const std::function<void(ProjectTransportState&)>& change);
    void changeEditGroups(const std::function<void(std::vector<EditGroup>&)>& change);
    void changeReampRoutes(const std::function<void(std::vector<ReampRoute>&)>& change);
    void recordTrackAutomation(AutomationTargetType type,
                               double normalizedValue);
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
    juce::String replacementInsertId;
    bool dirty = false;
    bool appShutdownPrepared = false;
    bool exportInProgress = false;
    ExportInputBlocker exportInputBlocker;
    bool statusIsError = false;
    bool recordingFinalizationInProgress = false;
    bool playAfterRuntimeTransition = false;
    bool updatingInputControls = false;
    bool updatingOutputControls = false;
    bool updatingTrackName = false;
    bool tempoEditActive = false;
    ProjectTransportState tempoEditStart;
    juce::String inputConfigurationSignature;
    int inputConfigurationPollTicks = 29;
    juce::String calibratingReampRouteId;
    std::uint64_t lastRuntimeCatalogRevision = 0;
    juce::String reducedIsolationMarkerSignature;
    juce::ThreadPool compatibilityValidator { 1 };

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
    juce::TextButton automationButton { "AUTOMATION" };
    juce::TextButton sessionPanelToggleButton { "<" };
    juce::TextButton inspectorPanelToggleButton { "INSPECT" };
    juce::TextButton mixerPanelToggleButton { "MIX" };

    juce::Component inspectorContent;
    juce::Viewport inspectorViewport;
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
    std::unique_ptr<PanelResizer> leftPanelResizer;
    std::unique_ptr<PanelResizer> inspectorPanelResizer;
    std::unique_ptr<PanelResizer> mixerPanelResizer;
    int leftPanelWidth = 286;
    int inspectorPanelWidth = 250;
    int mixerPanelHeight = 220;
    bool leftPanelCollapsed = false;
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
