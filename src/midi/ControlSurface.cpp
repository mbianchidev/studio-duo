#include "ControlSurface.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace studio
{
namespace
{
class NativeControlSurfaceMidiIo final : public ControlSurfaceMidiIo
{
public:
    ~NativeControlSurfaceMidiIo() override { close(); }
    juce::Array<juce::MidiDeviceInfo> inputs() override { return juce::MidiInput::getAvailableDevices(); }
    juce::Array<juce::MidiDeviceInfo> outputs() override { return juce::MidiOutput::getAvailableDevices(); }

    juce::Result open(const ControlSurfaceSettings& settings, juce::MidiInputCallback& callback) override
    {
        close();
        output = juce::MidiOutput::openDevice(settings.outputDeviceId);
        if (output == nullptr)
            return juce::Result::fail("Cannot open the selected control-surface output. Close other MIDI clients or reconnect that device.");
        input = juce::MidiInput::openDevice(settings.inputDeviceId, &callback);
        if (input == nullptr)
        {
            close();
            return juce::Result::fail("Cannot open the selected control-surface input. Disable it for musical input and close other MIDI clients.");
        }
        input->start();
        return juce::Result::ok();
    }

    void close() override
    {
        if (input != nullptr)
            input->stop();
        input.reset();
        output.reset();
    }

    void send(const juce::MidiMessage& message) override
    {
        if (output != nullptr)
            output->sendMessageNow(message);
    }

private:
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
};

bool normalised(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool continuous(ControlSurfaceEvent::Type type)
{
    return type == ControlSurfaceEvent::Type::volume || type == ControlSurfaceEvent::Type::pan;
}
}

bool ControlSurfaceSettings::enabled() const noexcept
{
    return protocol == ControlSurfaceProtocol::mackieControl;
}

juce::Result ControlSurfaceSettings::validate() const
{
    if (protocol != ControlSurfaceProtocol::disabled && protocol != ControlSurfaceProtocol::mackieControl)
        return juce::Result::fail("The selected control-surface protocol is unsupported.");
    if (enabled() && (inputDeviceId.isEmpty() || outputDeviceId.isEmpty()))
        return juce::Result::fail("Choose an explicit dedicated control-surface input and output.");
    return juce::Result::ok();
}

juce::Result validateControlSurfacePortUse(
    const ControlSurfaceSettings& settings, bool inputUsedForMusic, const juce::String& musicalOutputId)
{
    if (const auto valid = settings.validate(); valid.failed())
        return valid;
    if (!settings.enabled())
        return juce::Result::ok();
    if (inputUsedForMusic)
        return juce::Result::fail("The selected control input is enabled for musical MIDI. Disable it in Audio / MIDI before connecting the surface.");
    if (musicalOutputId == settings.outputDeviceId)
        return juce::Result::fail("The selected control output is also the musical MIDI output. Choose separate dedicated ports.");
    return juce::Result::ok();
}

juce::Result MackieControlProtocol::setState(ControlSurfaceState value)
{
    if (value.projectId.isEmpty())
        return juce::Result::fail("Control-surface feedback requires a project identity.");
    std::set<juce::String> ids;
    const auto valid = [&ids](const ControlSurfaceTrack& track)
    {
        return track.id.isNotEmpty() && ids.insert(track.id).second
            && normalised(track.volume) && normalised(track.pan);
    };
    if (!std::all_of(value.tracks.begin(), value.tracks.end(), valid)
        || (value.master && !valid(*value.master)))
        return juce::Result::fail("Control-surface feedback contains duplicate targets or invalid fader/pan values.");
    if (state.projectId != value.projectId)
        reset();
    bank = std::min(bank, value.tracks.empty() ? std::size_t { 0 } : value.tracks.size() - 1);
    state = std::move(value);
    return juce::Result::ok();
}

ControlSurfaceTrack* MackieControlProtocol::trackForStrip(int strip)
{
    if (strip == 8)
        return state.master ? &*state.master : nullptr;
    if (strip < 0 || strip >= 8 || bank + static_cast<std::size_t>(strip) >= state.tracks.size())
        return nullptr;
    return &state.tracks[bank + static_cast<std::size_t>(strip)];
}

ControlSurfaceTrack* MackieControlProtocol::findTrack(const juce::String& id)
{
    if (state.master && state.master->id == id)
        return &*state.master;
    const auto found = std::find_if(state.tracks.begin(), state.tracks.end(),
        [&id](const auto& track) { return track.id == id; });
    return found != state.tracks.end() ? &*found : nullptr;
}

void MackieControlProtocol::ignoreInput(const juce::MidiMessage& message)
{
    if (message.getChannel() != 1 || !message.isNoteOnOrOff())
        return;
    const auto note = message.getNoteNumber();
    const auto pressed = message.isNoteOn();
    heldButtons.set(static_cast<std::size_t>(note), pressed);
    if (note >= 104 && note <= 112)
    {
        const auto strip = static_cast<std::size_t>(note - 104);
        if (pressed && !touched[strip])
        {
            const auto* track = trackForStrip(static_cast<int>(strip));
            touchedTargets[strip] = track != nullptr ? track->id : juce::String();
        }
        touched[strip] = pressed;
        if (!pressed)
        {
            touchedTargets[strip].clear();
            sentFaders[strip] = -1;
        }
    }
}

std::optional<ControlSurfaceEvent> MackieControlProtocol::process(const juce::MidiMessage& message)
{
    using Type = ControlSurfaceEvent::Type;
    const auto event = [this](Type type, juce::String id = {}, float value = 0.0f)
    { return ControlSurfaceEvent { type, state.projectId, std::move(id), value }; };
    if (message.isPitchWheel() && message.getChannel() >= 1 && message.getChannel() <= 9)
    {
        const auto strip = message.getChannel() - 1;
        auto* track = touched[static_cast<std::size_t>(strip)]
            ? findTrack(touchedTargets[static_cast<std::size_t>(strip)]) : trackForStrip(strip);
        if (track == nullptr)
            return touchedTargets[static_cast<std::size_t>(strip)].isNotEmpty()
                ? std::optional(event(Type::unavailableTarget, touchedTargets[static_cast<std::size_t>(strip)]))
                : std::nullopt;
        track->volume = static_cast<float>(message.getPitchWheelValue()) / 16383.0f;
        return event(Type::volume, track->id, track->volume);
    }
    if (message.getChannel() != 1)
        return std::nullopt;
    if (message.isController() && message.getControllerNumber() >= 16 && message.getControllerNumber() <= 23)
    {
        auto* track = trackForStrip(message.getControllerNumber() - 16);
        const auto raw = message.getControllerValue();
        const auto delta = (raw & 0x3f) * ((raw & 0x40) != 0 ? -1 : 1);
        if (track == nullptr || delta == 0)
            return std::nullopt;
        track->pan = juce::jlimit(0.0f, 1.0f, track->pan + static_cast<float>(delta) * 0.01f);
        return event(Type::pan, track->id, track->pan);
    }
    if (!message.isNoteOnOrOff())
        return std::nullopt;
    const auto note = message.getNoteNumber();
    const auto pressed = message.isNoteOn();
    if (note >= 104 && note <= 112)
    {
        ignoreInput(message);
        return std::nullopt;
    }
    const auto held = heldButtons.test(static_cast<std::size_t>(note));
    heldButtons.set(static_cast<std::size_t>(note), pressed);
    if (!pressed || held)
        return std::nullopt;
    if (note >= 0 && note <= 39)
    {
        auto* track = trackForStrip(note % 8);
        if (track == nullptr)
            return std::nullopt;
        if (note < 8)
        {
            track->armed = !track->armed;
            return event(Type::arm, track->id, track->armed ? 1.0f : 0.0f);
        }
        if (note < 16)
        {
            track->solo = !track->solo;
            return event(Type::solo, track->id, track->solo ? 1.0f : 0.0f);
        }
        if (note < 24)
        {
            track->muted = !track->muted;
            return event(Type::mute, track->id, track->muted ? 1.0f : 0.0f);
        }
        if (note < 32)
        {
            state.selectedTrackId = track->id;
            return event(Type::select, track->id);
        }
        track->pan = 0.5f;
        return event(Type::pan, track->id, track->pan);
    }
    if (note >= 46 && note <= 49)
    {
        if (std::any_of(touched.begin(), touched.begin() + 8, [](bool value) { return value; }))
            return event(Type::bankWhileTouched);
        const auto previous = bank;
        const auto last = state.tracks.empty() ? std::size_t { 0 } : state.tracks.size() - 1;
        if (note == 46) bank = bank >= 8 ? bank - 8 : 0;
        if (note == 47 && bank < last / 8 * 8) bank = std::min(bank + 8, last / 8 * 8);
        if (note == 48 && bank > 0) --bank;
        if (note == 49) bank = std::min(bank + 1, last);
        if (bank != previous)
            invalidateFeedback();
        return std::nullopt;
    }
    switch (note)
    {
        case 80: return event(Type::save);
        case 81: return event(Type::undo);
        case 86: state.loopEnabled = !state.loopEnabled; return event(Type::loop);
        case 89: state.metronomeEnabled = !state.metronomeEnabled; return event(Type::metronome);
        case 91: return event(Type::rewind);
        case 92: return event(Type::forward);
        case 93: state.playing = state.recording = false; return event(Type::stop);
        case 94: state.playing = true; return event(Type::play);
        case 95: state.recording = true; return event(Type::record);
        default: return std::nullopt;
    }
}

std::vector<juce::MidiMessage> MackieControlProtocol::feedback()
{
    if (!feedbackInitialised)
        invalidateFeedback();
    std::vector<juce::MidiMessage> messages;
    const auto led = [&](int note, bool on)
    {
        const auto value = on ? 127 : 0;
        auto& previous = sentLeds[static_cast<std::size_t>(note)];
        if (previous != value)
        {
            messages.push_back(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(value)));
            previous = value;
        }
    };
    juce::String names;
    for (int strip = 0; strip < 9; ++strip)
    {
        const auto index = static_cast<std::size_t>(strip);
        const auto* track = trackForStrip(strip);
        const auto fader = track != nullptr ? juce::roundToInt(track->volume * 16383.0f) : 0;
        if (!touched[index] && sentFaders[index] != fader)
        {
            messages.push_back(juce::MidiMessage::pitchWheel(strip + 1, fader));
            sentFaders[index] = fader;
        }
        if (strip == 8)
            continue;
        led(strip, track != nullptr && track->armed);
        led(8 + strip, track != nullptr && track->solo);
        led(16 + strip, track != nullptr && track->muted);
        led(24 + strip, track != nullptr && track->id == state.selectedTrackId);
        const auto ring = track != nullptr ? 0x10 | (1 + juce::roundToInt(track->pan * 10.0f))
            | (std::abs(track->pan - 0.5f) < 0.0001f ? 0x40 : 0) : 0;
        if (sentRings[index] != ring)
        {
            messages.push_back(juce::MidiMessage::controllerEvent(1, 48 + strip, ring));
            sentRings[index] = ring;
        }
        const auto name = track != nullptr ? track->name : juce::String();
        for (int character = 0; character < 6; ++character)
        {
            const auto code = character < name.length() ? name[character] : juce::juce_wchar(' ');
            names << juce::String::charToString(code >= 32 && code < 127 ? code : juce::juce_wchar('?'));
        }
        names << " ";
    }
    led(93, !state.playing);
    led(94, state.playing);
    led(95, state.recording);
    led(86, state.loopEnabled);
    led(89, state.metronomeEnabled);
    led(115, std::any_of(state.tracks.begin(), state.tracks.end(), [](const auto& track) { return track.solo; }));
    if (!sentDisplay || names != sentNames)
    {
        std::array<juce::uint8, 62> display { 0, 0, 0x66, 0x14, 0x12, 0 };
        for (int index = 0; index < 56; ++index)
            display[static_cast<std::size_t>(index + 6)] = static_cast<juce::uint8>(names[index]);
        messages.push_back(juce::MidiMessage::createSysExMessage(display.data(), static_cast<int>(display.size())));
        sentNames = names;
        sentDisplay = true;
    }
    return messages;
}

std::optional<juce::MidiMessage> MackieControlProtocol::handshakeReply(const juce::MidiMessage& message)
{
    if (!message.isSysEx() || message.getSysExDataSize() != 16)
        return std::nullopt;
    const auto* data = message.getSysExData();
    if (data[0] != 0 || data[1] != 0 || data[2] != 0x66 || data[3] != 0x14 || data[4] != 1)
        return std::nullopt;
    std::array<juce::uint8, 16> reply;
    std::copy_n(data, reply.size(), reply.begin());
    reply[4] = 2;
    const int a = data[12], b = data[13], c = data[14], d = data[15];
    reply[12] = static_cast<juce::uint8>((a + (b ^ 0x0a) - d) & 0x7f);
    reply[13] = static_cast<juce::uint8>(((c >> 4) ^ (a + d)) & 0x7f);
    reply[14] = static_cast<juce::uint8>(((d - (c << 2)) ^ (a | b)) & 0x7f);
    reply[15] = static_cast<juce::uint8>((b - c + (0xf0 ^ (d << 4))) & 0x7f);
    return juce::MidiMessage::createSysExMessage(reply.data(), static_cast<int>(reply.size()));
}

juce::MidiMessage MackieControlProtocol::deviceQuery()
{
    const juce::uint8 bytes[] { 0, 0, 0x66, 0x14, 0 };
    return juce::MidiMessage::createSysExMessage(bytes, static_cast<int>(sizeof(bytes)));
}

std::size_t MackieControlProtocol::bankOffset() const noexcept { return bank; }

void MackieControlProtocol::reset()
{
    bank = 0;
    heldButtons.reset();
    touched.fill(false);
    touchedTargets.fill({});
    invalidateFeedback();
}

void MackieControlProtocol::invalidateFeedback()
{
    sentLeds.fill(-1);
    sentFaders.fill(-1);
    sentRings.fill(-1);
    sentDisplay = false;
    feedbackInitialised = true;
}

ControlSurfaceSession::ControlSurfaceSession(std::unique_ptr<ControlSurfaceMidiIo> backend)
    : io(backend != nullptr ? std::move(backend) : std::make_unique<NativeControlSurfaceMidiIo>())
{
}

ControlSurfaceSession::~ControlSurfaceSession()
{
    onEvents = {};
    onStatusChanged = {};
    readState = {};
    shutdown();
}

juce::Result ControlSurfaceSession::configure(ControlSurfaceSettings value)
{
    if (const auto result = value.validate(); result.failed())
        return result;
    disconnect();
    configuration = std::move(value);
    faulted = false;
    nextConnectionCheck = 0.0;
    setStatus(configuration.enabled() ? "Waiting for the dedicated control-surface ports." : "Control surface disabled.", false);
    return juce::Result::ok();
}

void ControlSurfaceSession::setHardwareAvailable(bool available)
{
    if (midiAvailable == available)
        return;
    midiAvailable = available;
    nextConnectionCheck = 0.0;
    if (!available)
    {
        disconnect();
        setStatus(configuration.enabled() ? "Control surface suspended: MIDI is unavailable." : "Control surface disabled.", false);
    }
}

void ControlSurfaceSession::reconnect()
{
    disconnect();
    faulted = false;
    nextConnectionCheck = 0.0;
    setStatus(configuration.enabled() ? "Reconnecting the selected control-surface ports." : "Control surface disabled.", false);
}

void ControlSurfaceSession::shutdown()
{
    midiAvailable = false;
    disconnect();
    setStatus(configuration.enabled() ? "Control surface stopped." : "Control surface disabled.", false);
}

void ControlSurfaceSession::discardPendingCommands()
{
    commandInputAllowed.store(false, std::memory_order_release);
    generation.fetch_add(1, std::memory_order_acq_rel);
}

void ControlSurfaceSession::disconnect()
{
    commandInputAllowed.store(false, std::memory_order_release);
    io->close();
    portsOpen = false;
    generation.fetch_add(1, std::memory_order_acq_rel);
    {
        const juce::SpinLock::ScopedLockType lock(inputWriteLock);
        queue.reset();
    }
    overflowed.store(false, std::memory_order_release);
    protocol.reset();
}

void ControlSurfaceSession::setStatus(juce::String message, bool error)
{
    if (statusMessage == message && statusError == error)
        return;
    statusMessage = std::move(message);
    statusError = error;
    if (onStatusChanged)
        onStatusChanged(statusMessage, statusError);
    else if (error)
        juce::Logger::writeToLog(statusMessage);
}

void ControlSurfaceSession::poll(ControlSurfaceState state, double nowMilliseconds,
                                bool commandsAllowed, bool inputUsedForMusic, const juce::String& musicalOutputId)
{
    if (!configuration.enabled() || !midiAvailable || faulted)
        return;
    if (state.projectId != context)
    {
        generation.fetch_add(1, std::memory_order_acq_rel);
        context = state.projectId;
    }
    if (const auto valid = protocol.setState(std::move(state)); valid.failed())
    {
        disconnect();
        faulted = true;
        setStatus(valid.getErrorMessage(), true);
        return;
    }
    commandInputAllowed.store(commandsAllowed, std::memory_order_release);
    if (const auto use = validateControlSurfacePortUse(configuration, inputUsedForMusic, musicalOutputId); use.failed())
    {
        if (portsOpen)
            disconnect();
        setStatus(use.getErrorMessage(), true);
        nextConnectionCheck = nowMilliseconds + 1000.0;
        return;
    }
    if (nowMilliseconds >= nextConnectionCheck)
    {
        nextConnectionCheck = nowMilliseconds + 1000.0;
        const auto inputDevices = io->inputs();
        const auto outputDevices = io->outputs();
        const auto present = [](const auto& devices, const juce::String& identifier)
        {
            return std::any_of(devices.begin(), devices.end(),
                [&identifier](const auto& device) { return device.identifier == identifier; });
        };
        if (!present(inputDevices, configuration.inputDeviceId) || !present(outputDevices, configuration.outputDeviceId))
        {
            if (portsOpen)
                disconnect();
            setStatus("A selected control-surface port is unavailable. Waiting for the same explicit device identities.", true);
            return;
        }
        if (!portsOpen)
        {
            if (const auto opened = io->open(configuration, *this); opened.failed())
            {
                disconnect();
                setStatus(opened.getErrorMessage(), true);
                return;
            }
            portsOpen = true;
            protocol.invalidateFeedback();
            io->send(MackieControlProtocol::deviceQuery());
            setStatus("Mackie Control ports connected.", false);
        }
    }
    if (!portsOpen)
        return;
    if (overflowed.exchange(false, std::memory_order_acq_rel))
    {
        disconnect();
        faulted = true;
        setStatus("Control-surface input overflowed. Pending commands were discarded; use Reconnect to resume.", true);
        return;
    }
    std::vector<ControlSurfaceEvent> pending;
    auto discardedOldInput = false;
    auto acceptedInput = false;
    const auto dispatchGeneration = generation.load(std::memory_order_acquire);
    const auto active = [&]
    { return portsOpen && generation.load(std::memory_order_acquire) == dispatchGeneration; };
    const auto flush = [&]
    {
        if (!pending.empty() && onEvents)
            onEvents(pending);
        pending.clear();
        return active();
    };
    const auto count = queue.getNumReady();
    for (int index = 0; index < count; ++index)
    {
        int first = 0, firstSize = 0, second = 0, secondSize = 0;
        queue.prepareToRead(1, first, firstSize, second, secondSize);
        if (firstSize == 0)
            break;
        const auto packet = packets[static_cast<std::size_t>(first)];
        queue.finishedRead(1);
        if (packet.generation != generation.load(std::memory_order_acquire))
            continue;
        const juce::MidiMessage message(packet.bytes.data(), packet.size, 0.0);
        if (const auto reply = MackieControlProtocol::handshakeReply(message))
        {
            io->send(*reply);
            protocol.invalidateFeedback();
        }
        if (message.isSysEx() && message.getSysExDataSize() == 12)
        {
            const auto* data = message.getSysExData();
            if (data[0] == 0 && data[1] == 0 && data[2] == 0x66 && data[3] == 0x14 && data[4] == 3)
                protocol.invalidateFeedback();
        }
        const auto oldInput = nowMilliseconds - packet.receivedAtMilliseconds > 250.0;
        if (!commandsAllowed || !packet.commandsAllowed || oldInput)
        {
            protocol.ignoreInput(message);
            discardedOldInput = discardedOldInput || oldInput;
            continue;
        }
        if (const auto event = protocol.process(message))
        {
            acceptedInput = true;
            if (continuous(event->type))
            {
                const auto previous = std::find_if(pending.begin(), pending.end(), [&](const auto& value)
                { return value.type == event->type && value.trackId == event->trackId; });
                if (previous != pending.end())
                    *previous = *event;
                else
                    pending.push_back(*event);
            }
            else
            {
                if (!flush())
                    return;
                pending.push_back(*event);
                if (!flush())
                    return;
            }
        }
    }
    if (!flush())
        return;
    if (discardedOldInput)
        setStatus("Old control commands were discarded while the interface was busy. Release and press the controls again.", true);
    else if (acceptedInput)
        setStatus("Mackie Control ports connected.", false);
    if (readState)
        if (const auto valid = protocol.setState(readState()); valid.failed())
        {
            disconnect();
            faulted = true;
            setStatus(valid.getErrorMessage(), true);
            return;
        }
    if (!active())
        return;
    for (const auto& message : protocol.feedback())
        io->send(message);
}

void ControlSurfaceSession::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    const auto packetGeneration = generation.load(std::memory_order_acquire);
    const auto allowed = commandInputAllowed.load(std::memory_order_acquire);
    const auto size = message.getRawDataSize();
    if (size <= 0 || size > static_cast<int>(Packet().bytes.size()))
    {
        overflowed.store(true, std::memory_order_release);
        return;
    }
    const juce::SpinLock::ScopedLockType lock(inputWriteLock);
    int first = 0, firstSize = 0, second = 0, secondSize = 0;
    queue.prepareToWrite(1, first, firstSize, second, secondSize);
    if (firstSize == 0)
    {
        overflowed.store(true, std::memory_order_release);
        return;
    }
    auto& packet = packets[static_cast<std::size_t>(first)];
    packet.size = size;
    packet.generation = packetGeneration;
    packet.commandsAllowed = allowed;
    const auto timestamp = message.getTimeStamp();
    packet.receivedAtMilliseconds = std::isfinite(timestamp) && timestamp > 0.0
        ? timestamp * 1000.0 : juce::Time::getMillisecondCounterHiRes();
    std::copy_n(message.getRawData(), size, packet.bytes.begin());
    queue.finishedWrite(1);
}

juce::Array<juce::MidiDeviceInfo> ControlSurfaceSession::inputs()
{
    return midiAvailable ? io->inputs() : juce::Array<juce::MidiDeviceInfo>();
}

juce::Array<juce::MidiDeviceInfo> ControlSurfaceSession::outputs()
{
    return midiAvailable ? io->outputs() : juce::Array<juce::MidiDeviceInfo>();
}

bool ControlSurfaceSession::hardwareAvailable() const noexcept { return midiAvailable; }
bool ControlSurfaceSession::connected() const noexcept { return portsOpen; }
const ControlSurfaceSettings& ControlSurfaceSession::settings() const noexcept { return configuration; }
const juce::String& ControlSurfaceSession::status() const noexcept { return statusMessage; }
bool ControlSurfaceSession::hasError() const noexcept { return statusError; }

void MusicalMidiInputCollector::setReservedInput(juce::String identifier)
{
    const juce::ScopedLock lock(reservationLock);
    reservedInput = std::move(identifier);
}

bool MusicalMidiInputCollector::acceptsDevice(const juce::String& identifier) const
{
    const juce::ScopedLock lock(reservationLock);
    return reservedInput.isEmpty() || identifier != reservedInput;
}

void MusicalMidiInputCollector::collectFromDevice(const juce::String& identifier, const juce::MidiMessage& message)
{
    if (acceptsDevice(identifier))
        addMessageToQueue(message);
}

void MusicalMidiInputCollector::handleIncomingMidiMessage(juce::MidiInput* input, const juce::MidiMessage& message)
{
    collectFromDevice(input != nullptr ? input->getIdentifier() : juce::String(), message);
}
}
