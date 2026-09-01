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

class AddTrackCommand final : public ProjectCommand
{
public:
    explicit AddTrackCommand(Track trackToAdd);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    Track track;
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
    double oldStartSeconds = 0.0;
    double oldSourceOffsetSeconds = 0.0;
    double oldDurationSeconds = 0.0;
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
    std::size_t removalIndex = 0;
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
    Track duplicatedTrack;
    std::size_t insertionIndex = 0;
    bool createdDuplicate = false;
};
}
