#pragma once

#include "ProjectModel.h"

#include <optional>
#include <vector>

namespace studio
{
struct ProjectTemplateDescriptor
{
    juce::String id;
    juce::String name;
    juce::String description;
};

class ProjectTemplates
{
public:
    [[nodiscard]] static Project createBlankSong();
    [[nodiscard]] static const std::vector<
        ProjectTemplateDescriptor>& descriptors();
    [[nodiscard]] static std::optional<Project> create(
        const juce::String& templateId);
};
}
