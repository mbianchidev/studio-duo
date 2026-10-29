#pragma once

#include "ProjectCommands.h"

namespace studio
{
struct TrackLifecycleState
{
    bool active = true;
    bool hidden = false;
    bool folderCollapsed = false;
    juce::String frozenTrackId;
    juce::String freezeSourceTrackId;

    static TrackLifecycleState fromTrack(const Track&);
};

class SetTrackLifecycleCommand final : public ProjectCommand
{
public:
    SetTrackLifecycleCommand(juce::String trackId, TrackLifecycleState before,
                             TrackLifecycleState after, juce::String name);
    juce::String name() const override;
    bool perform(Project&, juce::String& error) override;
    void undo(Project&) override;

private:
    static void apply(Track&, const TrackLifecycleState&);
    juce::String trackId, commandName;
    TrackLifecycleState before, after;
};

class ArrangementEditing
{
public:
    static std::unique_ptr<ProjectCommand> groupTracks(
        const Project&, const std::vector<juce::String>& trackIds,
        bool busFolder, juce::String& error);
    static std::optional<AudioClip> slipClip(
        const AudioClip&, double sourceDeltaSeconds, juce::String& error);
    static std::optional<AudioClip> stretchClip(
        const AudioClip&, double durationSeconds, juce::String& error);
    static std::vector<const Track*> visibleTracks(const Project&);
    static std::vector<juce::String> trackSubtreeIds(const Project&, const std::vector<juce::String>& roots);
};
}
