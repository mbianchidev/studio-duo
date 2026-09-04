#pragma once

#include "ProjectModel.h"

#include <memory>
#include <utility>
#include <vector>

namespace studio
{
class ProjectCommand
{
public:
    virtual ~ProjectCommand() = default;
    [[nodiscard]] virtual juce::String name() const = 0;
    virtual bool perform(Project& project, juce::String& error) = 0;
    virtual void undo(Project& project) = 0;
};

class CommandStack
{
public:
    bool perform(std::unique_ptr<ProjectCommand> command, Project& project, juce::String& error);
    bool undo(Project& project);
    bool redo(Project& project, juce::String& error);
    void clear();

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] juce::String undoName() const;
    [[nodiscard]] juce::String redoName() const;

private:
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    std::size_t nextCommand = 0;
};

class BatchProjectCommand final : public ProjectCommand
{
public:
    BatchProjectCommand(juce::String commandName,
                        std::vector<std::unique_ptr<ProjectCommand>> commands);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String commandName;
    std::vector<std::unique_ptr<ProjectCommand>> commands;
};

class AddSongSectionCommand final : public ProjectCommand
{
public:
    explicit AddSongSectionCommand(SongSection sectionToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    SongSection section;
};

class AddTrackCommand final : public ProjectCommand
{
public:
    explicit AddTrackCommand(Track trackToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    Track track;
    std::optional<RoutingConnection> outputRoute;
    std::size_t insertionIndex = 0;
};

class RenameTrackCommand final : public ProjectCommand
{
public:
    RenameTrackCommand(juce::String trackToRename, juce::String replacementName);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    juce::String newName;
    juce::String oldName;
    bool capturedOriginal = false;
};

class AddClipCommand final : public ProjectCommand
{
public:
    AddClipCommand(juce::String destinationTrackId, AudioClip clipToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    AudioClip clip;
    std::size_t insertionIndex = 0;
};

class DuplicateClipCommand final : public ProjectCommand
{
public:
    explicit DuplicateClipCommand(juce::String clipToDuplicate);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;
    [[nodiscard]] const juce::String& duplicatedClipId() const noexcept;
    [[nodiscard]] const juce::String& duplicatedTrackId() const noexcept;

private:
    juce::String sourceClipId;
    juce::String trackId;
    AudioClip duplicatedClip;
    std::size_t insertionIndex = 0;
    bool createdDuplicate = false;
};

class AddRecordingTakeCommand final : public ProjectCommand
{
public:
    explicit AddRecordingTakeCommand(std::vector<Track> tracksToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    std::vector<Track> tracks;
    std::vector<std::pair<juce::String, bool>> parentCollapseStates;
    std::vector<std::pair<juce::String, juce::String>>
        parentActiveTakeStates;
    bool capturedOriginal = false;
};

class MoveClipCommand final : public ProjectCommand
{
public:
    MoveClipCommand(juce::String clipToMove,
                    double destinationSeconds,
                    juce::String destinationTrackId = {});

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String clipId;
    juce::String newTrackId;
    juce::String oldTrackId;
    double newStartSeconds = 0.0;
    double oldStartSeconds = 0.0;
    std::size_t oldClipIndex = 0;
    bool capturedOriginal = false;
};

class SetClipStateCommand final : public ProjectCommand
{
public:
    SetClipStateCommand(juce::String trackId,
                        AudioClip before,
                        AudioClip after,
                        juce::String commandName);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    AudioClip oldClip;
    AudioClip newClip;
    juce::String commandName;
};

class TrimClipCommand final : public ProjectCommand
{
public:
    TrimClipCommand(juce::String clipToTrim,
                    double destinationStartSeconds,
                    double destinationSourceOffsetSeconds,
                    double destinationDurationSeconds);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String clipId;
    double newStartSeconds = 0.0;
    double newSourceOffsetSeconds = 0.0;
    double newDurationSeconds = 0.0;
    AudioClip originalClip;
    bool capturedOriginal = false;
};

class SplitClipCommand final : public ProjectCommand
{
public:
    SplitClipCommand(juce::String clipToSplit, double splitPositionSeconds);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String clipId;
    double splitSeconds = 0.0;
    AudioClip original;
    AudioClip rightSide;
    juce::String trackId;
    std::size_t clipIndex = 0;
    bool capturedOriginal = false;
};

class DeleteClipCommand final : public ProjectCommand
{
public:
    explicit DeleteClipCommand(juce::String clipToDelete);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String clipId;
    juce::String trackId;
    AudioClip deletedClip;
    std::size_t clipIndex = 0;
    bool capturedOriginal = false;
};

class SetActiveTakeCommand final : public ProjectCommand
{
public:
    SetActiveTakeCommand(juce::String parentTrackId,
                         juce::String activeTakeTrackId);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String parentId;
    juce::String newActiveTakeId;
    juce::String oldActiveTakeId;
    bool capturedOriginal = false;
};

class SetCompRegionsCommand final : public ProjectCommand
{
public:
    SetCompRegionsCommand(juce::String parentTrackId,
                          std::vector<CompRegion> before,
                          std::vector<CompRegion> after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    static bool validate(const Project& project,
                         const juce::String& parentId,
                         const std::vector<CompRegion>& regions,
                         juce::String& error);

    juce::String parentId;
    std::vector<CompRegion> oldRegions;
    std::vector<CompRegion> newRegions;
};

class SetEditGroupsCommand final : public ProjectCommand
{
public:
    SetEditGroupsCommand(std::vector<EditGroup> before,
                         std::vector<EditGroup> after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    static bool validate(const Project& project,
                         const std::vector<EditGroup>& groups,
                         juce::String& error);

    std::vector<EditGroup> oldGroups;
    std::vector<EditGroup> newGroups;
};

class SetReampRoutesCommand final : public ProjectCommand
{
public:
    SetReampRoutesCommand(std::vector<ReampRoute> before,
                          std::vector<ReampRoute> after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    static bool validate(const Project& project,
                         const std::vector<ReampRoute>& routes,
                         juce::String& error);

    std::vector<ReampRoute> oldRoutes;
    std::vector<ReampRoute> newRoutes;
    std::vector<ToneSnapshot> oldToneSnapshots;
    bool capturedOldSnapshots = false;
};

struct ProjectTransportState
{
    double tempo = 120.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    std::vector<TempoChange> tempoChanges;
    std::vector<MeterChange> meterChanges;
    bool metronomeEnabled = true;
    int metronomeSubdivision = 1;
    int metronomeOutputChannel = 0;
    float metronomeLevel = 0.65f;
    float metronomeAccentLevel = 1.0f;
    bool punchEnabled = false;
    double punchInSeconds = 0.0;
    double punchOutSeconds = 8.0;
    int countInBars = 0;
    double preRollSeconds = 0.0;
    double postRollSeconds = 0.0;
    bool loopEnabled = false;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 8.0;

    static ProjectTransportState fromProject(const Project& project);
};

class SetProjectTransportCommand final : public ProjectCommand
{
public:
    SetProjectTransportCommand(ProjectTransportState before,
                               ProjectTransportState after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    static void apply(Project& project, const ProjectTransportState& state);

    ProjectTransportState oldState;
    ProjectTransportState newState;
};

struct TrackMixState
{
    float volumeDecibels = 0.0f;
    float pan = 0.0f;
    bool muted = false;
    bool solo = false;
    bool armed = false;
    int inputChannel = 0;
    bool stereoInput = false;
    bool inputMonitoring = false;
    bool versionsCollapsed = false;
    juce::Colour colour { 0xffdd5b3f };

    static TrackMixState fromTrack(const Track& track);
};

struct TrackRoutingState
{
    juce::String folderTrackId;
    std::vector<juce::String> controlledTrackIds;
    ChannelLayout channelLayout = ChannelLayout::stereo;
    bool polarityInverted = false;
    bool soloSafe = false;
    int hardwareOutputChannel = 0;
    float controlRoomDimDecibels = -20.0f;
    bool controlRoomDimmed = false;
    bool controlRoomMono = false;

    static TrackRoutingState fromTrack(const Track& track);
};

class SetTrackRoutingStateCommand final : public ProjectCommand
{
public:
    SetTrackRoutingStateCommand(juce::String trackToChange,
                                TrackRoutingState before,
                                TrackRoutingState after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    static void apply(Track& track, const TrackRoutingState& state);

    juce::String trackId;
    TrackRoutingState oldState;
    TrackRoutingState newState;
};

class SetAutomationLaneCommand final : public ProjectCommand
{
public:
    SetAutomationLaneCommand(AutomationLane before,
                             AutomationLane after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    AutomationLane oldLane;
    AutomationLane newLane;
};

class AddAutomationLaneCommand final : public ProjectCommand
{
public:
    explicit AddAutomationLaneCommand(AutomationLane laneToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    AutomationLane lane;
    std::size_t insertionIndex = 0;
};

class RemoveAutomationLaneCommand final : public ProjectCommand
{
public:
    explicit RemoveAutomationLaneCommand(juce::String laneToRemove);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String laneId;
    AutomationLane removedLane;
    std::size_t removalIndex = 0;
    bool capturedOriginal = false;
};

class SetTrackAutomationModeCommand final : public ProjectCommand
{
public:
    SetTrackAutomationModeCommand(juce::String trackToChange,
                                  AutomationMode mode,
                                  bool armed);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    AutomationMode newMode = AutomationMode::read;
    AutomationMode oldMode = AutomationMode::read;
    bool newArmed = false;
    bool oldArmed = false;
    bool capturedOriginal = false;
};

class SetTrackMixCommand final : public ProjectCommand
{
public:
    SetTrackMixCommand(juce::String trackToChange, TrackMixState before, TrackMixState after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    static void apply(Track& track, const TrackMixState& state);

    juce::String trackId;
    TrackMixState oldState;
    TrackMixState newState;
};

class SetTrackOutputCommand final : public ProjectCommand
{
public:
    SetTrackOutputCommand(juce::String sourceTrackId,
                          juce::String destinationTrackId);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    juce::String newOutputTrackId;
    juce::String oldOutputTrackId;
    std::optional<RoutingConnection> oldRoute;
    RoutingConnection newRoute;
    bool capturedOriginal = false;
};

class AddRoutingConnectionCommand final : public ProjectCommand
{
public:
    explicit AddRoutingConnectionCommand(RoutingConnection connectionToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    RoutingConnection connection;
    std::size_t insertionIndex = 0;
    bool capturedIndex = false;
};

class UpdateRoutingConnectionCommand final : public ProjectCommand
{
public:
    UpdateRoutingConnectionCommand(RoutingConnection before,
                                   RoutingConnection after);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    RoutingConnection oldConnection;
    RoutingConnection newConnection;
};

class RemoveRoutingConnectionCommand final : public ProjectCommand
{
public:
    explicit RemoveRoutingConnectionCommand(juce::String connectionToRemove);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String connectionId;
    RoutingConnection removedConnection;
    std::vector<std::pair<std::size_t, AutomationLane>>
        removedAutomationLanes;
    std::size_t removalIndex = 0;
    bool capturedOriginal = false;
};

class AddPluginInsertCommand final : public ProjectCommand
{
public:
    AddPluginInsertCommand(juce::String destinationTrackId, PluginInsert insertToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    PluginInsert insert;
    std::size_t insertionIndex = 0;
    bool capturedIndex = false;
};

class RemovePluginInsertCommand final : public ProjectCommand
{
public:
    RemovePluginInsertCommand(juce::String sourceTrackId, juce::String insertToRemove);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    juce::String insertId;
    PluginInsert removedInsert;
    std::vector<std::pair<std::size_t, AutomationLane>>
        removedAutomationLanes;
    std::size_t removalIndex = 0;
    bool capturedOriginal = false;
};

class ReplacePluginInsertCommand final : public ProjectCommand
{
public:
    ReplacePluginInsertCommand(juce::String sourceTrackId,
                               juce::String insertToReplace,
                               PluginInsert replacement);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    PluginInsert* find(Project& project) const;

    juce::String trackId;
    juce::String insertId;
    PluginInsert oldInsert;
    PluginInsert newInsert;
    bool capturedOriginal = false;
};

class SetPluginBypassCommand final : public ProjectCommand
{
public:
    SetPluginBypassCommand(juce::String sourceTrackId,
                           juce::String insertToChange,
                           bool shouldBeBypassed);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    PluginInsert* find(Project& project) const;

    juce::String trackId;
    juce::String insertId;
    bool newBypassed = false;
    bool oldBypassed = false;
    bool capturedOriginal = false;
};

class SetPluginBridgeModeCommand final : public ProjectCommand
{
public:
    SetPluginBridgeModeCommand(juce::String sourceTrackId,
                               juce::String insertToChange,
                               PluginBridgeMode mode);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    PluginInsert* find(Project& project) const;

    juce::String trackId;
    juce::String insertId;
    PluginBridgeMode newMode = PluginBridgeMode::sandboxed;
    PluginBridgeMode oldMode = PluginBridgeMode::sandboxed;
    bool oldRecoveryDisabled = false;
    bool capturedOriginal = false;
};

class RemoveTrackCommand final : public ProjectCommand
{
public:
    explicit RemoveTrackCommand(juce::String trackToRemove);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String trackId;
    std::vector<std::pair<std::size_t, Track>> removedTracks;
    juce::String affectedParentId;
    juce::String oldActiveTakeId;
    std::vector<CompRegion> oldCompRegions;
    std::vector<EditGroup> oldEditGroups;
    std::vector<ReampRoute> oldReampRoutes;
    std::vector<RoutingConnection> oldRoutingConnections;
    std::vector<AutomationLane> oldAutomationLanes;
    std::vector<ToneSnapshot> oldToneSnapshots;
    std::vector<MixerSnapshot> oldMixerSnapshots;
    std::vector<std::pair<juce::String, TrackRoutingState>> oldTrackRoutingStates;
    std::vector<std::pair<juce::String, juce::String>> oldTrackOutputs;
    bool capturedOriginal = false;
};

class DuplicateTrackCommand final : public ProjectCommand
{
public:
    explicit DuplicateTrackCommand(juce::String trackToDuplicate);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;
    [[nodiscard]] const juce::String& duplicatedTrackId() const noexcept;

private:
    juce::String sourceTrackId;
    juce::String duplicatedRootTrackId;
    std::vector<Track> duplicatedTracks;
    std::vector<ReampRoute> duplicatedRoutes;
    std::vector<RoutingConnection> duplicatedConnections;
    std::vector<AutomationLane> duplicatedAutomationLanes;
    std::vector<ToneSnapshot> duplicatedToneSnapshots;
    std::size_t insertionIndex = 0;
    bool createdDuplicate = false;
};

class AddToneSnapshotCommand final : public ProjectCommand
{
public:
    explicit AddToneSnapshotCommand(ToneSnapshot snapshotToAdd);
    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    ToneSnapshot snapshot;
};

class SetToneSnapshotsCommand final : public ProjectCommand
{
public:
    SetToneSnapshotsCommand(std::vector<ToneSnapshot> before,
                            std::vector<ToneSnapshot> after);
    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    std::vector<ToneSnapshot> oldSnapshots;
    std::vector<ToneSnapshot> newSnapshots;
};

class RecallToneSnapshotCommand final : public ProjectCommand
{
public:
    explicit RecallToneSnapshotCommand(ToneSnapshot snapshotToRecall);
    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    ToneSnapshot snapshot;
    Track oldReturnTrack;
    std::vector<RoutingConnection> oldRoutes;
    std::vector<AutomationLane> oldAutomation;
    juce::String oldActiveSnapshotId;
    bool capturedOriginal = false;
};

class AddMixerSnapshotCommand final : public ProjectCommand
{
public:
    explicit AddMixerSnapshotCommand(MixerSnapshot snapshotToAdd);
    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    MixerSnapshot snapshot;
};

class AddRenderReportsCommand final : public ProjectCommand
{
public:
    explicit AddRenderReportsCommand(std::vector<RenderReport> reportsToAdd);
    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    std::vector<RenderReport> reports;
};

class RecallMixerSnapshotCommand final : public ProjectCommand
{
public:
    explicit RecallMixerSnapshotCommand(MixerSnapshot snapshotToRecall);
    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    MixerSnapshot snapshot;
    std::vector<Track> oldTracks;
    std::vector<RoutingConnection> oldRoutes;
    std::vector<AutomationLane> oldAutomation;
    bool capturedOriginal = false;
};
}
