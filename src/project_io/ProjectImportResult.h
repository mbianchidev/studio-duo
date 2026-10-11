#pragma once

#include "model/ProjectModel.h"

#include <optional>

namespace studio
{
struct ProjectImportResult
{
    juce::Result result = juce::Result::fail(
        "Project import did not run.");
    CompatibilityReport report;
    std::optional<Project> project;
    juce::File package;
    bool requiresCompatibilityConfirmation = false;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return result.wasOk()
            && project.has_value()
            && package.isDirectory();
    }
};
}
