#pragma once

#include "RoutingUiModel.h"
#include "StudioIconButton.h"
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
    [[nodiscard]] int preferredHeight() const;
    void setHardwareOutputs(juce::StringArray names);
    void editConnection(const juce::String& connectionId);
    void showAddRouteMenu(
        juce::Rectangle<int> targetScreenArea = {});

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
    void showAddMenu(
        juce::Rectangle<int> targetScreenArea = {});
    void showTrackMenu();
    void showRouteMenu(const RoutingConnection& route);

    const Project* project = nullptr;
    juce::String trackId;
    juce::StringArray hardwareOutputs;
    StudioIconButton addButton {
        StudioIcon::route,
        "Add route",
        "Add a MIDI, send, sidechain, or direct hardware route"
    };
    juce::TextButton trackButton { "TRACK" };
};
}
