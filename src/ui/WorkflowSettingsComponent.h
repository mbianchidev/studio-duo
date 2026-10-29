#pragma once

#include "update/StudioPreferences.h"

namespace studio
{
class WorkflowSettingsComponent final : public juce::Component
{
public:
    WorkflowSettingsComponent(juce::ApplicationCommandManager&, StudioPreferences&, std::function<void()> macrosChanged);
    ~WorkflowSettingsComponent() override;
    void resized() override;

private:
    juce::KeyMappingEditorComponent shortcuts;
    std::unique_ptr<juce::Component> macros;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
};
}
