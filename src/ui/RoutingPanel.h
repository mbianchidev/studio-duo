#pragma once

#include "RoutingUiModel.h"
#include "model/ProjectCommands.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class RoutingPanel final : public juce::Component
{
public:
    RoutingPanel();

    void setProject(const Project* value);
    void setTrack(const juce::String& value);
    void setHardwareOutputs(juce::StringArray names);

    std::function<void(RoutingConnection)> onAddConnection;
    std::function<void(RoutingConnection, RoutingConnection)> onUpdateConnection;
    std::function<void(const juce::String&)> onRemoveConnection;
    std::function<void(const juce::String&,
                       TrackRoutingState,
                       TrackRoutingState)> onTrackRoutingChanged;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    [[nodiscard]] std::vector<const RoutingConnection*> displayedRoutes() const;
    void showAddMenu();
    void showTrackMenu();
    void showRouteMenu(const RoutingConnection& route);

    const Project* project = nullptr;
    juce::String trackId;
    juce::StringArray hardwareOutputs;
    juce::TextButton addButton { "ADD" };
    juce::TextButton trackButton { "TRACK" };
};
}
