#include "TestHarness.h"
#include "TestSuites.h"
#include "midi/ControlSurface.h"
#include "midi/ControlSurfaceProject.h"
#include "ui/ControlSurfaceSettingsComponent.h"
#include "ui/StudioTheme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
studio::ControlSurfaceState mockState(int tracks = 17)
{
    studio::ControlSurfaceState state;
    state.projectId = "synthetic-control-project";
    for (int index = 0; index < tracks; ++index)
        state.tracks.push_back({ "mock-track-" + juce::String(index), "Mock " + juce::String(index + 1) });
    state.master = studio::ControlSurfaceTrack { "mock-master", "Master" };
    state.selectedTrackId = "mock-track-0";
    return state;
}

juce::MidiMessage button(int note, bool pressed = true)
{
    return juce::MidiMessage::noteOn(1, note, juce::uint8(pressed ? 127 : 0));
}

class MockMidiIo final : public studio::ControlSurfaceMidiIo
{
public:
    juce::Array<juce::MidiDeviceInfo> inputs() override { ++enumerations; return inputList; }
    juce::Array<juce::MidiDeviceInfo> outputs() override { ++enumerations; return outputList; }
    juce::Result open(const studio::ControlSurfaceSettings&, juce::MidiInputCallback& receiver) override
    {
        ++opens;
        if (failOpen)
            return juce::Result::fail("Synthetic control-port open failure.");
        callback = &receiver;
        return juce::Result::ok();
    }
    void close() override { ++closes; callback = nullptr; }
    void send(const juce::MidiMessage& message) override { sent.push_back(message); }
    void receive(const juce::MidiMessage& message)
    {
        if (callback != nullptr)
        {
            auto stamped = message;
            stamped.setTimeStamp(nowMilliseconds / 1000.0);
            callback->handleIncomingMidiMessage(nullptr, stamped);
        }
    }
    juce::Array<juce::MidiDeviceInfo> inputList { juce::MidiDeviceInfo { "Mock surface", "mock-control-in" } };
    juce::Array<juce::MidiDeviceInfo> outputList { juce::MidiDeviceInfo { "Mock surface", "mock-control-out" } };
    std::vector<juce::MidiMessage> sent;
    juce::MidiInputCallback* callback = nullptr;
    int opens = 0, closes = 0, enumerations = 0;
    bool failOpen = false;
    double nowMilliseconds = 6000.0;
};
}

void controlSurfaceTests()
{
    using Type = studio::ControlSurfaceEvent::Type;
    const studio::ControlSurfaceSettings enabled {
        studio::ControlSurfaceProtocol::mackieControl, "mock-control-in", "mock-control-out"
    };
    expect(studio::ControlSurfaceSettings().validate().wasOk()
               && !studio::ControlSurfaceSettings().enabled(),
           "Control surfaces default to disabled without discovering or opening MIDI.");
    auto invalid = enabled;
    invalid.inputDeviceId.clear();
    expect(invalid.validate().failed(), "Enabled control surfaces require explicit dedicated ports.");
    expect(studio::validateControlSurfacePortUse(enabled, true, {}).failed()
               && studio::validateControlSurfacePortUse(enabled, false, "mock-control-out").failed()
               && studio::validateControlSurfacePortUse({}, true, "mock-control-out").wasOk(),
           "Port conflicts are rejectable before changing preferences or stealing an existing musical assignment.");
    studio::MackieControlProtocol protocol;
    auto state = mockState();
    expect(protocol.setState(state).wasOk(), "A synthetic track bank configures the protocol.");
    const auto play = protocol.process(button(94));
    expect(play && play->type == Type::play, "MCU play uses note 94 on MIDI channel one.");
    expect(!protocol.process(button(94)), "Repeated held-button messages do not toggle transport twice.");
    juce::ignoreUnused(protocol.process(button(94, false)));
    expect(protocol.process(button(94)).has_value(), "A released transport button can be pressed again.");
    expect(!protocol.process(juce::MidiMessage::noteOn(2, 95, juce::uint8(127))),
           "A musical note on another channel is not an MCU transport command.");
    juce::ignoreUnused(protocol.process(button(47)));
    const auto select = protocol.process(button(24));
    expect(protocol.bankOffset() == 8 && select && select->trackId == "mock-track-8",
           "Bank right changes eight stable track targets, not the selected musical MIDI channel.");
    const auto fader = protocol.process(juce::MidiMessage::pitchWheel(1, 8192));
    const auto master = protocol.process(juce::MidiMessage::pitchWheel(9, 16383));
    expect(fader && fader->type == Type::volume && fader->trackId == "mock-track-8"
               && std::abs(fader->value - 8192.0f / 16383.0f) < 1e-6f
               && master && master->trackId == "mock-master" && master->value > 0.999f,
           "All fourteen fader bits and the independent master channel survive decoding.");
    const auto pan = protocol.process(juce::MidiMessage::controllerEvent(1, 16, 3));
    const auto panBack = protocol.process(juce::MidiMessage::controllerEvent(1, 16, 0x42));
    expect(pan && panBack && pan->type == Type::pan
               && std::abs(pan->value - 0.53f) < 1e-5f && std::abs(panBack->value - 0.51f) < 1e-5f,
           "Signed-magnitude V-Pot deltas accumulate instead of being interpreted as absolute CC values.");
    juce::ignoreUnused(protocol.process(button(104)));
    juce::ignoreUnused(protocol.process(button(47, false)));
    const auto blocked = protocol.process(button(47));
    expect(blocked && blocked->type == Type::bankWhileTouched && protocol.bankOffset() == 8,
           "Banking while a channel fader is touched cannot retarget an active move.");
    juce::ignoreUnused(protocol.process(button(104, false)));
    juce::ignoreUnused(protocol.process(button(47, false)));
    juce::ignoreUnused(protocol.process(button(47)));
    expect(protocol.bankOffset() == 16, "The final partial bank remains reachable after touch release.");
    const auto missing = protocol.process(button(25));
    expect(!missing, "Unused strips never map onto another track.");
    state.tracks.front().volume = std::numeric_limits<float>::quiet_NaN();
    expect(protocol.setState(state).failed(), "Nonfinite state cannot enter outgoing motor feedback.");
    state = mockState(2);
    expect(protocol.setState(state).wasOk(), "The feedback fixture restores valid host state.");
    protocol.reset();
    const auto initialFeedback = protocol.feedback();
    expect(!initialFeedback.empty() && protocol.feedback().empty(),
           "Unchanged host state does not flood a control surface with repeated feedback.");
    const auto ring = std::find_if(initialFeedback.begin(), initialFeedback.end(), [](const auto& message)
    { return message.isController() && message.getControllerNumber() == 48; });
    const auto names = std::find_if(initialFeedback.begin(), initialFeedback.end(), [](const auto& message)
    { return message.isSysEx(); });
    expect(ring != initialFeedback.end() && ring->getControllerValue() == 0x56
               && names != initialFeedback.end() && names->getSysExDataSize() == 62,
           "Pan rings use CC 48 rather than input CC 16; names use bounded seven-bit LCD feedback.");
    juce::ignoreUnused(protocol.process(button(104)));
    state.tracks.front().volume = 0.1f;
    expect(protocol.setState(state).wasOk(), "Host automation can update feedback while a fader is touched.");
    const auto touchedFeedback = protocol.feedback();
    expect(std::none_of(touchedFeedback.begin(), touchedFeedback.end(), [](const auto& message)
               { return message.isPitchWheel() && message.getChannel() == 1; }),
           "Motor feedback cannot fight a physically touched fader.");
    juce::ignoreUnused(protocol.process(button(104, false)));
    const auto releasedFeedback = protocol.feedback();
    expect(std::any_of(releasedFeedback.begin(), releasedFeedback.end(), [](const auto& message)
               { return message.isPitchWheel() && message.getChannel() == 1 && message.getPitchWheelValue() == 1638; }),
           "Touch release resynchronizes the motor to current host state.");
    const juce::uint8 query[] { 0, 0, 0x66, 0x14, 1, 'M', 'O', 'C', 'K', '0', '0', '1', 1, 2, 3, 4 };
    const auto reply = studio::MackieControlProtocol::handshakeReply(
        juce::MidiMessage::createSysExMessage(query, static_cast<int>(sizeof(query))));
    expect(reply && reply->getSysExDataSize() == 16 && reply->getSysExData()[4] == 2
               && reply->getSysExData()[12] == 5 && reply->getSysExData()[13] == 5
               && reply->getSysExData()[14] == 123 && reply->getSysExData()[15] == 47,
           "The MCU host handshake uses synthetic challenge/identity data only and returns seven-bit response bytes.");

    studio::MusicalMidiInputCollector collector;
    collector.reset(48000.0);
    collector.setReservedInput("mock-control-in");
    auto note = juce::MidiMessage::noteOn(1, 60, juce::uint8(100));
    note.setTimeStamp(juce::Time::getMillisecondCounterHiRes() / 1000.0);
    collector.collectFromDevice("mock-control-in", note);
    collector.collectFromDevice("mock-keyboard-in", note);
    juce::MidiBuffer collected;
    collector.removeNextBlockOfMessages(collected, 256);
    expect(!collector.acceptsDevice("mock-control-in") && collector.acceptsDevice("mock-keyboard-in")
               && collected.getNumEvents() == 1,
           "Reserved control input never enters musical recording or retrospective capture.");
    collector.setReservedInput({});
    expect(collector.acceptsDevice("mock-control-in"), "Disabling the reservation restores ordinary MIDI routing.");

    auto backend = std::make_unique<MockMidiIo>();
    auto* io = backend.get();
    auto session = std::make_unique<studio::ControlSurfaceSession>(std::move(backend));
    state = mockState();
    session->poll(state, 0.0, true, false, {});
    expect(io->enumerations == 0 && io->opens == 0, "A disabled session is driver-free.");
    expect(session->configure(enabled).wasOk(), "Explicit mock surface settings are accepted.");
    session->poll(state, 1000.0, true, false, {});
    expect(io->enumerations == 0 && io->opens == 0, "Saved settings cannot bypass the MIDI startup probe.");
    session->setHardwareAvailable(true);
    session->poll(state, 2000.0, true, true, {});
    expect(io->opens == 0 && session->hasError(), "A musical-input conflict is visible without changing existing device settings.");
    session->poll(state, 4000.0, true, false, "mock-control-out");
    expect(io->opens == 0 && session->hasError(), "Control feedback cannot share the selected musical output.");
    session->poll(state, 6000.0, true, false, {});
    expect(session->connected() && io->opens == 1 && !io->sent.empty(), "Dedicated mock ports open and receive initial feedback.");
    std::vector<studio::ControlSurfaceEvent> received;
    session->onEvents = [&](std::span<const studio::ControlSurfaceEvent> events)
    { received.insert(received.end(), events.begin(), events.end()); };
    io->receive(button(95));
    session->poll(state, 6033.0, false, false, {});
    expect(received.empty(), "Modal/export input blocking discards queued record commands.");
    session->poll(state, 6050.0, true, false, {});
    io->receive(button(95, false));
    io->receive(button(94));
    session->poll(state, 6066.0, true, false, {});
    expect(received.size() == 1 && received.front().type == Type::play,
           "Only message-thread polling dispatches physical control commands.");
    io->inputList.clear();
    session->poll(state, 8000.0, true, false, {});
    expect(!session->connected() && session->hasError(), "Hot unplug closes only the owned control ports and reports disconnection.");
    io->inputList.add(juce::MidiDeviceInfo { "Mock surface", "different-mock-input" });
    session->poll(state, 9000.0, true, false, {});
    expect(!session->connected() && io->opens == 1,
           "A similarly named replacement cannot silently receive the saved control mapping.");
    io->inputList.clear();
    io->inputList.add(juce::MidiDeviceInfo { "Mock surface", "mock-control-in" });
    session->poll(state, 10000.0, true, false, {});
    expect(session->connected() && io->opens == 2, "The same explicit device identity can reconnect without choosing another port.");
    received.clear();
    io->nowMilliseconds = 10000.0;
    io->receive(juce::MidiMessage::pitchWheel(1, 1024));
    io->receive(juce::MidiMessage::pitchWheel(1, 4096));
    io->receive(juce::MidiMessage::pitchWheel(1, 8192));
    io->receive(juce::MidiMessage::controllerEvent(1, 16, 1));
    io->receive(juce::MidiMessage::controllerEvent(1, 16, 1));
    session->poll(state, 10016.0, true, false, {});
    expect(received.size() == 2 && received[0].type == Type::volume && received[1].type == Type::pan
               && std::abs(received[0].value - 8192.0f / 16383.0f) < 1e-6f
               && std::abs(received[1].value - 0.52f) < 1e-5f,
           "A frame coalesces continuous input while preserving relative encoder travel.");
    received.clear();
    io->receive(button(95));
    state.projectId = "replacement-synthetic-project";
    session->poll(state, 10025.0, true, false, {});
    expect(received.empty(), "Queued transport commands cannot cross a project-replacement generation.");
    received.clear();
    for (int index = 0; index < 600; ++index)
        io->receive(button(95, index % 2 == 0));
    session->poll(state, 10033.0, true, false, {});
    expect(!session->connected() && session->hasError() && received.empty(),
           "Input overflow fails closed instead of replaying an incomplete sequence of transport commands.");
    session->poll(state, 13000.0, true, false, {});
    expect(!session->connected(), "An overflow fault requires an explicit reconnect.");
    session->reconnect();
    session->poll(state, 15000.0, true, false, {});
    expect(session->connected(), "Explicit reconnect recovers the owned control session after overflow.");
    received.clear();
    io->nowMilliseconds = 15000.0;
    io->receive(button(95));
    session->poll(state, 15400.0, true, false, {});
    expect(received.empty() && session->hasError() && session->connected(),
           "A blocked message thread cannot replay an old record press after a dialog closes.");
    io->nowMilliseconds = 15401.0;
    io->receive(button(95));
    session->poll(state, 15433.0, true, false, {});
    expect(received.empty(), "Discarded old button presses still require a physical release before rearming.");
    io->nowMilliseconds = 15434.0;
    io->receive(button(95, false));
    io->receive(button(95));
    session->poll(state, 15466.0, true, false, {});
    expect(received.size() == 1 && received.front().type == Type::record && !session->hasError(),
           "A fresh released-and-pressed control recovers from a visible stale-input warning.");
    received.clear();
    io->nowMilliseconds = 15467.0;
    io->receive(button(94));
    session->discardPendingCommands();
    session->poll(state, 15499.0, true, false, {});
    expect(received.empty() && session->connected(),
           "Reopening or repairing the same project identity invalidates queued input without reopening MIDI ports.");
    const auto sentBeforeShutdown = io->sent.size();
    session->onEvents = [&](auto) { session->shutdown(); };
    io->nowMilliseconds = 15500.0;
    io->receive(button(94));
    session->poll(state, 15532.0, true, false, {});
    expect(!session->connected() && io->sent.size() == sentBeforeShutdown,
           "A command callback can close its session without stale feedback or further dispatch afterward.");
    session->shutdown();
    expect(!session->connected(), "Shutdown releases the owned control input and output.");
    session->onEvents = {};
    session->setHardwareAvailable(true);
    session->reconnect();
    io->failOpen = true;
    session->poll(state, 17000.0, true, false, {});
    expect(!session->connected() && session->hasError() && session->status().contains("Synthetic"),
           "A driver-open failure is explicit and leaves no successful-looking connection.");
    io->failOpen = false;
    session->poll(state, 19000.0, true, false, {});
    expect(session->connected() && session->settings() == enabled,
           "A later retry keeps the same saved identities after a recoverable open failure.");
    session->shutdown();
    auto settingsIo = std::make_unique<MockMidiIo>();
    auto* settingsBackend = settingsIo.get();
    auto settingsSession = std::make_unique<studio::ControlSurfaceSession>(std::move(settingsIo));
    auto applied = 0;
    studio::StudioTheme theme;
    studio::ControlSurfaceSettingsComponent settingsPage(*settingsSession, {}, [&](const auto& value)
    {
        ++applied;
        return settingsSession->configure(value);
    });
    settingsPage.setLookAndFeel(&theme);
    expect(settingsBackend->enumerations == 0,
           "Opening control settings before MIDI readiness cannot discover devices.");
    settingsSession->setHardwareAvailable(true);
    juce::ComboBox* protocolSelector = nullptr;
    juce::ComboBox* inputSelector = nullptr;
    juce::ComboBox* outputSelector = nullptr;
    juce::Button* refreshPorts = nullptr;
    juce::Button* applySettings = nullptr;
    juce::Label* connectionStatus = nullptr;
    auto namedControls = 0;
    for (auto* child : settingsPage.getChildren())
    {
        if (auto* selector = dynamic_cast<juce::ComboBox*>(child))
        {
            if (selector->getTitle() == "Control-surface protocol") protocolSelector = selector;
            if (selector->getTitle() == "Dedicated control input") inputSelector = selector;
            if (selector->getTitle() == "Dedicated control output") outputSelector = selector;
        }
        if (auto* buttonControl = dynamic_cast<juce::Button*>(child))
        {
            if (buttonControl->getTitle() == "Refresh control-surface ports") refreshPorts = buttonControl;
            if (buttonControl->getTitle() == "Apply control-surface settings") applySettings = buttonControl;
        }
        if (auto* label = dynamic_cast<juce::Label*>(child);
            label != nullptr && label->getTitle() == "Control-surface connection status")
            connectionStatus = label;
        if (dynamic_cast<juce::ComboBox*>(child) != nullptr || dynamic_cast<juce::Button*>(child) != nullptr)
        {
            ++namedControls;
            expect(child->getTitle().isNotEmpty() && (!child->isEnabled() || child->getWantsKeyboardFocus())
                       && settingsPage.getLocalBounds().contains(child->getBounds()),
                   "Every control-surface selector/action is named, keyboard-focusable and inside its native layout.");
        }
    }
    expect(namedControls == 6 && protocolSelector && inputSelector && outputSelector
               && refreshPorts && applySettings && connectionStatus,
           "The settings audit covers the complete protocol/port/action form.");
    if (protocolSelector && inputSelector && outputSelector && refreshPorts && applySettings && connectionStatus)
    {
        refreshPorts->onClick();
        protocolSelector->setSelectedId(2, juce::sendNotificationSync);
        applySettings->onClick();
        expect(applied == 0 && connectionStatus->getText().contains("explicit"),
               "Enabling an incomplete surface produces a visible recoverable error rather than an implicit port choice.");
        inputSelector->setSelectedId(1, juce::sendNotificationSync);
        outputSelector->setSelectedId(1, juce::sendNotificationSync);
        applySettings->onClick();
        expect(applied == 1 && settingsSession->settings() == enabled,
               "Native selector/button actions persist only the explicitly chosen synthetic ports.");
        expect(inputSelector->getWantsKeyboardFocus() && outputSelector->getWantsKeyboardFocus()
                   && refreshPorts->getWantsKeyboardFocus() && applySettings->getWantsKeyboardFocus(),
               "Enabled surface controls rejoin native keyboard focus after configuration.");
    }
    juce::Image settingsImage(juce::Image::ARGB, settingsPage.getWidth(), settingsPage.getHeight(), true,
                             juce::SoftwareImageType());
    juce::Graphics settingsGraphics(settingsImage);
    settingsPage.paintEntireComponent(settingsGraphics, true);
    expect(settingsImage.getPixelAt(2, 2).getARGB() == studio::StudioColours::panel,
           "The native control form paints through a deterministic software renderer.");
    const auto imagePath = juce::SystemStats::getEnvironmentVariable("STUDIO_DUO_CONTROL_SURFACE_IMAGE", {});
    if (imagePath.isNotEmpty())
    {
        const auto destination = juce::File(imagePath).createOutputStream();
        expect(destination != nullptr && juce::PNGImageFormat().writeImageToStream(settingsImage, *destination),
               "The owned control-form screenshot can be retained without capturing the desktop.");
    }
    settingsPage.setLookAndFeel(nullptr);

    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    const auto trackId = track.id;
    const auto originalMix = studio::TrackMixState::fromTrack(track);
    auto modelState = studio::controlSurfaceStateForProject(project, trackId, true, false);
    expect(modelState.master && modelState.master->id == project.masterTrackId()
               && modelState.selectedTrackId == trackId && modelState.playing,
           "The project adapter reserves an independent master and stable root-track identities.");
    std::vector<studio::ControlSurfaceEvent> edits {
        { Type::volume, project.id, trackId, 0.25f }, { Type::pan, project.id, trackId, 0.75f }
    };
    juce::String error;
    auto mix = studio::prepareControlSurfaceMixEdit(project, edits, 2.0, error);
    studio::CommandStack history;
    expect(mix && mix->command && history.perform(std::move(mix->command), project, error)
               && std::abs(project.findTrack(trackId)->volumeDecibels + 42.0f) < 1e-5f
               && std::abs(project.findTrack(trackId)->pan - 0.5f) < 1e-5f,
           "One message-thread transaction applies coalesced fader and pan changes through real project commands.");
    expect(history.undo(project)
               && std::abs(project.findTrack(trackId)->volumeDecibels - originalMix.volumeDecibels) < 1e-5f
               && std::abs(project.findTrack(trackId)->pan - originalMix.pan) < 1e-5f,
           "Undo restores all control-surface mix values together.");
    project.findTrack(trackId)->automationArmed = true;
    project.findTrack(trackId)->automationMode = studio::AutomationMode::write;
    edits.resize(1);
    mix = studio::prepareControlSurfaceMixEdit(project, edits, 2.0, error);
    expect(mix && mix->command && history.perform(std::move(mix->command), project, error)
               && project.automationLanes.size() == 1
               && project.automationLanes.front().target.trackId == trackId,
           "Armed control changes use the existing automation recorder, not a separate untracked gain path.");
    expect(history.undo(project) && project.automationLanes.empty()
               && std::abs(project.findTrack(trackId)->volumeDecibels - originalMix.volumeDecibels) < 1e-5f,
           "A control mix and its recorded automation undo atomically.");
    project.findTrack(trackId)->automationMode = studio::AutomationMode::preview;
    mix = studio::prepareControlSurfaceMixEdit(project, edits, 2.0, error);
    expect(mix && !mix->command && mix->previews.size() == 1
               && std::abs(project.findTrack(trackId)->volumeDecibels - originalMix.volumeDecibels) < 1e-5f,
           "Preview mode stages a control value without silently changing the durable mix.");
    edits.front().projectId = "different-synthetic-project";
    expect(!studio::prepareControlSurfaceMixEdit(project, edits, 2.0, error) && error.isNotEmpty(),
           "A stale project identity cannot apply queued controls to a replacement project.");
}
