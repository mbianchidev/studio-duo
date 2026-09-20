#pragma once

#include "StudioIconButton.h"
#include "StudioPanControl.h"
#include "StudioTheme.h"
#include "AutomationPanel.h"
#include "MixerPanel.h"
#include "MasteringWorkspaceComponent.h"
#include "MidiEditorComponent.h"
#include "PluginInsertPanel.h"
#include "PluginParameterPanel.h"
#include "RoutingPanel.h"
#include "TimelineComponent.h"
#include "audio/StudioAudioDeviceManager.h"
#include "audio/StudioAudioEngine.h"
#include "dawproject_io/DawProjectIO.h"
#include "model/LinkedEditModel.h"
#include "model/ProjectCommands.h"
#include "model/TransportEditing.h"
#include "plugin_host/PluginBrowserComponent.h"
#include "plugin_host/PluginCatalog.h"
#include "project_io/ProjectFile.h"
#include "render/RenderEngine.h"
#include "update/UpdateService.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <optional>
#include <vector>

namespace studio
{
class StudioPreferences;

class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::KeyListener,
                            private UpdateService::Listener
{
public:
    explicit MainComponent(bool startAudioOnLaunch = true);
    ~MainComponent() override;

    [[nodiscard]] bool hasAudioDeviceManager() const noexcept;
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

    struct ActiveAutomationGesture
    {
        AutomationTarget target;
        juce::String laneName;
        double startSeconds = 0.0;
        double startValue = 0.0;
    };

    struct AutomationPreview
    {
        AutomationTarget target;
        juce::String laneName;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double startValue = 0.0;
        double endValue = 0.0;
    };

    void timerCallback() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override;

    void initialiseAudio();
    bool ensureAudioDeviceManager();
    [[nodiscard]] juce::AudioIODevice* currentAudioDevice() const noexcept;
    bool connectAudioEngine();
    void createNewProject();
    void beginOpenProject();
    void beginSaveProject();
    void beginImportAudio();
    void beginExportMix();
    void chooseMixExportDestination(MixExportSettings settings);
    void finishMixExport(const juce::File& destination,
                         const juce::Result& result,
                         const MixExportSettings& settings);
    void setMasteringWorkspaceVisible(bool visible);
    void showExportMenu();
    void beginImportDawProject();
    void chooseDawProjectImportDestination(
        const juce::File& sourceArchive);
    void importDawProjectTo(const juce::File& sourceArchive,
                            const juce::File& destinationPackage);
    void beginExportDawProject();
    void exportDawProjectTo(const juce::File& destinationArchive);
    void showLatestCompatibilityReport();
    void beginSaveCompatibilityReport();
    [[nodiscard]] const CompatibilityReport*
        latestCompatibilityReport() const noexcept;
    void recordCompatibilityReport(
        const CompatibilityReport& report);
    void showSettings(bool showUpdates = false);
    void restartForUpdate();
    void updateStateChanged(
        const UpdateSnapshot& snapshot) override;
    void maybePromptForUpdate();
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
    void exportMixTo(const juce::File& destination,
                     MixExportSettings settings);
    void togglePlayback();
    void toggleRecording();
    void stopTransportAndRecording();
    void finishRecording();
    void completeRecording(std::vector<ActiveRecordingTarget> targets,
                           std::vector<StudioAudioEngine::RecordingResult> recordings,
                           std::vector<juce::String> midiTrackIds,
                           std::optional<StudioAudioEngine::MidiRecordingResult>
                               midiRecording);
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
    void validateInstalledPlugins();
    void changePluginMode(const juce::String& trackId,
                          const juce::String& insertId,
                          PluginBridgeMode mode);
    void showPluginParameters(const juce::String& trackId,
                              const juce::String& insertId);
    void splitSelectedClip();
    void trimSelectedClipStartToPlayhead();
    void trimSelectedClipEndToPlayhead();
    void copySelectedClip();
    void pasteCopiedClip();
    void duplicateSelectedClip();
    void duplicateClip(const juce::String& clipId);
    void deleteSelectedClip();
    void createMidiClip(const juce::String& trackId, double startSeconds);
    void editMidiClip(const juce::String& trackId,
                      const MidiClip& before,
                      const MidiClip& after,
                      const juce::String& commandName);
    void captureRetrospectiveMidi();
    void importDrumMap();
    void editDrumMapEntry(int pitch);
    void humanizeSelectedMidiClip();
    void applyMidiRoutingTemplate(const juce::String& templateId);
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
    void showTrackColourMenu();
    void showTrackQuickEditor(const juce::String& trackId,
                              juce::Rectangle<int> targetScreenArea);
    void showTrackingMenu();
    void showLoopSettings();
    bool applyLoopSettings(const LoopRangeSettings& settings);
    void promptProjectMarker(double position,
                             const juce::String& markerId = {});
    void moveProjectMarker(const juce::String& markerId,
                           double position);
    void removeProjectMarker(const juce::String& markerId);
    void showSectionSettings(const juce::String& sectionId);
    void setSongSectionRange(const juce::String& sectionId,
                             double startSeconds,
                             double endSeconds);
    void removeSongSection(const juce::String& sectionId);
    void showAutomationPanel();
    void promptSongSection(double position,
                           const juce::String& sectionId = {});
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
    void zoomTimeline(double factor,
                      bool reset = false,
                      std::optional<double> focalSeconds = std::nullopt);
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
    void beginAutomationGesture(AutomationTarget target,
                                juce::String laneName,
                                double normalizedValue);
    void endAutomationGesture(const AutomationTarget& target,
                              juce::String laneName,
                              double normalizedValue);
    void commitAutomationPreview();
    void recordAutomationGesture(const AutomationTarget& target,
                                 juce::String laneName,
                                 double startSeconds,
                                 double endSeconds,
                                 double startValue,
                                 double endValue,
                                 std::optional<AutomationMode> modeOverride =
                                     std::nullopt);
    [[nodiscard]] LinkedClipSelection linkedClipsAt(
        const juce::String& clipId,
        double seconds) const;
    bool updateLinkedClips(
        const juce::String& clipId,
        const juce::String& commandName,
        const std::function<bool(AudioClip&, const AudioClip&, juce::String&)>& update);
    [[nodiscard]] Track makeRecordingVersionTrack(const Track& parent) const;
    Track* recordingTrack();
    [[nodiscard]] bool hasActiveRecordingTargets() const noexcept;
    void setStatus(const juce::String& message, bool error = false);
    void showError(const juce::String& title, const juce::String& message);
    static juce::String positionText(double seconds, const Project& project);

    StudioTheme theme;
    std::unique_ptr<juce::Drawable> brandLogo;
    UpdateService updateService {
        STUDIO_DUO_VERSION,
        STUDIO_DUO_UPDATE_MANIFEST_URL
    };
    std::unique_ptr<StudioAudioDeviceManager> deviceManager;
    juce::String audioStartupError;
    StudioAudioEngine audioEngine;
    Project project { Project::createDefault() };
    CommandStack commandStack;
    juce::File projectPackage;
    std::vector<ActiveRecordingTarget> activeRecordingTargets;
    std::vector<juce::String> activeMidiRecordingTrackIds;
    std::optional<ActiveAutomationGesture> activeAutomationGesture;
    std::optional<AutomationPreview> pendingAutomationPreview;
    RecordingPlan activeRecordingPlan;
    double recordingStartSeconds = 0.0;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    juce::String copiedClipId;
    juce::String replacementInsertId;
    bool dirty = false;
    bool appShutdownPrepared = false;
    bool audioEngineInitialised = false;
    bool exportInProgress = false;
    bool shutdownRequestedDuringExport = false;
    MixExportSettings lastMixExportSettings;
    ExportInputBlocker exportInputBlocker;
    bool statusIsError = false;
    bool recordingFinalizationInProgress = false;
    bool playAfterRuntimeTransition = false;
    bool updatingInputControls = false;
    bool updatingOutputControls = false;
    bool updatingTrackName = false;
    bool inspectorVolumeGestureActive = false;
    bool inspectorPanGestureActive = false;
    bool tempoEditActive = false;
    ProjectTransportState tempoEditStart;
    juce::String inputConfigurationSignature;
    int inputConfigurationPollTicks = 29;
    juce::String calibratingReampRouteId;
    std::uint64_t lastRuntimeCatalogRevision = 0;
    juce::String reducedIsolationMarkerSignature;
    std::optional<CompatibilityReport> transientCompatibilityReport;
    juce::ThreadPool compatibilityValidator { 1 };
    juce::ThreadPool exportWorker { 1 };

    StudioIconButton newButton {
        StudioIcon::newFile, "New project", "Create a new project"
    };
    StudioIconButton openButton {
        StudioIcon::openFolder, "Open project", "Open a .studioduo project"
    };
    StudioIconButton saveButton {
        StudioIcon::save, "Save project", "Save project (Command/Ctrl+S)"
    };
    StudioIconButton exportButton {
        StudioIcon::exportFile,
        "Export",
        "Export audio, open mastering and release tools, or use DAWproject interchange"
    };
    StudioIconButton settingsButton {
        StudioIcon::settings,
        "Settings",
        "Configure audio, MIDI, and automatic updates"
    };
    StudioIconButton undoButton {
        StudioIcon::undo, "Undo", "Undo (Command/Ctrl+Z)"
    };
    StudioIconButton redoButton {
        StudioIcon::redo, "Redo", "Redo (Command/Ctrl+Shift+Z)"
    };
    StudioIconButton playButton {
        StudioIcon::play, "Play", "Play (Space)"
    };
    StudioIconButton stopButton {
        StudioIcon::stop,
        "Stop",
        "Stop playback; recordings stop at the current position"
    };
    StudioIconButton recordButton {
        StudioIcon::record,
        "Start recording",
        "Record armed audio, MIDI, and instrument tracks"
    };
    StudioIconButton loopButton {
        StudioIcon::loop,
        "Enable loop",
        "Enable the configured loop"
    };
    StudioIconButton loopRangeButton {
        StudioIcon::loopRange,
        "Configure loop range",
        "Configure loop start and end: seconds, musical positions, or markers"
    };
    StudioIconButton metronomeButton {
        StudioIcon::metronome,
        "Enable metronome",
        "Enable the metronome"
    };
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::Label meterLabel;
    juce::Label positionLabel;
    juce::Label projectLabel;

    StudioIconButton addTrackButton {
        StudioIcon::add,
        "Add track",
        "Add an audio, instrument, MIDI, aux, bus, folder, VCA, or control-room track"
    };
    StudioIconButton addBusButton {
        StudioIcon::bus, "Add bus track", "Add a stereo bus track"
    };
    StudioIconButton importButton {
        StudioIcon::importFile,
        "Import audio",
        "Import WAV, AIFF, FLAC, or MP3 audio"
    };
    StudioIconButton duplicateTrackButton {
        StudioIcon::duplicate,
        "Duplicate track",
        "Duplicate the selected track and its edits"
    };
    StudioIconButton deleteTrackButton {
        StudioIcon::deleteItem,
        "Delete track",
        "Delete the selected track"
    };
    StudioIconButton trackingButton {
        StudioIcon::marker,
        "Tracking setup",
        "Add or edit markers, song sections, tempo, meter, punch, count-in, and click routing"
    };
    StudioIconButton automationButton {
        StudioIcon::automation,
        "Automation",
        "Edit and record mixer and plugin automation"
    };
    StudioIconButton newMidiClipButton {
        StudioIcon::midi,
        "New MIDI clip",
        "Create an ordinary editable MIDI clip at the playhead (Command/Ctrl+Shift+N)"
    };
    StudioIconButton sessionPanelToggleButton {
        StudioIcon::tracks,
        "Tracks",
        "Show or hide the session tracks pane"
    };
    StudioIconButton inspectorPanelToggleButton {
        StudioIcon::inspect,
        "Inspector",
        "Show or hide the inspector"
    };
    StudioIconButton mixerPanelToggleButton {
        StudioIcon::mixer,
        "Mixer",
        "Show or hide the mixer"
    };

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
    StudioPanSlider panSlider;
    StudioIconButton muteButton {
        StudioIcon::mute,
        "Mute track",
        "Mute selected track"
    };
    StudioIconButton soloButton {
        StudioIcon::solo,
        "Solo track",
        "Solo selected track"
    };
    StudioIconButton armButton {
        StudioIcon::record,
        "Arm track",
        "Arm audio tracks for recording or MIDI and instrument tracks for live input"
    };
    juce::TextButton trackColourButton { "COLOR" };
    StudioIconButton splitClipButton {
        StudioIcon::split,
        "Split clip",
        "Split the selected clip at the playhead"
    };
    StudioIconButton deleteClipButton {
        StudioIcon::deleteItem,
        "Delete clip",
        "Delete the selected clip"
    };
    StudioIconButton trimClipStartButton {
        StudioIcon::trimStart,
        "Trim clip start",
        "Trim selected clip start to playhead ([)"
    };
    StudioIconButton trimClipEndButton {
        StudioIcon::trimEnd,
        "Trim clip end",
        "Trim selected clip end to playhead (])"
    };
    StudioIconButton zoomOutButton {
        StudioIcon::zoomOut,
        "Zoom out",
        "Zoom timeline out (Command/Ctrl+-)"
    };
    juce::TextButton zoomResetButton { "100%" };
    StudioIconButton zoomInButton {
        StudioIcon::zoomIn,
        "Zoom in",
        "Zoom timeline in (Command/Ctrl++)"
    };
    StudioIconButton snapButton {
        StudioIcon::snap,
        "Snap edits",
        "Snap clip, marker, and section edits to the selected grid"
    };
    juce::ComboBox editGridSelector;

    juce::Viewport timelineViewport;
    TimelineComponent timeline;
    MasteringWorkspaceComponent masteringWorkspace;
    std::unique_ptr<MixerPanel> mixer;
    MidiEditorComponent midiEditor;
    std::unique_ptr<PanelResizer> leftPanelResizer;
    std::unique_ptr<PanelResizer> inspectorPanelResizer;
    std::unique_ptr<PanelResizer> mixerPanelResizer;
    int leftPanelWidth = 286;
    int inspectorPanelWidth = 250;
    int mixerPanelHeight = 260;
    int midiEditorHeight = 330;
    bool leftPanelCollapsed = false;
    bool masteringWorkspaceVisible = false;
    PluginCatalog pluginCatalog;
    std::unique_ptr<PluginBrowserComponent> pluginBrowser;
    std::unique_ptr<RoutingPanel> routingPanel;
    std::unique_ptr<PluginInsertPanel> insertPanel;
    juce::Label statusLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::DialogWindow> settingsWindow;
    std::unique_ptr<StudioPreferences> preferences;
    UpdateSnapshot latestUpdateSnapshot;
    juce::String lastAvailabilityPromptVersion;
    juce::String lastReadyPromptVersion;
    bool updatePromptVisible = false;
    juce::TooltipWindow tooltipWindow { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
