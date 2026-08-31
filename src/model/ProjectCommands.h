#pragma once

#include "ProjectModel.h"

#include <memory>
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

class MoveClipCommand final : public ProjectCommand
{
public:
    MoveClipCommand(juce::String clipToMove, double destinationSeconds);

    [[nodiscard]] juce::String name() const override;
    bool perform(Project& project, juce::String& error) override;
    void undo(Project& project) override;

private:
    juce::String clipId;
    double newStartSeconds = 0.0;
    double oldStartSeconds = 0.0;
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
    Track removedTrack;
    std::size_t removalIndex = 0;
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
