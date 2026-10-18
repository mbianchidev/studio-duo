#pragma once

#include "ControlSurface.h"
#include "automation/AutomationRecorder.h"
#include "model/ProjectCommands.h"

namespace studio
{
struct ControlSurfaceMixEdit
{
    std::unique_ptr<ProjectCommand> command;
    std::vector<AutomationPreviewTarget> previews;
};

[[nodiscard]] ControlSurfaceState controlSurfaceStateForProject(
    const Project&, const juce::String& selectedTrackId, bool playing, bool recording);
[[nodiscard]] std::optional<ControlSurfaceMixEdit> prepareControlSurfaceMixEdit(
    const Project&, std::span<const ControlSurfaceEvent>, double positionSeconds, juce::String& error);
}
