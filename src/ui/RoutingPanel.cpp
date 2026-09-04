#include "RoutingPanel.h"

#include "StudioTheme.h"

#include <algorithm>
#include <cmath>

namespace studio
{
RoutingPanel::RoutingPanel()
{
    addAndMakeVisible(addButton);
    addAndMakeVisible(trackButton);
    addButton.onClick = [this] { showAddMenu(); };
    trackButton.onClick = [this] { showTrackMenu(); };
}

void RoutingPanel::setProject(const Project* value)
{
    project = value;
    repaint();
}

void RoutingPanel::setTrack(const juce::String& value)
{
    trackId = value;
    repaint();
}

void RoutingPanel::setHardwareOutputs(juce::StringArray names)
{
    hardwareOutputs = std::move(names);
}

void RoutingPanel::editConnection(const juce::String& connectionId)
{
    if (project == nullptr)
        return;
    const auto route = std::find_if(
        project->routingConnections.cbegin(),
        project->routingConnections.cend(),
        [&connectionId](const auto& candidate)
        {
            return candidate.id == connectionId;
        });
    if (route != project->routingConnections.cend())
        showRouteMenu(*route);
}

std::vector<const RoutingConnection*> RoutingPanel::displayedRoutes() const
{
    std::vector<const RoutingConnection*> result;
    if (project == nullptr)
        return result;
    for (const auto& route : project->routingConnections)
    {
        if (route.sourceTrackId == trackId
            && route.kind != RouteKind::mainOutput)
            result.push_back(&route);
    }
    return result;
}

void RoutingPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
    graphics.drawText("ROUTING", 0, 0, getWidth() - 112, 24,
                      juce::Justification::centredLeft);

    const auto routes = displayedRoutes();
    if (routes.empty())
    {
        graphics.setFont(juce::Font(juce::FontOptions(10.0f)));
        graphics.drawFittedText("No sends, sidechains, or direct hardware routes.",
                                0,
                                28,
                                getWidth(),
                                getHeight() - 32,
                                juce::Justification::topLeft,
                                2);
        return;
    }

    auto y = 28;
    for (const auto* route : routes)
    {
        if (y + 28 > getHeight())
            break;
        const juce::Rectangle<int> row(0, y, getWidth(), 26);
        graphics.setColour(juce::Colour(route->enabled
                                            ? StudioColours::raised
                                            : 0xff191c1f));
        graphics.fillRoundedRectangle(row.toFloat(), 3.0f);
        graphics.setColour(juce::Colour(route->muted
                                            ? StudioColours::amber
                                            : StudioColours::text));
        graphics.setFont(juce::Font(juce::FontOptions(9.5f)));
        graphics.drawFittedText(
            RoutingUiModel::summary(*project, *route),
            row.reduced(6, 2),
            juce::Justification::centredLeft,
            1);
        y += 30;
    }
}

void RoutingPanel::resized()
{
    auto header = getLocalBounds().removeFromTop(24);
    trackButton.setBounds(header.removeFromRight(56).reduced(2));
    addButton.setBounds(header.removeFromRight(50).reduced(2));
}

void RoutingPanel::mouseDown(const juce::MouseEvent& event)
{
    if (event.position.y < 28.0f)
        return;
    const auto routes = displayedRoutes();
    const auto index = static_cast<int>((event.position.y - 28.0f) / 30.0f);
    if (index >= 0 && index < static_cast<int>(routes.size()))
        showRouteMenu(*routes[static_cast<std::size_t>(index)]);
}

void RoutingPanel::showAddMenu()
{
    const auto* source = project != nullptr ? project->findTrack(trackId) : nullptr;
    if (source == nullptr
        || source->type == TrackType::folder
        || source->type == TrackType::vca
        || source->type == TrackType::midi
        || source->type == TrackType::controlRoom)
        return;

    juce::PopupMenu menu;
    const auto addSends = [this](juce::PopupMenu& target, RouteTap tap)
    {
        for (const auto& destination :
             RoutingUiModel::sendDestinations(*project, trackId))
        {
            target.addItem(destination.label,
                           [this, destination, tap]
                           {
                               RoutingConnection route;
                               route.name = destination.label;
                               route.kind = RouteKind::send;
                               route.tap = tap;
                               route.sourceTrackId = trackId;
                               route.destination.type =
                                   RouteEndpointType::track;
                               route.destination.trackId =
                                   destination.trackId;
                               if (onAddConnection)
                                   onAddConnection(std::move(route));
                           });
        }
        if (target.getNumItems() == 0)
            target.addItem("No valid destinations", false, false, [] {});
    };

    juce::PopupMenu preSends;
    addSends(preSends, RouteTap::preFader);
    menu.addSubMenu("Pre-fader send", preSends);
    juce::PopupMenu postSends;
    addSends(postSends, RouteTap::postFader);
    menu.addSubMenu("Post-fader send", postSends);

    juce::PopupMenu sidechains;
    for (const auto& destination :
         RoutingUiModel::sidechainDestinations(*project, trackId))
    {
        sidechains.addItem(destination.label,
                           [this, destination]
                           {
                               RoutingConnection route;
                               route.name = "Sidechain to "
                                   + destination.label;
                               route.kind = RouteKind::sidechain;
                               route.tap = RouteTap::preFader;
                               route.sourceTrackId = trackId;
                               route.destination.type =
                                   RouteEndpointType::pluginSidechain;
                               route.destination.trackId =
                                   destination.trackId;
                               route.destination.insertId =
                                   destination.insertId;
                               if (onAddConnection)
                                   onAddConnection(std::move(route));
                           });
    }
    if (sidechains.getNumItems() == 0)
        sidechains.addItem("No valid insert sidechains", false, false, [] {});
    menu.addSubMenu("Plugin sidechain", sidechains);

    juce::PopupMenu hardware;
    for (int channel = 0; channel < hardwareOutputs.size(); channel += 2)
    {
        auto label = hardwareOutputs[channel];
        if (channel + 1 < hardwareOutputs.size())
            label << " + " << hardwareOutputs[channel + 1];
        hardware.addItem(label,
                         [this, channel, label]
                         {
                             RoutingConnection route;
                             route.name = label;
                             route.kind = RouteKind::hardwareOutput;
                             route.sourceTrackId = trackId;
                             route.destination.type =
                                 RouteEndpointType::hardwareOutput;
                             route.destination.firstChannel = channel;
                             route.destination.channels =
                                 channel + 1 < hardwareOutputs.size() ? 2 : 1;
                             if (onAddConnection)
                                 onAddConnection(std::move(route));
                         });
    }
    if (hardware.getNumItems() == 0)
        hardware.addItem("No active outputs", false, false, [] {});
    menu.addSubMenu("Hardware output", hardware);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(addButton));
}

void RoutingPanel::showTrackMenu()
{
    const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
    if (track == nullptr)
        return;

    const auto selectedTrackId = track->id;
    const auto apply = [this, selectedTrackId](
                           const std::function<void(TrackRoutingState&)>& change)
    {
        const auto* selected = project != nullptr
            ? project->findTrack(selectedTrackId)
            : nullptr;
        if (selected == nullptr)
            return;
        auto before = TrackRoutingState::fromTrack(*selected);
        auto after = before;
        change(after);
        if (onTrackRoutingChanged)
            onTrackRoutingChanged(selectedTrackId, before, after);
    };

    juce::PopupMenu menu;
    menu.addItem("Solo safe",
                 true,
                 track->soloSafe,
                 [apply]
                 {
                     apply([](auto& state) { state.soloSafe = !state.soloSafe; });
                 });
    menu.addItem("Invert polarity",
                 true,
                 track->polarityInverted,
                 [apply]
                 {
                     apply([](auto& state)
                     {
                         state.polarityInverted = !state.polarityInverted;
                     });
                 });
    menu.addItem("Mono channel",
                 true,
                 track->channelLayout == ChannelLayout::mono,
                 [apply]
                 {
                     apply([](auto& state)
                     {
                         state.channelLayout =
                             state.channelLayout == ChannelLayout::mono
                             ? ChannelLayout::stereo
                             : ChannelLayout::mono;
                     });
                 });

    juce::PopupMenu folders;
    folders.addItem("No folder",
                    true,
                    track->folderTrackId.isEmpty(),
                    [apply]
                    {
                        apply([](auto& state) { state.folderTrackId.clear(); });
                    });
    for (const auto& candidate : project->tracks)
    {
        if (candidate.type != TrackType::folder || candidate.id == track->id)
            continue;
        folders.addItem(candidate.name,
                        true,
                        candidate.id == track->folderTrackId,
                        [apply, folderId = candidate.id]
                        {
                            apply([folderId](auto& state)
                            {
                                state.folderTrackId = folderId;
                            });
                        });
    }
    menu.addSubMenu("Folder", folders);

    juce::PopupMenu vcas;
    for (const auto& candidate : project->tracks)
    {
        if (candidate.type != TrackType::vca)
            continue;
        const auto assigned = std::find(
            candidate.controlledTrackIds.cbegin(),
            candidate.controlledTrackIds.cend(),
            track->id) != candidate.controlledTrackIds.cend();
        vcas.addItem(candidate.name,
                     true,
                     assigned,
                     [this,
                      candidate,
                      controlledTrackId = selectedTrackId,
                      assigned]
                     {
                         auto before = TrackRoutingState::fromTrack(candidate);
                         auto after = before;
                         if (assigned)
                         {
                             after.controlledTrackIds.erase(
                                 std::remove(after.controlledTrackIds.begin(),
                                             after.controlledTrackIds.end(),
                                             controlledTrackId),
                                 after.controlledTrackIds.end());
                         }
                         else
                         {
                             after.controlledTrackIds.push_back(controlledTrackId);
                         }
                         if (onTrackRoutingChanged)
                             onTrackRoutingChanged(candidate.id, before, after);
                     });
    }
    if (vcas.getNumItems() == 0)
        vcas.addItem("No VCA tracks", false, false, [] {});
    menu.addSubMenu("VCA assignment", vcas);

    if (track->type == TrackType::controlRoom)
    {
        menu.addSeparator();
        juce::PopupMenu outputs;
        for (int channel = 0; channel < hardwareOutputs.size(); channel += 2)
        {
            auto label = hardwareOutputs[channel];
            if (channel + 1 < hardwareOutputs.size())
                label << " + " << hardwareOutputs[channel + 1];
            outputs.addItem(label,
                            true,
                            track->hardwareOutputChannel == channel,
                            [apply, channel]
                            {
                                apply([channel](auto& state)
                                {
                                    state.hardwareOutputChannel = channel;
                                });
                            });
        }
        if (outputs.getNumItems() == 0)
            outputs.addItem("No active outputs", false, false, [] {});
        menu.addSubMenu("Monitor output", outputs);
        menu.addItem("Dim",
                     true,
                     track->controlRoomDimmed,
                     [apply]
                     {
                         apply([](auto& state)
                         {
                             state.controlRoomDimmed =
                                 !state.controlRoomDimmed;
                         });
                     });
        menu.addItem("Mono monitor",
                     true,
                     track->controlRoomMono,
                     [apply]
                     {
                         apply([](auto& state)
                         {
                             state.controlRoomMono = !state.controlRoomMono;
                         });
                     });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(trackButton));
}

void RoutingPanel::showRouteMenu(const RoutingConnection& route)
{
    const auto update = [this, route](
                            const std::function<void(RoutingConnection&)>& change)
    {
        auto after = route;
        change(after);
        if (onUpdateConnection)
            onUpdateConnection(route, std::move(after));
    };

    juce::PopupMenu menu;
    menu.addItem("Enabled",
                 true,
                 route.enabled,
                 [update]
                 {
                     update([](auto& value) { value.enabled = !value.enabled; });
                 });
    menu.addItem("Muted",
                 true,
                 route.muted,
                 [update]
                 {
                     update([](auto& value) { value.muted = !value.muted; });
                 });
    menu.addItem("Pre-fader",
                 true,
                 route.tap == RouteTap::preFader,
                 [update]
                 {
                     update([](auto& value)
                     {
                         value.tap = RouteTap::preFader;
                     });
                 });
    menu.addItem("Post-fader",
                 true,
                 route.tap == RouteTap::postFader,
                 [update]
                 {
                     update([](auto& value)
                     {
                         value.tap = RouteTap::postFader;
                     });
                 });

    juce::PopupMenu levels;
    for (const auto level : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f })
    {
        levels.addItem(juce::String(level, 1) + " dB",
                       true,
                       std::abs(route.gainDecibels - level) < 0.001f,
                       [update, level]
                       {
                           update([level](auto& value)
                           {
                               value.gainDecibels = level;
                           });
                       });
    }
    menu.addSubMenu("Level", levels);
    menu.addSeparator();
    menu.addItem("Remove route",
                 [this, routeId = route.id]
                 {
                     if (onRemoveConnection)
                         onRemoveConnection(routeId);
                 });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}
}
