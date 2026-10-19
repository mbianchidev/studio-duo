#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace studio
{
enum class ControlSurfaceProtocol { disabled, mackieControl };

struct ControlSurfaceSettings
{
    ControlSurfaceProtocol protocol = ControlSurfaceProtocol::disabled;
    juce::String inputDeviceId;
    juce::String outputDeviceId;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] juce::Result validate() const;
    bool operator==(const ControlSurfaceSettings&) const = default;
};

[[nodiscard]] juce::Result validateControlSurfacePortUse(
    const ControlSurfaceSettings&, bool inputUsedForMusic, const juce::String& musicalOutputId);

struct ControlSurfaceTrack
{
    juce::String id;
    juce::String name;
    float volume = 60.0f / 72.0f;
    float pan = 0.5f;
    bool armed = false;
    bool solo = false;
    bool muted = false;
};

struct ControlSurfaceState
{
    juce::String projectId;
    std::vector<ControlSurfaceTrack> tracks;
    std::optional<ControlSurfaceTrack> master;
    juce::String selectedTrackId;
    bool playing = false;
    bool recording = false;
    bool loopEnabled = false;
    bool metronomeEnabled = false;
};

struct ControlSurfaceEvent
{
    enum class Type
    {
        volume, pan, arm, solo, mute, select,
        play, stop, record, rewind, forward, loop, metronome, save, undo,
        bankWhileTouched, unavailableTarget
    };
    Type type = Type::stop;
    juce::String projectId;
    juce::String trackId;
    float value = 0.0f;
};

class MackieControlProtocol
{
public:
    [[nodiscard]] juce::Result setState(ControlSurfaceState state);
    [[nodiscard]] std::optional<ControlSurfaceEvent> process(const juce::MidiMessage& message);
    void ignoreInput(const juce::MidiMessage& message);
    [[nodiscard]] std::vector<juce::MidiMessage> feedback();
    [[nodiscard]] static std::optional<juce::MidiMessage> handshakeReply(const juce::MidiMessage& message);
    [[nodiscard]] static juce::MidiMessage deviceQuery();
    [[nodiscard]] std::size_t bankOffset() const noexcept;
    void reset();
    void invalidateFeedback();

private:
    ControlSurfaceTrack* trackForStrip(int strip);
    ControlSurfaceTrack* findTrack(const juce::String& id);
    ControlSurfaceState state;
    std::size_t bank = 0;
    std::bitset<128> heldButtons;
    std::array<bool, 9> touched {};
    std::array<juce::String, 9> touchedTargets;
    std::array<int, 128> sentLeds {};
    std::array<int, 9> sentFaders {};
    std::array<int, 8> sentRings {};
    juce::String sentNames;
    bool sentDisplay = false;
    bool feedbackInitialised = false;
};

class ControlSurfaceMidiIo
{
public:
    virtual ~ControlSurfaceMidiIo() = default;
    virtual juce::Array<juce::MidiDeviceInfo> inputs() = 0;
    virtual juce::Array<juce::MidiDeviceInfo> outputs() = 0;
    virtual juce::Result open(const ControlSurfaceSettings&, juce::MidiInputCallback&) = 0;
    virtual void close() = 0;
    virtual void send(const juce::MidiMessage&) = 0;
};

class ControlSurfaceSession final : private juce::MidiInputCallback
{
public:
    explicit ControlSurfaceSession(std::unique_ptr<ControlSurfaceMidiIo> io = {});
    ~ControlSurfaceSession() override;
    [[nodiscard]] juce::Result configure(ControlSurfaceSettings settings);
    void setHardwareAvailable(bool available);
    void reconnect();
    void discardPendingCommands();
    void shutdown();
    void poll(ControlSurfaceState state, double nowMilliseconds,
              bool commandsAllowed, bool inputUsedForMusic, const juce::String& musicalOutputId);
    [[nodiscard]] juce::Array<juce::MidiDeviceInfo> inputs();
    [[nodiscard]] juce::Array<juce::MidiDeviceInfo> outputs();
    [[nodiscard]] bool hardwareAvailable() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const ControlSurfaceSettings& settings() const noexcept;
    [[nodiscard]] const juce::String& status() const noexcept;
    [[nodiscard]] bool hasError() const noexcept;
    std::function<void(std::span<const ControlSurfaceEvent>)> onEvents;
    std::function<ControlSurfaceState()> readState;
    std::function<void(const juce::String&, bool)> onStatusChanged;

private:
    static constexpr int queueCapacity = 512;
    struct Packet
    {
        std::array<juce::uint8, 64> bytes {};
        int size = 0;
        std::uint64_t generation = 0;
        bool commandsAllowed = false;
        double receivedAtMilliseconds = 0.0;
    };
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void disconnect();
    void setStatus(juce::String message, bool error);
    std::unique_ptr<ControlSurfaceMidiIo> io;
    ControlSurfaceSettings configuration;
    MackieControlProtocol protocol;
    juce::AbstractFifo queue { queueCapacity };
    juce::SpinLock inputWriteLock;
    std::array<Packet, queueCapacity> packets;
    std::atomic<std::uint64_t> generation { 0 };
    std::atomic<bool> overflowed { false };
    std::atomic<bool> commandInputAllowed { false };
    bool midiAvailable = false;
    bool portsOpen = false;
    bool faulted = false;
    bool statusError = false;
    double nextConnectionCheck = 0.0;
    juce::String context;
    juce::String statusMessage { "Control surface disabled." };
};

class MusicalMidiInputCollector final : public juce::MidiMessageCollector
{
public:
    void setReservedInput(juce::String identifier);
    [[nodiscard]] bool acceptsDevice(const juce::String& identifier) const;
    void collectFromDevice(const juce::String& identifier, const juce::MidiMessage& message);
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;

private:
    juce::CriticalSection reservationLock;
    juce::String reservedInput;
};
}
