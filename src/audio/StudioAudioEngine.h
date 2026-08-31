#pragma once

#include "RecordingWaveform.h"
#include "model/ProjectModel.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
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

    StudioAudioEngine();
    ~StudioAudioEngine() override;

    juce::Result initialise(juce::AudioDeviceManager& manager);
    void shutdown();

    juce::Result updateProject(const Project& project);
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

    juce::Result startRecording(const juce::File& destination,
                                int firstInputChannel,
                                int channels);
    RecordingResult stopRecording();
    std::optional<double> audioFileDuration(const juce::File& source, juce::String& error);
    juce::Result renderToWav(const Project& project, const juce::File& destination, double sampleRate);

private:
    struct RenderClip
    {
        std::int64_t startSample = 0;
        std::int64_t sourceOffsetSamples = 0;
        std::int64_t lengthSamples = 0;
        float leftGain = 1.0f;
        float rightGain = 1.0f;
        juce::AudioBuffer<float> samples;
    };

    struct RenderSnapshot
    {
        double sampleRate = 48000.0;
        std::int64_t lengthSamples = 0;
        std::int64_t loopStartSample = 0;
        std::int64_t loopEndSample = 0;
        double tempo = 120.0;
        bool loopEnabled = false;
        std::vector<RenderClip> clips;
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
                                                juce::String& error);
    std::optional<juce::AudioBuffer<float>> readAndResample(const juce::File& source,
                                                           double targetSampleRate,
                                                           juce::String& error);
    static void mixSample(const RenderSnapshot& snapshot,
                          std::int64_t timelineSample,
                          float& left,
                          float& right) noexcept;
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
    std::atomic<int> activeSnapshot { 0 };
    std::atomic<int> readingSnapshot { -1 };
    std::atomic<std::int64_t> playheadSample { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<bool> playing { false };
    std::atomic<bool> metronomeEnabled { true };
    std::atomic<bool> monitoringEnabled { false };
    std::atomic<int> monitoringFirstInput { 0 };
    std::atomic<int> monitoringChannels { 1 };
    std::atomic<float> outputLeftPeak { 0.0f };
    std::atomic<float> outputRightPeak { 0.0f };
    LockFreeRecorder recorder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StudioAudioEngine)
};
}
