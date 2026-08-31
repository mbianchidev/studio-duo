#pragma once

#include "RecordingWaveform.h"
#include "model/ProjectModel.h"
#include "plugin_host/PluginBridgeClient.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace studio
{
class StudioAudioEngine final : private juce::AudioIODeviceCallback
{
public:
    struct RecordingResult
    {
        juce::Result result { juce::Result::ok() };
        juce::File file;
        double durationSeconds = 0.0;
        juce::String warning;
    };

    struct PluginRuntimeRequest
    {
        juce::String trackId;
        juce::String insertId;
        juce::String name;
        std::optional<juce::PluginDescription> description;
        juce::MemoryBlock state;
        int latencySamples = 0;
        double tailSeconds = 0.0;
        std::uint64_t catalogRevision = 0;
        bool bypassed = false;
        bool missing = false;
    };

    struct PluginRuntimeStatus
    {
        enum class State
        {
            bypassed,
            missing,
            loading,
            ready,
            failed
        };

        juce::String trackId;
        juce::String insertId;
        juce::String name;
        State state = State::loading;
        juce::String message;
        int latencySamples = 0;
        double tailSeconds = 0.0;
    };

    StudioAudioEngine();
    ~StudioAudioEngine() override;

    juce::Result initialise(juce::AudioDeviceManager& manager);
    void shutdown();

    juce::Result updateProject(const Project& project,
                               std::vector<PluginRuntimeRequest> pluginRequests = {});
    void play() noexcept;
    void pause() noexcept;
    void stop() noexcept;
    void seekSeconds(double seconds) noexcept;
    void setMetronomeEnabled(bool enabled) noexcept;
    void setInputMonitoring(bool enabled, int firstInputChannel, int channels) noexcept;

    [[nodiscard]] bool isPlaying() const noexcept;
    [[nodiscard]] bool isRecording() const noexcept;
    [[nodiscard]] double positionSeconds() const noexcept;
    [[nodiscard]] float leftPeak() const noexcept;
    [[nodiscard]] float rightPeak() const noexcept;
    [[nodiscard]] double currentSampleRate() const noexcept;
    [[nodiscard]] double recordingDurationSeconds() const noexcept;
    [[nodiscard]] std::vector<float> recordingWaveform() const;
    [[nodiscard]] std::vector<PluginRuntimeStatus> pluginRuntimeStatuses() const;
    [[nodiscard]] std::uint64_t pluginLateBlockCount() const noexcept;
    [[nodiscard]] bool pluginRuntimeTransitionPending() const;
    void forcePluginRuntimeReload(const Project& project,
                                  std::vector<PluginRuntimeRequest> pluginRequests);

    juce::Result startRecording(const juce::File& destination,
                                int firstInputChannel,
                                int channels);
    RecordingResult stopRecording();
    void stopRecordingAsync(std::function<void(RecordingResult)> completion);
    std::optional<double> audioFileDuration(const juce::File& source, juce::String& error);
    juce::Result renderToWav(const Project& project, const juce::File& destination, double sampleRate);

private:
    struct RenderClip
    {
        std::int64_t startSample = 0;
        std::int64_t sourceOffsetSamples = 0;
        std::int64_t lengthSamples = 0;
        float gain = 1.0f;
        juce::AudioBuffer<float> samples;
    };

    struct RenderSource
    {
        struct DelayCompensator
        {
            int delaySamples = 0;
            int writePosition = 0;
            juce::AudioBuffer<float> buffer;
        };

        std::uint64_t runtimeKey = 0;
        int runtimeLatencySamples = 0;
        float volumeGain = 1.0f;
        float pan = 0.0f;
        bool audible = true;
        bool isParentContent = false;
        std::vector<RenderClip> clips;
        juce::AudioBuffer<float> processingBuffer {
            2,
            PluginBridgeSharedState::maxBlockSize
        };
        DelayCompensator compensation;
    };

    struct RenderTrack
    {
        std::uint64_t runtimeKey = 0;
        float volumeGain = 1.0f;
        float pan = 0.0f;
        bool audible = true;
        int runtimeLatencySamples = 0;
        std::vector<RenderSource> sources;
        juce::AudioBuffer<float> processingBuffer {
            2,
            PluginBridgeSharedState::maxBlockSize
        };
        RenderSource::DelayCompensator compensation;
    };

    struct RenderSnapshot
    {
        double sampleRate = 48000.0;
        int processingQuantum = 512;
        std::int64_t contentLengthSamples = 0;
        std::int64_t lengthSamples = 0;
        std::int64_t loopStartSample = 0;
        std::int64_t loopEndSample = 0;
        double tempo = 120.0;
        bool loopEnabled = false;
        std::uint64_t masterRuntimeKey = 0;
        float masterGain = 1.0f;
        float masterPan = 0.0f;
        bool masterAudible = true;
        std::vector<RenderTrack> tracks;
        juce::AudioBuffer<float> masterBuffer {
            2,
            PluginBridgeSharedState::maxBlockSize
        };
        juce::AudioBuffer<float> clickBuffer {
            2,
            PluginBridgeSharedState::maxBlockSize
        };
        RenderSource::DelayCompensator clickCompensation;
    };

    struct InsertRuntime
    {
        juce::String insertId;
        juce::String name;
        std::unique_ptr<PluginBridgeClient> bridge;
        RenderSource::DelayCompensator failureDelay;
    };

    struct TrackRuntime
    {
        std::uint64_t key = 0;
        std::vector<InsertRuntime> inserts;
    };

    struct PluginRuntimeGraph
    {
        std::vector<TrackRuntime> tracks;
    };

    class LockFreeRecorder final : private juce::Thread
    {
    public:
        LockFreeRecorder();
        ~LockFreeRecorder() override;

        juce::Result start(const juce::File& destination,
                           double sampleRate,
                           int channels,
                           int firstInputChannel);
        RecordingResult stop();
        void stopAccepting() noexcept;
        RecordingResult finishStop();
        void push(const float* const* inputs, int inputChannels, int samples) noexcept;
        [[nodiscard]] bool isActive() const noexcept;
        [[nodiscard]] double capturedDurationSeconds() const noexcept;
        [[nodiscard]] std::vector<float> waveform() const;

    private:
        void run() override;

        static constexpr int capacitySamples = 1 << 20;
        juce::AbstractFifo fifo { capacitySamples };
        juce::AudioBuffer<float> ringBuffer { 2, capacitySamples };
        std::unique_ptr<juce::AudioFormatWriter> writer;
        std::atomic<bool> accepting { false };
        std::atomic<std::int64_t> samplesWritten { 0 };
        std::atomic<std::int64_t> samplesDropped { 0 };
        std::atomic<std::int64_t> samplesCaptured { 0 };
        juce::File outputFile;
        double recordingSampleRate = 48000.0;
        int recordingChannels = 1;
        int recordingFirstInputChannel = 0;
        RecordingWaveform recordingWaveform;
    };

    std::optional<RenderSnapshot> buildSnapshot(const Project& project,
                                                double sampleRate,
                                                const std::vector<PluginRuntimeRequest>& pluginRequests,
                                                juce::String& error);
    std::optional<juce::AudioBuffer<float>> readAndResample(const juce::File& source,
                                                           double targetSampleRate,
                                                           juce::String& error);
    void configureRuntimeTiming(RenderSnapshot& snapshot,
                                const std::vector<PluginRuntimeRequest>& pluginRequests) const;
    static void mixSample(const RenderSnapshot& snapshot,
                          std::int64_t timelineSample,
                          float& left,
                          float& right) noexcept;
    static void renderSourceBlock(RenderSource& source,
                                  std::int64_t timelineSample,
                                  int samples,
                                  bool loopEnabled,
                                  std::int64_t loopStartSample,
                                  std::int64_t loopEndSample) noexcept;
    static void applyTrackGainAndPan(juce::AudioBuffer<float>& buffer,
                                     int samples,
                                     float gain,
                                     float pan) noexcept;
    static void applyDelayCompensation(juce::AudioBuffer<float>& buffer,
                                       int samples,
                                       RenderSource::DelayCompensator& delay) noexcept;
    void processRuntimeChain(std::uint64_t runtimeKey,
                             juce::AudioBuffer<float>& buffer) noexcept;
    void requestPluginRuntime(std::vector<PluginRuntimeRequest> requests,
                              RenderSnapshot snapshot);
    void runPluginRuntimeBuilder();
    [[nodiscard]] juce::String pluginRuntimeFingerprint(
        const std::vector<PluginRuntimeRequest>& requests) const;
    [[nodiscard]] static std::uint64_t runtimeKey(const juce::String& trackId) noexcept;
    [[nodiscard]] static std::uint64_t renderPair(std::uint64_t generation,
                                                  int snapshotIndex,
                                                  int runtimeIndex) noexcept;
    int chooseWritableRuntime() const noexcept;
    static void addMetronome(const RenderSnapshot& snapshot,
                             std::int64_t timelineSample,
                             float& left,
                             float& right) noexcept;
    int chooseWritableSnapshot() const noexcept;

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

    juce::AudioDeviceManager* deviceManager = nullptr;
    juce::AudioFormatManager formatManager;
    std::array<RenderSnapshot, 3> snapshots;
    std::atomic<std::uint64_t> activeRenderPair { 0 };
    std::array<std::atomic<std::uint64_t>, 3> snapshotGenerations {};
    std::atomic<int> activeSnapshot { 0 };
    std::atomic<int> readingSnapshot { -1 };
    std::atomic<std::int64_t> playheadSample { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> deviceBlockSize { 512 };
    std::atomic<bool> playing { false };
    std::atomic<bool> metronomeEnabled { true };
    std::atomic<bool> monitoringEnabled { false };
    std::atomic<int> monitoringFirstInput { 0 };
    std::atomic<int> monitoringChannels { 1 };
    std::atomic<float> outputLeftPeak { 0.0f };
    std::atomic<float> outputRightPeak { 0.0f };
    std::array<PluginRuntimeGraph, 3> pluginRuntimeGraphs;
    std::array<std::atomic<std::uint64_t>, 3> pluginRuntimeGenerations {};
    std::atomic<int> activePluginRuntime { 0 };
    std::atomic<int> readingPluginRuntime { -1 };
    juce::ThreadPool pluginRuntimeBuilder { 1 };
    mutable juce::CriticalSection pluginRequestLock;
    std::vector<PluginRuntimeRequest> pendingPluginRequests;
    std::optional<RenderSnapshot> pendingPluginSnapshot;
    std::uint64_t pendingSnapshotGeneration = 0;
    juce::String pendingPluginFingerprint;
    juce::String buildingPluginFingerprint;
    juce::String desiredPluginFingerprint;
    juce::String activePluginFingerprint;
    std::uint64_t pendingPluginGeneration = 0;
    std::uint64_t desiredPluginGeneration = 0;
    std::uint64_t activePluginGeneration = 0;
    bool hasPendingPluginRequest = false;
    bool pluginBuilderRunning = false;
    std::atomic<bool> shuttingDown { false };
    mutable juce::CriticalSection pluginStatusLock;
    std::vector<PluginRuntimeStatus> pluginStatuses;
    std::atomic<std::uint64_t> pluginLateBlocks { 0 };
    LockFreeRecorder recorder;
    juce::ThreadPool recordingFinalizer { 1 };
    std::atomic<bool> recordingFinalizing { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StudioAudioEngine)
};
}
