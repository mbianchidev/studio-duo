#include "StudioAudioEngine.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace studio
{
namespace
{
juce::AudioFormatWriterOptions writerOptions(double sampleRate, int channels)
{
    return juce::AudioFormatWriterOptions {}
        .withSampleRate(sampleRate)
        .withNumChannels(channels)
        .withBitsPerSample(24);
}

struct RecordingCallbackScope
{
    ~RecordingCallbackScope()
    {
        if (counter != nullptr)
            counter->fetch_sub(1, std::memory_order_acq_rel);
    }

    std::atomic<int>* counter = nullptr;
};
}

StudioAudioEngine::LockFreeRecorder::LockFreeRecorder()
    : juce::Thread("Studio Duo recorder")
{
}

StudioAudioEngine::LockFreeRecorder::~LockFreeRecorder()
{
    stop();
}

juce::Result StudioAudioEngine::LockFreeRecorder::start(const juce::File& destination,
                                                        double newSampleRate,
                                                        int channels,
                                                        int firstInputChannel)
{
    if (isActive())
        return juce::Result::fail("A recording is already in progress.");

    destination.getParentDirectory().createDirectory();
    if (destination.existsAsFile() && !destination.deleteFile())
        return juce::Result::fail("Could not replace the existing recording.");

    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail("Could not open the recording file for writing.");

    juce::WavAudioFormat wav;
    writer = wav.createWriterFor(stream, writerOptions(newSampleRate, juce::jlimit(1, 2, channels)));
    if (writer == nullptr)
        return juce::Result::fail("Could not create the WAV recording writer.");

    if (ringBuffer == nullptr)
        ringBuffer = std::make_unique<juce::AudioBuffer<float>>(2, capacitySamples);
    if (recordingWaveform == nullptr)
        recordingWaveform = std::make_unique<RecordingWaveform>();

    outputFile = destination;
    recordingSampleRate = newSampleRate;
    recordingChannels = juce::jlimit(1, 2, channels);
    recordingFirstInputChannel = juce::jmax(0, firstInputChannel);
    fifo.reset();
    ringBuffer->clear();
    samplesWritten.store(0, std::memory_order_relaxed);
    samplesDropped.store(0, std::memory_order_relaxed);
    samplesCaptured.store(0, std::memory_order_relaxed);
    recordingWaveform->reset();
    accepting.store(true, std::memory_order_release);
    startThread();
    return juce::Result::ok();
}

StudioAudioEngine::RecordingResult StudioAudioEngine::LockFreeRecorder::stop()
{
    stopAccepting();
    return finishStop();
}

void StudioAudioEngine::LockFreeRecorder::stopAccepting() noexcept
{
    accepting.store(false, std::memory_order_release);
    signalThreadShouldExit();
}

StudioAudioEngine::RecordingResult StudioAudioEngine::LockFreeRecorder::finishStop()
{
    RecordingResult result;
    result.file = outputFile;

    if (!isThreadRunning() && writer == nullptr)
        return result;

    signalThreadShouldExit();
    if (!stopThread(5000))
    {
        result.result = juce::Result::fail("The recording writer did not stop cleanly.");
        return result;
    }

    if (writer != nullptr)
        writer->flush();
    writer.reset();

    result.durationSeconds = static_cast<double>(samplesWritten.load(std::memory_order_acquire))
        / recordingSampleRate;

    const auto dropped = samplesDropped.load(std::memory_order_acquire);
    if (dropped > 0)
        result.warning = "Dropped "
            + juce::String(dropped)
            + " samples because disk writing fell behind.";

    return result;
}

void StudioAudioEngine::LockFreeRecorder::push(const float* const* inputs,
                                               int inputChannels,
                                               int sourceSampleOffset,
                                               int samples) noexcept
{
    if (!accepting.load(std::memory_order_acquire) || samples <= 0)
        return;

    const auto writable = std::min(samples, fifo.getFreeSpace());
    if (writable < samples)
        samplesDropped.fetch_add(samples - writable, std::memory_order_relaxed);

    int start1 = 0;
    int size1 = 0;
    int start2 = 0;
    int size2 = 0;
    fifo.prepareToWrite(writable, start1, size1, start2, size2);

    const auto copyRegion = [&](int destinationStart, int count, int sourceStart)
    {
        for (int channel = 0; channel < recordingChannels; ++channel)
        {
            const auto sourceChannel = recordingFirstInputChannel + channel;
            const auto* source = sourceChannel < inputChannels ? inputs[sourceChannel] : nullptr;
            if (source != nullptr)
                ringBuffer->copyFrom(channel,
                                     destinationStart,
                                     source + sourceSampleOffset + sourceStart,
                                     count);
            else
                ringBuffer->clear(channel, destinationStart, count);
        }
    };

    copyRegion(start1, size1, 0);
    copyRegion(start2, size2, size1);
    fifo.finishedWrite(size1 + size2);
    samplesCaptured.fetch_add(size1 + size2, std::memory_order_relaxed);
    recordingWaveform->push(inputs,
                            inputChannels,
                            recordingFirstInputChannel,
                            recordingChannels,
                            writable,
                            sourceSampleOffset);
}

void StudioAudioEngine::LockFreeRecorder::noteDroppedSamples(int samples) noexcept
{
    if (samples > 0)
        samplesDropped.fetch_add(samples, std::memory_order_relaxed);
}

bool StudioAudioEngine::LockFreeRecorder::isActive() const noexcept
{
    return accepting.load(std::memory_order_acquire);
}

int StudioAudioEngine::LockFreeRecorder::availableSamples() const noexcept
{
    return fifo.getFreeSpace();
}

double StudioAudioEngine::LockFreeRecorder::capturedDurationSeconds() const noexcept
{
    return static_cast<double>(samplesCaptured.load(std::memory_order_relaxed))
        / recordingSampleRate;
}

std::vector<float> StudioAudioEngine::LockFreeRecorder::waveform() const
{
    return recordingWaveform != nullptr
        ? recordingWaveform->snapshot()
        : std::vector<float> {};
}

void StudioAudioEngine::LockFreeRecorder::run()
{
    while (!threadShouldExit() || fifo.getNumReady() > 0)
    {
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        fifo.prepareToRead(8192, start1, size1, start2, size2);

        if (size1 + size2 == 0)
        {
            wait(2);
            continue;
        }

        bool writeSucceeded = true;
        if (writer != nullptr && size1 > 0)
            writeSucceeded = writer->writeFromAudioSampleBuffer(*ringBuffer, start1, size1);
        if (writer != nullptr && size2 > 0)
            writeSucceeded = writer->writeFromAudioSampleBuffer(*ringBuffer, start2, size2)
                && writeSucceeded;

        if (!writeSucceeded)
            samplesDropped.fetch_add(size1 + size2, std::memory_order_relaxed);
        else
            samplesWritten.fetch_add(size1 + size2, std::memory_order_relaxed);

        fifo.finishedRead(size1 + size2);
    }
}

StudioAudioEngine::StudioAudioEngine()
{
    formatManager.registerBasicFormats();
    for (auto& snapshot : snapshots)
    {
        snapshot.sampleRate = sampleRate.load();
        snapshot.lengthSamples = static_cast<std::int64_t>(snapshot.sampleRate * 8.0);
        snapshot.loopEndSample = snapshot.lengthSamples;
    }
}

StudioAudioEngine::~StudioAudioEngine()
{
    shutdown();
}

juce::Result StudioAudioEngine::initialise(juce::AudioDeviceManager& manager)
{
    shutdown();
    shuttingDown.store(false, std::memory_order_release);

    const auto error = manager.initialiseWithDefaultDevices(1, 2);
    if (error.isNotEmpty())
        return juce::Result::fail("Audio device setup failed: " + error);

    deviceManager = &manager;
    deviceManager->addAudioCallback(this);
    return juce::Result::ok();
}

void StudioAudioEngine::shutdown()
{
    shuttingDown.store(true, std::memory_order_release);
    playing.store(false, std::memory_order_release);
    recordingAccepting.store(false, std::memory_order_release);
    calibrationActive.store(false, std::memory_order_release);
    calibrationResultReady.store(false, std::memory_order_release);

    if (deviceManager != nullptr)
    {
        deviceManager->removeAudioCallback(this);
        deviceManager = nullptr;
    }

    waitForRecordingCallbacks();
    recordingFinalizer.removeAllJobs(false, -1);
    if (activeRecorderCount.load(std::memory_order_acquire) > 0)
        finishRecordingSession();
    recordingFinalizing.store(false, std::memory_order_release);
    pluginRuntimeBuilder.removeAllJobs(false, -1);

    for (auto& graph : pluginRuntimeGraphs)
        graph.tracks.clear();
    activePluginRuntime.store(0, std::memory_order_release);
    readingPluginRuntime.store(-1, std::memory_order_release);
    activeSnapshot.store(0, std::memory_order_release);
    activeRenderPair.store(0, std::memory_order_release);
    readingSnapshot.store(-1, std::memory_order_release);
    for (auto& generation : snapshotGenerations)
        generation.store(0, std::memory_order_relaxed);
    for (auto& generation : pluginRuntimeGenerations)
        generation.store(0, std::memory_order_relaxed);
    pluginLateBlocks.store(0, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock(pluginRequestLock);
        pendingPluginRequests.clear();
        pendingPluginSnapshot.reset();
        pendingPluginFingerprint.clear();
        buildingPluginFingerprint.clear();
        desiredPluginFingerprint.clear();
        activePluginFingerprint.clear();
        pendingPluginGeneration = 0;
        pendingSnapshotGeneration = 0;
        desiredPluginGeneration = 0;
        activePluginGeneration = 0;
        hasPendingPluginRequest = false;
        pluginBuilderRunning = false;
    }
    {
        const juce::ScopedLock lock(pluginStatusLock);
        pluginStatuses.clear();
    }
}

juce::Result StudioAudioEngine::updateProject(
    const Project& project,
    std::vector<PluginRuntimeRequest> pluginRequests)
{
    metronomeEnabled.store(project.metronomeEnabled, std::memory_order_release);
    juce::String error;
    auto snapshot = buildSnapshot(project,
                                  currentSampleRate(),
                                  pluginRequests,
                                  error);
    if (!snapshot.has_value())
        return juce::Result::fail(error);

    requestPluginRuntime(std::move(pluginRequests), std::move(*snapshot));
    return juce::Result::ok();
}

void StudioAudioEngine::play() noexcept
{
    playing.store(true, std::memory_order_release);
}

void StudioAudioEngine::pause() noexcept
{
    playing.store(false, std::memory_order_release);
}

void StudioAudioEngine::stop() noexcept
{
    playing.store(false, std::memory_order_release);
    playheadSample.store(0, std::memory_order_release);
}

void StudioAudioEngine::seekSeconds(double seconds) noexcept
{
    const auto target = static_cast<std::int64_t>(std::max(0.0, seconds) * currentSampleRate());
    playheadSample.store(target, std::memory_order_release);
}

void StudioAudioEngine::setMetronomeEnabled(bool enabled) noexcept
{
    metronomeEnabled.store(enabled, std::memory_order_release);
}

void StudioAudioEngine::setInputMonitoring(bool enabled,
                                           int firstInputChannel,
                                           int channels) noexcept
{
    monitoringFirstInput.store(juce::jmax(0, firstInputChannel), std::memory_order_relaxed);
    monitoringChannels.store(juce::jlimit(1, 2, channels), std::memory_order_relaxed);
    monitoringEnabled.store(enabled, std::memory_order_release);
}

bool StudioAudioEngine::isPlaying() const noexcept
{
    return playing.load(std::memory_order_acquire);
}

bool StudioAudioEngine::isRecording() const noexcept
{
    return recordingAccepting.load(std::memory_order_acquire);
}

double StudioAudioEngine::positionSeconds() const noexcept
{
    return static_cast<double>(playheadSample.load(std::memory_order_acquire)) / currentSampleRate();
}

float StudioAudioEngine::leftPeak() const noexcept
{
    return outputLeftPeak.load(std::memory_order_relaxed);
}

float StudioAudioEngine::rightPeak() const noexcept
{
    return outputRightPeak.load(std::memory_order_relaxed);
}

double StudioAudioEngine::currentSampleRate() const noexcept
{
    return sampleRate.load(std::memory_order_acquire);
}

std::vector<StudioAudioEngine::RecordingProgress> StudioAudioEngine::recordingProgress() const
{
    const auto count = activeRecorderCount.load(std::memory_order_acquire);
    std::vector<RecordingProgress> progress;
    progress.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        const auto& recorder = recorders[static_cast<std::size_t>(index)];
        progress.push_back(recorder != nullptr
                               ? RecordingProgress {
                                   recorder->capturedDurationSeconds(),
                                   recorder->waveform()
                               }
                               : RecordingProgress {});
    }
    return progress;
}

std::vector<StudioAudioEngine::PluginRuntimeStatus> StudioAudioEngine::pluginRuntimeStatuses() const
{
    const juce::ScopedLock graphLock(pluginRequestLock);
    std::vector<PluginRuntimeStatus> statuses;
    {
        const juce::ScopedLock statusLock(pluginStatusLock);
        statuses = pluginStatuses;
    }

    const auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
        activePluginRuntime.load(std::memory_order_acquire))];
    for (auto& status : statuses)
    {
        if (status.state != PluginRuntimeStatus::State::ready)
            continue;

        for (const auto& track : graph.tracks)
        {
            const auto insert = std::find_if(track.inserts.cbegin(),
                                             track.inserts.cend(),
                                             [&status](const auto& candidate)
            {
                return candidate.insertId == status.insertId;
            });
            if (insert != track.inserts.cend()
                && (insert->bridge == nullptr || !insert->bridge->isReady()))
            {
                status.state = PluginRuntimeStatus::State::failed;
                status.message = "Sandbox worker disconnected";
                break;
            }
        }
    }
    return statuses;
}

std::uint64_t StudioAudioEngine::pluginLateBlockCount() const noexcept
{
    return pluginLateBlocks.load(std::memory_order_relaxed);
}

bool StudioAudioEngine::pluginRuntimeTransitionPending() const
{
    const juce::ScopedLock lock(pluginRequestLock);
    return pluginBuilderRunning
        || hasPendingPluginRequest
        || desiredPluginFingerprint != activePluginFingerprint;
}

void StudioAudioEngine::forcePluginRuntimeReload(
    const Project& project,
    std::vector<PluginRuntimeRequest> pluginRequests)
{
    {
        const juce::ScopedLock lock(pluginRequestLock);
        desiredPluginFingerprint.clear();
    }
    updateProject(project, std::move(pluginRequests));
}

juce::Result StudioAudioEngine::startRecording(const std::vector<RecordingRequest>& requests,
                                               const RecordingPlan& plan)
{
    if (recordingFinalizing.load(std::memory_order_acquire))
        return juce::Result::fail("The previous recording is still finalizing.");
    if (recordingAccepting.load(std::memory_order_acquire)
        || activeRecorderCount.load(std::memory_order_acquire) > 0)
        return juce::Result::fail("A recording is already in progress.");
    if (requests.empty())
        return juce::Result::fail("Arm at least one audio track before recording.");
    if (requests.size() > maximumRecordingTracks)
        return juce::Result::fail("Too many tracks are armed for one recording pass.");

    if (deviceManager == nullptr)
        return juce::Result::fail("No audio device is available.");

    const auto* device = deviceManager->getCurrentAudioDevice();
    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
        return juce::Result::fail("Enable at least one audio input before recording.");

    const auto activeInputs = device->getActiveInputChannels();
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        const auto& request = requests[index];
        const auto captureChannels = juce::jlimit(1, 2, request.channels);
        if (request.file.getFullPathName().isEmpty())
            return juce::Result::fail("Every armed track needs a recording destination.");
        if (request.firstInputChannel < 0)
            return juce::Result::fail("Recording input channel numbers cannot be negative.");
        for (int channel = 0; channel < captureChannels; ++channel)
        {
            if (!activeInputs[request.firstInputChannel + channel])
            {
                return juce::Result::fail(
                    "An armed track uses an input that is not enabled in audio I/O settings.");
            }
        }

        const auto duplicateDestination = std::any_of(
            requests.cbegin(),
            requests.cbegin() + static_cast<std::ptrdiff_t>(index),
            [&request](const auto& earlier)
            {
                return earlier.file == request.file;
            });
        if (duplicateDestination)
            return juce::Result::fail("Armed tracks must record to separate WAV files.");
    }

    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        auto& recorder = recorders[index];
        if (recorder == nullptr)
            recorder = std::make_unique<LockFreeRecorder>();

        const auto& request = requests[index];
        const auto result = recorder->start(request.file,
                                            currentSampleRate(),
                                            request.channels,
                                            request.firstInputChannel);
        if (result.failed())
        {
            for (std::size_t started = 0; started < index; ++started)
            {
                recorders[started]->stop();
                requests[started].file.deleteFile();
            }
            request.file.deleteFile();
            return juce::Result::fail(
                "Could not start recording track "
                + juce::String(static_cast<int>(index + 1))
                + ": "
                + result.getErrorMessage());
        }
    }

    if (plan.captureStartSeconds < 0.0
        || (plan.captureEndSeconds >= 0.0
            && plan.captureEndSeconds <= plan.captureStartSeconds)
        || (plan.transportEndSeconds >= 0.0
            && plan.transportEndSeconds < plan.captureEndSeconds))
    {
        return juce::Result::fail("The recording transport range is invalid.");
    }

    const auto recordingSampleRate = currentSampleRate();
    recordingCaptureStartSample.store(
        static_cast<std::int64_t>(std::llround(plan.captureStartSeconds
                                              * recordingSampleRate)),
        std::memory_order_relaxed);
    recordingCaptureEndSample.store(
        plan.captureEndSeconds >= 0.0
            ? static_cast<std::int64_t>(std::llround(plan.captureEndSeconds
                                                    * recordingSampleRate))
            : -1,
        std::memory_order_relaxed);
    recordingTransportEndSample.store(
        plan.transportEndSeconds >= 0.0
            ? static_cast<std::int64_t>(std::llround(plan.transportEndSeconds
                                                    * recordingSampleRate))
            : -1,
        std::memory_order_relaxed);
    recordingLoopEnabled.store(plan.loopEnabled, std::memory_order_relaxed);
    playheadSample.store(
        static_cast<std::int64_t>(std::llround(plan.transportStartSeconds
                                              * recordingSampleRate)),
        std::memory_order_release);
    activeRecorderCount.store(static_cast<int>(requests.size()), std::memory_order_release);
    recordingAccepting.store(true, std::memory_order_release);
    playing.store(true, std::memory_order_release);
    return juce::Result::ok();
}

std::vector<StudioAudioEngine::RecordingResult> StudioAudioEngine::stopRecording()
{
    recordingAccepting.store(false, std::memory_order_release);
    playing.store(false, std::memory_order_release);
    waitForRecordingCallbacks();
    playheadSample.store(0, std::memory_order_release);
    return finishRecordingSession();
}

void StudioAudioEngine::stopRecordingAsync(
    std::function<void(std::vector<RecordingResult>)> completion)
{
    if (recordingFinalizing.exchange(true, std::memory_order_acq_rel))
    {
        std::vector<RecordingResult> failure(1);
        failure.front().result = juce::Result::fail("The recording is already finalizing.");
        juce::MessageManager::callAsync(
            [finished = std::move(completion), results = std::move(failure)]() mutable
            {
                if (finished)
                    finished(std::move(results));
            });
        return;
    }

    recordingAccepting.store(false, std::memory_order_release);
    playing.store(false, std::memory_order_release);
    recordingFinalizer.addJob([this, callback = std::move(completion)]() mutable
    {
        waitForRecordingCallbacks();
        playheadSample.store(0, std::memory_order_release);
        auto recordingResults = finishRecordingSession();
        recordingFinalizing.store(false, std::memory_order_release);
        juce::MessageManager::callAsync(
            [finished = std::move(callback), completed = std::move(recordingResults)]() mutable
            {
                if (finished)
                    finished(std::move(completed));
            });
    });
}

void StudioAudioEngine::waitForRecordingCallbacks() const noexcept
{
    while (recordingCallbacksInFlight.load(std::memory_order_acquire) > 0)
        juce::Thread::sleep(1);
}

std::vector<StudioAudioEngine::RecordingResult> StudioAudioEngine::finishRecordingSession()
{
    const auto count = activeRecorderCount.exchange(0, std::memory_order_acq_rel);
    for (int index = 0; index < count; ++index)
        recorders[static_cast<std::size_t>(index)]->stopAccepting();

    std::vector<RecordingResult> results;
    results.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        results.push_back(recorders[static_cast<std::size_t>(index)]->finishStop());
    return results;
}

std::optional<double> StudioAudioEngine::audioFileDuration(const juce::File& source, juce::String& error)
{
    auto reader = formatManager.createReaderFor(source);
    if (reader == nullptr)
    {
        error = "Studio Duo could not read " + source.getFileName() + ".";
        return std::nullopt;
    }

    return static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
}

std::vector<double> StudioAudioEngine::analyseTransients(const AudioClip& clip,
                                                         juce::String& error)
{
    auto audio = readAndResample(clip.sourceFile, currentSampleRate(), error);
    if (!audio.has_value())
        return {};

    auto transients = AudioAnalysis::detectTransients(*audio,
                                                      currentSampleRate());
    transients.erase(
        std::remove_if(transients.begin(),
                       transients.end(),
                       [&clip](double transient)
                       {
                           return transient < clip.sourceRangeStartSeconds
                               || transient > clip.sourceRangeEnd();
                       }),
        transients.end());
    return transients;
}

juce::Result StudioAudioEngine::renderClipToWav(const AudioClip& clip,
                                                const juce::File& destination,
                                                double renderSampleRate)
{
    juce::String error;
    auto audio = readAndResample(clip.sourceFile, renderSampleRate, error);
    if (!audio.has_value())
        return juce::Result::fail(error);
    auto processed = processClipAudio(clip,
                                      *audio,
                                      renderSampleRate,
                                      error);
    if (!processed.has_value())
        return juce::Result::fail(error);
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create the consolidation directory.");
    if (destination.existsAsFile() && !destination.deleteFile())
        return juce::Result::fail("Could not replace the consolidated audio file.");

    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail("Could not open the consolidated audio file.");
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream,
                                      writerOptions(renderSampleRate, 2));
    if (writer == nullptr)
        return juce::Result::fail("Could not create the consolidated WAV writer.");

    RenderClip renderClip;
    renderClip.lengthSamples = static_cast<std::int64_t>(
        std::llround(clip.durationSeconds * renderSampleRate));
    renderClip.sampleRate = renderSampleRate;
    renderClip.gain = juce::Decibels::decibelsToGain(clip.gainDecibels);
    renderClip.processing = clip;
    renderClip.processing.sourceOffsetSeconds = 0.0;
    renderClip.processing.sourceLengthSeconds = clip.durationSeconds;
    renderClip.processing.sourceRangeStartSeconds = 0.0;
    renderClip.processing.sourceRangeEndSeconds = clip.durationSeconds;
    renderClip.processing.playbackRate = 1.0;
    renderClip.processing.reversed = false;
    renderClip.processing.warpMarkers.clear();
    renderClip.samples = std::move(*processed);

    juce::AudioBuffer<float> block(2, 2048);
    std::int64_t position = 0;
    while (position < renderClip.lengthSamples)
    {
        const auto samples = static_cast<int>(std::min<std::int64_t>(
            block.getNumSamples(),
            renderClip.lengthSamples - position));
        block.clear();
        for (int sample = 0; sample < samples; ++sample)
        {
            float left = 0.0f;
            float right = 0.0f;
            readRenderClipSample(renderClip,
                                 position + sample,
                                 left,
                                 right);
            block.setSample(0, sample, juce::jlimit(-1.0f, 1.0f, left));
            block.setSample(1, sample, juce::jlimit(-1.0f, 1.0f, right));
        }
        if (!writer->writeFromAudioSampleBuffer(block, 0, samples))
            return juce::Result::fail("Consolidation stopped while writing audio.");
        position += samples;
    }
    writer->flush();
    return juce::Result::ok();
}

juce::Result StudioAudioEngine::startLatencyCalibration(int outputChannel,
                                                        int inputChannel)
{
    if (recordingAccepting.load(std::memory_order_acquire)
        || recordingFinalizing.load(std::memory_order_acquire))
        return juce::Result::fail("Stop recording before calibrating hardware latency.");
    if (calibrationActive.load(std::memory_order_acquire))
        return juce::Result::fail("A hardware latency calibration is already running.");
    if (deviceManager == nullptr)
        return juce::Result::fail("No audio device is available.");
    if (outputChannel < 0 || inputChannel < 0)
        return juce::Result::fail("Calibration channel numbers cannot be negative.");

    auto* device = deviceManager->getCurrentAudioDevice();
    if (device == nullptr
        || !device->getActiveOutputChannels()[outputChannel]
        || !device->getActiveInputChannels()[inputChannel])
    {
        return juce::Result::fail(
            "Enable the selected reamp output and return input in audio I/O settings.");
    }

    playing.store(false, std::memory_order_release);
    calibrationOutputChannel.store(outputChannel, std::memory_order_relaxed);
    calibrationInputChannel.store(inputChannel, std::memory_order_relaxed);
    calibrationSamplesElapsed.store(0, std::memory_order_relaxed);
    calibrationLatencySamples.store(-1, std::memory_order_relaxed);
    calibrationResultReady.store(false, std::memory_order_release);
    calibrationActive.store(true, std::memory_order_release);
    return juce::Result::ok();
}

std::optional<StudioAudioEngine::LatencyCalibrationResult>
StudioAudioEngine::takeLatencyCalibrationResult()
{
    if (!calibrationResultReady.exchange(false, std::memory_order_acq_rel))
        return std::nullopt;

    LatencyCalibrationResult result;
    result.latencySamples = calibrationLatencySamples.load(
        std::memory_order_acquire);
    if (result.latencySamples < 0)
        result.result = juce::Result::fail(
            "No calibration return pulse was detected within two seconds.");
    return result;
}

juce::Result StudioAudioEngine::renderToWav(const Project& project,
                                            const juce::File& destination,
                                            double renderSampleRate)
{
    if (project.hasActivePluginInserts())
        return juce::Result::fail(
            "Fast export cannot omit active plugins. Bypass inserts or use playback capture until real-time bounce ships.");

    juce::String error;
    auto snapshot = buildSnapshot(project, renderSampleRate, {}, error);
    if (!snapshot.has_value())
        return juce::Result::fail(error);

    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create the export directory.");
    if (destination.existsAsFile() && !destination.deleteFile())
        return juce::Result::fail("Could not replace the existing export.");

    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail("Could not open the export file.");

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, writerOptions(renderSampleRate, 2));
    if (writer == nullptr)
        return juce::Result::fail("Could not create the WAV export writer.");

    juce::AudioBuffer<float> block(2, 2048);
    std::int64_t position = 0;
    while (position < snapshot->lengthSamples)
    {
        const auto samplesThisBlock = static_cast<int>(
            std::min<std::int64_t>(block.getNumSamples(), snapshot->lengthSamples - position));
        block.clear();

        for (int sample = 0; sample < samplesThisBlock; ++sample)
        {
            float left = 0.0f;
            float right = 0.0f;
            mixSample(*snapshot, position + sample, left, right);
            block.setSample(0, sample, juce::jlimit(-1.0f, 1.0f, left));
            block.setSample(1, sample, juce::jlimit(-1.0f, 1.0f, right));
        }

        if (!writer->writeFromAudioSampleBuffer(block, 0, samplesThisBlock))
            return juce::Result::fail("The WAV export stopped while writing audio.");

        position += samplesThisBlock;
    }

    writer->flush();
    return juce::Result::ok();
}

std::optional<StudioAudioEngine::RenderSnapshot> StudioAudioEngine::buildSnapshot(
    const Project& project,
    double targetSampleRate,
    const std::vector<PluginRuntimeRequest>& pluginRequests,
    juce::String& error)
{
    RenderSnapshot snapshot;
    snapshot.sampleRate = targetSampleRate;
    snapshot.processingQuantum = juce::jlimit(
        1,
        PluginBridgeSharedState::maxBlockSize,
        deviceBlockSize.load(std::memory_order_acquire));
    snapshot.contentLengthSamples = static_cast<std::int64_t>(
        std::ceil(project.lengthSeconds() * targetSampleRate));
    snapshot.lengthSamples = snapshot.contentLengthSamples;
    snapshot.loopStartSample = static_cast<std::int64_t>(project.loopStartSeconds * targetSampleRate);
    snapshot.loopEndSample = static_cast<std::int64_t>(project.loopEndSeconds * targetSampleRate);
    snapshot.tempo = project.tempo;
    snapshot.tempoChanges = project.tempoChanges;
    snapshot.meterChanges = project.meterChanges;
    snapshot.timeSignatureNumerator = project.timeSignatureNumerator;
    snapshot.timeSignatureDenominator = project.timeSignatureDenominator;
    snapshot.metronomeSubdivision = project.metronomeSubdivision;
    snapshot.metronomeOutputChannel = project.metronomeOutputChannel;
    snapshot.metronomeLevel = project.metronomeLevel;
    snapshot.metronomeAccentLevel = project.metronomeAccentLevel;
    snapshot.loopEnabled = project.loopEnabled && snapshot.loopEndSample > snapshot.loopStartSample;
    for (const auto& route : project.reampRoutes)
    {
        if (route.enabled && route.type == TonePathType::hardware)
        {
            snapshot.hardwareSends.push_back({
                runtimeKey(route.sourceTrackId),
                route.outputChannel
            });
        }
    }

    const auto anySolo = std::any_of(project.tracks.cbegin(), project.tracks.cend(), [](const auto& track)
    {
        return track.solo && track.type != TrackType::master;
    });

    if (const auto master = std::find_if(project.tracks.cbegin(), project.tracks.cend(), [](const auto& track)
        {
            return track.type == TrackType::master;
        }); master != project.tracks.cend())
    {
        snapshot.masterRuntimeKey = runtimeKey(master->id);
        snapshot.masterGain = juce::Decibels::decibelsToGain(master->volumeDecibels);
        snapshot.masterPan = master->pan;
        snapshot.masterAudible = !master->muted;
    }

    const auto buildClips = [this, targetSampleRate, &error](
                                const Track& track,
                                const std::vector<CompRegion>* compRegions)
        -> std::optional<std::vector<RenderClip>>
    {
        std::vector<RenderClip> clips;
        clips.reserve(track.clips.size());
        for (const auto& clip : track.clips)
        {
            if (clip.muted || !clip.sourceFile.existsAsFile())
                continue;

            auto audio = readAndResample(clip.sourceFile, targetSampleRate, error);
            if (!audio.has_value())
                return std::nullopt;
            auto processed = processClipAudio(clip,
                                              *audio,
                                              targetSampleRate,
                                              error);
            if (!processed.has_value())
                return std::nullopt;
            auto preparedClip = clip;
            preparedClip.sourceOffsetSeconds = 0.0;
            preparedClip.sourceLengthSeconds = clip.durationSeconds;
            preparedClip.sourceRangeStartSeconds = 0.0;
            preparedClip.sourceRangeEndSeconds = clip.durationSeconds;
            preparedClip.playbackRate = 1.0;
            preparedClip.reversed = false;
            preparedClip.warpMarkers.clear();

            const auto addRange = [&](double rangeStart, double rangeEnd)
            {
                const auto intersectionStart = std::max(clip.startSeconds, rangeStart);
                const auto intersectionEnd = std::min(clip.endSeconds(), rangeEnd);
                if (intersectionEnd <= intersectionStart)
                    return;

                RenderClip renderClip;
                renderClip.startSample = static_cast<std::int64_t>(
                    std::llround(intersectionStart * targetSampleRate));
                renderClip.lengthSamples = static_cast<std::int64_t>(
                    std::llround((intersectionEnd - intersectionStart)
                                 * targetSampleRate));
                renderClip.timelineOffsetBaseSeconds = intersectionStart
                    - clip.startSeconds;
                renderClip.sampleRate = targetSampleRate;
                renderClip.gain = juce::Decibels::decibelsToGain(clip.gainDecibels);
                renderClip.processing = preparedClip;
                renderClip.samples = *processed;
                if (renderClip.lengthSamples > 0)
                    clips.push_back(std::move(renderClip));
            };

            if (compRegions == nullptr)
            {
                addRange(clip.startSeconds, clip.endSeconds());
            }
            else
            {
                for (const auto& region : *compRegions)
                    if (region.sourceTrackId == track.id)
                        addRange(region.startSeconds, region.endSeconds());
            }
        }

        return clips;
    };

    for (const auto& parent : project.tracks)
    {
        if (parent.type == TrackType::master || parent.parentTrackId.isNotEmpty())
            continue;

        RenderTrack renderTrack;
        renderTrack.runtimeKey = runtimeKey(parent.id);
        renderTrack.volumeGain = juce::Decibels::decibelsToGain(parent.volumeDecibels);
        renderTrack.pan = parent.pan;
        renderTrack.audible = !parent.muted;

        auto parentClips = buildClips(parent, nullptr);
        if (!parentClips.has_value())
            return std::nullopt;

        RenderSource parentSource;
        parentSource.isParentContent = true;
        parentSource.audible = !parent.muted && (!anySolo || parent.solo);
        parentSource.clips = std::move(*parentClips);
        renderTrack.sources.push_back(std::move(parentSource));

        const auto activeTakeId = project.activeTakeTrackId(parent.id);
        for (const auto& child : project.tracks)
        {
            if (child.parentTrackId != parent.id)
                continue;

            const auto selectedByComp = std::any_of(
                parent.compRegions.cbegin(),
                parent.compRegions.cend(),
                [&child](const auto& region)
                {
                    return region.sourceTrackId == child.id;
                });
            const auto selectedByPlaylist = parent.compRegions.empty()
                && child.id == activeTakeId;
            const auto selectedBySolo = child.solo;
            if (!selectedByComp && !selectedByPlaylist && !selectedBySolo)
                continue;

            auto childClips = buildClips(
                child,
                selectedByComp && !selectedBySolo ? &parent.compRegions : nullptr);
            if (!childClips.has_value())
                return std::nullopt;

            RenderSource source;
            source.runtimeKey = runtimeKey(child.id);
            source.volumeGain = juce::Decibels::decibelsToGain(child.volumeDecibels);
            source.pan = child.pan;
            source.audible = !parent.muted
                && !child.muted
                && (!anySolo || parent.solo || child.solo);
            source.clips = std::move(*childClips);
            renderTrack.sources.push_back(std::move(source));
        }

        if (renderTrack.sources.size() == 1 && anySolo && !parent.solo)
            renderTrack.audible = false;
        else if (renderTrack.sources.size() > 1
                 && anySolo
                 && !parent.solo
                 && std::none_of(project.tracks.cbegin(),
                                 project.tracks.cend(),
                                 [&parent](const auto& child)
        {
            return child.parentTrackId == parent.id && child.solo;
        }))
            renderTrack.audible = false;

        snapshot.tracks.push_back(std::move(renderTrack));
    }

    configureRuntimeTiming(snapshot, pluginRequests);

    return snapshot;
}

void StudioAudioEngine::configureRuntimeTiming(
    RenderSnapshot& snapshot,
    const std::vector<PluginRuntimeRequest>& pluginRequests) const
{
    const auto latencyForKey = [&pluginRequests, &snapshot](std::uint64_t key)
    {
        return std::accumulate(pluginRequests.cbegin(),
                               pluginRequests.cend(),
                               0,
                               [key, &snapshot](int total, const auto& request)
        {
            if (runtimeKey(request.trackId) != key
                || request.bypassed
                || request.missing
                || !request.description.has_value())
                return total;

            return total
                + snapshot.processingQuantum
                + std::max(0, request.latencySamples);
        });
    };
    const auto tailForKey = [&pluginRequests](std::uint64_t key)
    {
        return std::accumulate(pluginRequests.cbegin(),
                               pluginRequests.cend(),
                               0.0,
                               [key](double total, const auto& request)
        {
            if (runtimeKey(request.trackId) != key
                || request.bypassed
                || request.missing
                || !request.description.has_value())
                return total;
            return total + std::max(0.0, request.tailSeconds);
        });
    };
    const auto resetDelay = [&snapshot](auto& delay, int samples)
    {
        delay.delaySamples = std::max(0, samples);
        delay.writePosition = 0;
        delay.buffer.setSize(2,
                             std::max(1,
                                      delay.delaySamples
                                          + snapshot.processingQuantum
                                          + 1),
                             false,
                             true,
                             false);
        delay.buffer.clear();
    };

    auto maximumTrackLatency = 0;
    auto maximumTrackTailSeconds = 0.0;
    for (auto& track : snapshot.tracks)
    {
        auto maximumSourceLatency = 0;
        auto maximumSourceTail = 0.0;
        for (auto& source : track.sources)
        {
            source.runtimeLatencySamples = source.isParentContent
                ? 0
                : latencyForKey(source.runtimeKey);
            maximumSourceLatency = std::max(maximumSourceLatency,
                                            source.runtimeLatencySamples);
            maximumSourceTail = std::max(maximumSourceTail,
                                         source.isParentContent
                                             ? 0.0
                                             : tailForKey(source.runtimeKey));
        }

        for (auto& source : track.sources)
            resetDelay(source.compensation,
                       maximumSourceLatency - source.runtimeLatencySamples);

        track.runtimeLatencySamples = maximumSourceLatency
            + latencyForKey(track.runtimeKey);
        maximumTrackLatency = std::max(maximumTrackLatency,
                                       track.runtimeLatencySamples);
        maximumTrackTailSeconds = std::max(maximumTrackTailSeconds,
                                           maximumSourceTail + tailForKey(track.runtimeKey));
    }

    for (auto& track : snapshot.tracks)
        resetDelay(track.compensation,
                   maximumTrackLatency - track.runtimeLatencySamples);

    const auto masterLatency = latencyForKey(snapshot.masterRuntimeKey);
    resetDelay(snapshot.clickCompensation,
               maximumTrackLatency + masterLatency);
    snapshot.lengthSamples = snapshot.contentLengthSamples
        + maximumTrackLatency
        + masterLatency
        + static_cast<std::int64_t>(
            std::ceil((maximumTrackTailSeconds + tailForKey(snapshot.masterRuntimeKey))
                      * snapshot.sampleRate));
}

std::optional<juce::AudioBuffer<float>> StudioAudioEngine::readAndResample(
    const juce::File& source,
    double targetSampleRate,
    juce::String& error)
{
    auto reader = formatManager.createReaderFor(source);
    if (reader == nullptr)
    {
        error = "Could not decode " + source.getFileName() + ".";
        return std::nullopt;
    }

    if (reader->lengthInSamples <= 0
        || reader->lengthInSamples > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
    {
        error = "Audio file is empty or too large for the current vertical slice: " + source.getFileName();
        return std::nullopt;
    }

    const auto sourceChannels = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
    const auto sourceLength = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> sourceBuffer(sourceChannels, sourceLength);
    if (!reader->read(&sourceBuffer, 0, sourceLength, 0, true, sourceChannels > 1))
    {
        error = "Could not read audio samples from " + source.getFileName() + ".";
        return std::nullopt;
    }

    const auto targetLength = juce::jmax(
        1,
        static_cast<int>(std::llround(static_cast<double>(sourceLength)
                                      * targetSampleRate
                                      / reader->sampleRate)));
    juce::AudioBuffer<float> targetBuffer(sourceChannels, targetLength);
    const auto sourcePerTarget = reader->sampleRate / targetSampleRate;

    for (int channel = 0; channel < sourceChannels; ++channel)
    {
        const auto* sourceSamples = sourceBuffer.getReadPointer(channel);
        auto* targetSamples = targetBuffer.getWritePointer(channel);

        for (int sample = 0; sample < targetLength; ++sample)
        {
            const auto sourcePosition = static_cast<double>(sample) * sourcePerTarget;
            const auto first = juce::jlimit(0, sourceLength - 1, static_cast<int>(sourcePosition));
            const auto second = juce::jmin(sourceLength - 1, first + 1);
            const auto fraction = static_cast<float>(sourcePosition - static_cast<double>(first));
            targetSamples[sample] = sourceSamples[first]
                + (sourceSamples[second] - sourceSamples[first]) * fraction;
        }
    }

    return targetBuffer;
}

std::optional<juce::AudioBuffer<float>> StudioAudioEngine::processClipAudio(
    const AudioClip& clip,
    const juce::AudioBuffer<float>& source,
    double targetSampleRate,
    juce::String& error) const
{
    if (targetSampleRate <= 0.0
        || source.getNumChannels() <= 0
        || source.getNumSamples() <= 0
        || clip.durationSeconds <= 0.0)
    {
        error = "The clip cannot be prepared for elastic playback.";
        return std::nullopt;
    }

    const auto channels = juce::jlimit(1, 2, source.getNumChannels());
    const auto outputLength = std::max(
        1,
        static_cast<int>(std::llround(clip.durationSeconds
                                      * targetSampleRate)));
    juce::AudioBuffer<float> output(channels, outputLength);
    output.clear();

    std::vector<std::pair<double, double>> points;
    points.emplace_back(0.0, clip.sourceSecondsAt(0.0));
    for (const auto& marker : clip.warpMarkers)
    {
        if (marker.timelineOffsetSeconds > 0.0
            && marker.timelineOffsetSeconds < clip.durationSeconds)
            points.emplace_back(marker.timelineOffsetSeconds,
                                clip.sourceSecondsAt(
                                    marker.timelineOffsetSeconds));
    }
    points.emplace_back(clip.durationSeconds,
                        clip.sourceSecondsAt(clip.durationSeconds));
    std::stable_sort(points.begin(),
                     points.end(),
                     [](const auto& left, const auto& right)
                     {
                         return left.first < right.first;
                     });

    for (std::size_t point = 0; point + 1 < points.size(); ++point)
    {
        const auto timelineStart = points[point].first;
        const auto timelineEnd = points[point + 1].first;
        const auto sourceStart = points[point].second;
        const auto sourceEnd = points[point + 1].second;
        const auto segmentOutputStart = static_cast<int>(
            std::llround(timelineStart * targetSampleRate));
        const auto segmentOutputEnd = std::min(
            outputLength,
            static_cast<int>(std::llround(timelineEnd * targetSampleRate)));
        const auto segmentOutputLength = segmentOutputEnd - segmentOutputStart;
        const auto segmentInputLength = std::max(
            1,
            static_cast<int>(std::llround(std::abs(sourceEnd - sourceStart)
                                          * targetSampleRate)));
        if (segmentOutputLength <= 0)
            continue;

        juce::AudioBuffer<float> input(channels, segmentInputLength);
        for (int channel = 0; channel < channels; ++channel)
        {
            for (int sample = 0; sample < segmentInputLength; ++sample)
            {
                const auto progress = segmentInputLength > 1
                    ? static_cast<double>(sample)
                        / static_cast<double>(segmentInputLength - 1)
                    : 0.0;
                const auto sourceSeconds = sourceStart
                    + (sourceEnd - sourceStart) * progress;
                const auto sourceSample = juce::jlimit(
                    0.0,
                    static_cast<double>(source.getNumSamples() - 1),
                    sourceSeconds * targetSampleRate);
                const auto first = static_cast<int>(std::floor(sourceSample));
                const auto second = std::min(first + 1,
                                             source.getNumSamples() - 1);
                const auto fraction = static_cast<float>(
                    sourceSample - static_cast<double>(first));
                const auto firstValue = source.getSample(channel, first);
                const auto secondValue = source.getSample(channel, second);
                input.setSample(channel,
                                sample,
                                firstValue + (secondValue - firstValue) * fraction);
            }
        }

        if (std::abs(static_cast<double>(segmentInputLength)
                     / static_cast<double>(segmentOutputLength)
                     - 1.0)
            < 0.0001)
        {
            for (int channel = 0; channel < channels; ++channel)
                output.copyFrom(channel,
                                segmentOutputStart,
                                input,
                                channel,
                                0,
                                std::min(segmentInputLength,
                                         segmentOutputLength));
            continue;
        }

        signalsmith::stretch::SignalsmithStretch<float> stretch;
        if (clip.stretchMode == StretchMode::drums)
            stretch.presetCheaper(channels, targetSampleRate);
        else
            stretch.presetDefault(channels, targetSampleRate);
        const auto inputLatency = stretch.inputLatency();
        const auto outputLatency = stretch.outputLatency();
        juce::AudioBuffer<float> paddedInput(channels,
                                             segmentInputLength
                                                 + inputLatency);
        paddedInput.clear();
        for (int channel = 0; channel < channels; ++channel)
            paddedInput.copyFrom(channel,
                                 0,
                                 input,
                                 channel,
                                 0,
                                 segmentInputLength);
        juce::AudioBuffer<float> paddedOutput(channels,
                                              segmentOutputLength
                                                  + outputLatency);
        paddedOutput.clear();

        std::array<float*, 2> inputPointers {};
        std::array<float*, 2> outputPointers {};
        for (int channel = 0; channel < channels; ++channel)
        {
            inputPointers[static_cast<std::size_t>(channel)]
                = paddedInput.getWritePointer(channel);
            outputPointers[static_cast<std::size_t>(channel)]
                = paddedOutput.getWritePointer(channel);
        }
        stretch.seek(inputPointers.data(),
                     inputLatency,
                     static_cast<double>(segmentInputLength)
                         / static_cast<double>(segmentOutputLength));
        for (int channel = 0; channel < channels; ++channel)
            inputPointers[static_cast<std::size_t>(channel)] += inputLatency;
        stretch.process(inputPointers.data(),
                        segmentInputLength,
                        outputPointers.data(),
                        segmentOutputLength);
        for (int channel = 0; channel < channels; ++channel)
            outputPointers[static_cast<std::size_t>(channel)]
                += segmentOutputLength;
        stretch.flush(outputPointers.data(), outputLatency);

        for (int channel = 0; channel < channels; ++channel)
        {
            for (int sample = 0;
                 sample < outputLatency
                     && outputLatency + sample < paddedOutput.getNumSamples();
                 ++sample)
            {
                const auto trimmed = paddedOutput.getSample(
                    channel,
                    outputLatency - 1 - sample);
                paddedOutput.addSample(channel,
                                       outputLatency + sample,
                                       -trimmed);
            }
            output.copyFrom(channel,
                            segmentOutputStart,
                            paddedOutput,
                            channel,
                            outputLatency,
                            segmentOutputLength);
        }
    }
    return output;
}

void StudioAudioEngine::mixSample(const RenderSnapshot& snapshot,
                                  std::int64_t timelineSample,
                                  float& left,
                                  float& right) noexcept
{
    if (!snapshot.masterAudible)
        return;

    for (const auto& track : snapshot.tracks)
    {
        if (!track.audible)
            continue;

        float trackLeft = 0.0f;
        float trackRight = 0.0f;
        for (const auto& source : track.sources)
        {
            if (!source.audible)
                continue;

            float sourceLeft = 0.0f;
            float sourceRight = 0.0f;
            for (const auto& clip : source.clips)
            {
                const auto relative = timelineSample - clip.startSample;
                if (relative < 0 || relative >= clip.lengthSamples)
                    continue;

                float clipLeft = 0.0f;
                float clipRight = 0.0f;
                if (readRenderClipSample(clip, relative, clipLeft, clipRight))
                {
                    sourceLeft += clipLeft;
                    sourceRight += clipRight;
                }
            }

            const auto leftPanGain = source.pan > 0.0f ? 1.0f - source.pan : 1.0f;
            const auto rightPanGain = source.pan < 0.0f ? 1.0f + source.pan : 1.0f;
            trackLeft += sourceLeft * source.volumeGain * leftPanGain;
            trackRight += sourceRight * source.volumeGain * rightPanGain;
        }

        const auto leftPanGain = track.pan > 0.0f ? 1.0f - track.pan : 1.0f;
        const auto rightPanGain = track.pan < 0.0f ? 1.0f + track.pan : 1.0f;
        left += trackLeft * track.volumeGain * leftPanGain;
        right += trackRight * track.volumeGain * rightPanGain;
    }

    const auto masterLeftGain = snapshot.masterGain
        * (snapshot.masterPan > 0.0f ? 1.0f - snapshot.masterPan : 1.0f);
    const auto masterRightGain = snapshot.masterGain
        * (snapshot.masterPan < 0.0f ? 1.0f + snapshot.masterPan : 1.0f);
    left *= masterLeftGain;
    right *= masterRightGain;
}

void StudioAudioEngine::renderSourceBlock(RenderSource& source,
                                          std::int64_t timelineSample,
                                          int samples,
                                          bool loopEnabled,
                                          std::int64_t loopStartSample,
                                          std::int64_t loopEndSample) noexcept
{
    source.processingBuffer.clear(0, 0, samples);
    source.processingBuffer.clear(1, 0, samples);
    if (!source.audible)
        return;

    for (int sample = 0; sample < samples; ++sample)
    {
        auto position = timelineSample + sample;
        if (loopEnabled && position >= loopEndSample)
        {
            const auto loopLength = loopEndSample - loopStartSample;
            if (loopLength > 0)
                position = loopStartSample + (position - loopStartSample) % loopLength;
        }
        float left = 0.0f;
        float right = 0.0f;
        for (const auto& clip : source.clips)
        {
            const auto relative = position - clip.startSample;
            if (relative < 0 || relative >= clip.lengthSamples)
                continue;

            float clipLeft = 0.0f;
            float clipRight = 0.0f;
            if (readRenderClipSample(clip, relative, clipLeft, clipRight))
            {
                left += clipLeft;
                right += clipRight;
            }
        }

        source.processingBuffer.setSample(0, sample, left);
        source.processingBuffer.setSample(1, sample, right);
    }

    applyTrackGainAndPan(source.processingBuffer,
                         samples,
                         source.volumeGain,
                         source.pan);
}

bool StudioAudioEngine::readRenderClipSample(const RenderClip& clip,
                                             std::int64_t relativeSample,
                                             float& left,
                                             float& right) noexcept
{
    const auto timelineOffsetSeconds = clip.timelineOffsetBaseSeconds
        + static_cast<double>(relativeSample) / clip.sampleRate;
    const auto sourceSample = clip.processing.sourceSecondsAt(
        timelineOffsetSeconds)
        * clip.sampleRate;
    if (sourceSample < 0.0
        || sourceSample >= static_cast<double>(clip.samples.getNumSamples()))
        return false;

    const auto firstIndex = static_cast<int>(std::floor(sourceSample));
    const auto secondIndex = std::min(firstIndex + 1,
                                      clip.samples.getNumSamples() - 1);
    const auto fraction = static_cast<float>(
        sourceSample - static_cast<double>(firstIndex));
    const auto preserveAttack = clip.processing.stretchMode == StretchMode::drums;
    const auto sampleChannel = [&clip, firstIndex, secondIndex, fraction](int channel)
    {
        const auto first = clip.samples.getSample(channel, firstIndex);
        const auto second = clip.samples.getSample(channel, secondIndex);
        return first + (second - first) * fraction;
    };
    const auto nearestIndex = fraction < 0.5f ? firstIndex : secondIndex;
    left = preserveAttack
        ? clip.samples.getSample(0, nearestIndex)
        : sampleChannel(0);
    right = clip.samples.getNumChannels() > 1
        ? preserveAttack
            ? clip.samples.getSample(1, nearestIndex)
            : sampleChannel(1)
        : left;
    auto gain = clip.gain
        * clip.processing.envelopeGainAt(timelineOffsetSeconds);
    if (clip.processing.polarityInverted)
        gain = -gain;
    left *= gain;
    right *= gain;
    return true;
}

void StudioAudioEngine::applyTrackGainAndPan(juce::AudioBuffer<float>& buffer,
                                             int samples,
                                             float gain,
                                             float pan) noexcept
{
    const auto leftGain = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
    const auto rightGain = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
    buffer.applyGain(0, 0, samples, leftGain);
    buffer.applyGain(1, 0, samples, rightGain);
}

void StudioAudioEngine::applyDelayCompensation(
    juce::AudioBuffer<float>& buffer,
    int samples,
    RenderSource::DelayCompensator& delay) noexcept
{
    if (delay.delaySamples <= 0 || delay.buffer.getNumSamples() <= delay.delaySamples)
        return;

    const auto capacity = delay.buffer.getNumSamples();
    for (int sample = 0; sample < samples; ++sample)
    {
        auto readPosition = delay.writePosition - delay.delaySamples;
        if (readPosition < 0)
            readPosition += capacity;

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto input = buffer.getSample(channel, sample);
            const auto output = delay.buffer.getSample(channel, readPosition);
            delay.buffer.setSample(channel, delay.writePosition, input);
            buffer.setSample(channel, sample, output);
        }

        delay.writePosition = (delay.writePosition + 1) % capacity;
    }
}

void StudioAudioEngine::processRuntimeChain(std::uint64_t key,
                                            juce::AudioBuffer<float>& buffer) noexcept
{
    if (key == 0)
        return;

    const auto readingIndex = readingPluginRuntime.load(std::memory_order_acquire);
    const auto runtimeIndex = readingIndex >= 0
        ? readingIndex
        : activePluginRuntime.load(std::memory_order_acquire);
    auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(runtimeIndex)];

    const auto track = std::find_if(graph.tracks.begin(), graph.tracks.end(), [key](const auto& candidate)
    {
        return candidate.key == key;
    });
    if (track != graph.tracks.end())
    {
        for (auto& insert : track->inserts)
        {
            if (insert.bridge != nullptr && insert.bridge->isReady())
            {
                const auto before = insert.bridge->lateBlockCount();
                insert.bridge->processBlock(buffer);
                const auto after = insert.bridge->lateBlockCount();
                if (after > before)
                    pluginLateBlocks.fetch_add(after - before, std::memory_order_relaxed);
            }
            else
            {
                applyDelayCompensation(buffer,
                                       buffer.getNumSamples(),
                                       insert.failureDelay);
            }
        }
    }

}

double StudioAudioEngine::tempoAt(const RenderSnapshot& snapshot,
                                  double seconds) noexcept
{
    if (snapshot.tempoChanges.empty())
        return snapshot.tempo;

    const auto position = std::max(0.0, seconds);
    if (position < snapshot.tempoChanges.front().timeSeconds)
        return snapshot.tempo;
    auto index = std::size_t { 0 };
    while (index + 1 < snapshot.tempoChanges.size()
           && snapshot.tempoChanges[index + 1].timeSeconds <= position)
        ++index;

    const auto& current = snapshot.tempoChanges[index];
    if (!current.rampToNext || index + 1 >= snapshot.tempoChanges.size())
        return current.bpm;

    const auto& next = snapshot.tempoChanges[index + 1];
    const auto duration = next.timeSeconds - current.timeSeconds;
    if (duration <= 0.0)
        return next.bpm;
    const auto progress = juce::jlimit(0.0,
                                       1.0,
                                       (position - current.timeSeconds) / duration);
    return current.bpm + (next.bpm - current.bpm) * progress;
}

double StudioAudioEngine::beatsAt(const RenderSnapshot& snapshot,
                                  double seconds) noexcept
{
    const auto target = std::max(0.0, seconds);
    if (snapshot.tempoChanges.empty())
        return target * snapshot.tempo / 60.0;

    auto beats = 0.0;
    if (snapshot.tempoChanges.front().timeSeconds > 0.0)
    {
        const auto initialEnd = std::min(target,
                                         snapshot.tempoChanges.front().timeSeconds);
        beats += initialEnd * snapshot.tempo / 60.0;
        if (target <= snapshot.tempoChanges.front().timeSeconds)
            return beats;
    }

    for (std::size_t index = 0; index < snapshot.tempoChanges.size(); ++index)
    {
        const auto& current = snapshot.tempoChanges[index];
        if (target <= current.timeSeconds)
            return beats;
        const auto segmentEnd = index + 1 < snapshot.tempoChanges.size()
            ? snapshot.tempoChanges[index + 1].timeSeconds
            : target;
        const auto end = std::min(target, segmentEnd);
        if (end <= current.timeSeconds)
            continue;

        const auto elapsed = end - current.timeSeconds;
        if (current.rampToNext && index + 1 < snapshot.tempoChanges.size())
        {
            const auto duration = snapshot.tempoChanges[index + 1].timeSeconds
                - current.timeSeconds;
            const auto slope = duration > 0.0
                ? (snapshot.tempoChanges[index + 1].bpm - current.bpm) / duration
                : 0.0;
            beats += (current.bpm * elapsed + 0.5 * slope * elapsed * elapsed) / 60.0;
        }
        else
        {
            beats += current.bpm * elapsed / 60.0;
        }

        if (target <= segmentEnd)
            return beats;
    }
    return beats;
}

MeterChange StudioAudioEngine::meterAt(const RenderSnapshot& snapshot,
                                       double seconds) noexcept
{
    auto current = MeterChange { 0.0,
                                 snapshot.timeSignatureNumerator,
                                 snapshot.timeSignatureDenominator };
    for (const auto& change : snapshot.meterChanges)
    {
        if (change.timeSeconds > seconds)
            break;
        current = change;
    }
    return current;
}

void StudioAudioEngine::addMetronome(const RenderSnapshot& snapshot,
                                     std::int64_t timelineSample,
                                     float& left,
                                     float& right) noexcept
{
    const auto seconds = static_cast<double>(timelineSample) / snapshot.sampleRate;
    const auto meter = meterAt(snapshot, seconds);
    const auto quarterBeats = beatsAt(snapshot, seconds);
    const auto meterStartQuarterBeats = beatsAt(snapshot, meter.timeSeconds);
    const auto metricBeats = (quarterBeats - meterStartQuarterBeats)
        * static_cast<double>(meter.denominator)
        / 4.0;
    const auto subdivision = std::max(1, snapshot.metronomeSubdivision);
    const auto clickPosition = metricBeats * static_cast<double>(subdivision);
    const auto clickIndex = static_cast<std::int64_t>(std::floor(clickPosition + 0.0000001));
    const auto clickProgress = clickPosition - static_cast<double>(clickIndex);
    const auto secondsPerMetricBeat = 60.0
        / tempoAt(snapshot, seconds)
        * 4.0
        / static_cast<double>(meter.denominator);
    const auto clickPositionSeconds = clickProgress
        * secondsPerMetricBeat
        / static_cast<double>(subdivision);
    constexpr auto clickDurationSeconds = 0.018;
    if (clickPositionSeconds < 0.0 || clickPositionSeconds >= clickDurationSeconds)
        return;

    const auto clicksPerBar = std::max(1, meter.numerator * subdivision);
    const auto accent = clickIndex % clicksPerBar == 0;
    const auto frequency = accent ? 1760.0 : 1320.0;
    const auto envelope = static_cast<float>(
        1.0 - clickPositionSeconds / clickDurationSeconds);
    const auto level = accent
        ? snapshot.metronomeAccentLevel
        : snapshot.metronomeLevel;
    const auto click = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi
                                                   * frequency
                                                   * clickPositionSeconds))
        * envelope
        * 0.18f
        * level;
    left += click;
    right += click;
}

int StudioAudioEngine::chooseWritableSnapshot() const noexcept
{
    const auto active = activeSnapshot.load(std::memory_order_acquire);
    const auto reading = readingSnapshot.load(std::memory_order_acquire);

    for (int index = 0; index < static_cast<int>(snapshots.size()); ++index)
        if (index != active && index != reading)
            return index;

    return (active + 1) % static_cast<int>(snapshots.size());
}

void StudioAudioEngine::requestPluginRuntime(std::vector<PluginRuntimeRequest> requests,
                                             RenderSnapshot snapshot)
{
    const auto fingerprint = pluginRuntimeFingerprint(requests);
    auto startBuilder = false;
    {
        const juce::ScopedLock lock(pluginRequestLock);
        if (fingerprint == activePluginFingerprint
            && fingerprint == desiredPluginFingerprint
            && !pluginBuilderRunning
            && !hasPendingPluginRequest)
        {
            const auto destination = chooseWritableSnapshot();
            snapshots[static_cast<std::size_t>(destination)] = std::move(snapshot);
            snapshotGenerations[static_cast<std::size_t>(destination)].store(
                activePluginGeneration,
                std::memory_order_release);
            activeSnapshot.store(destination, std::memory_order_release);
            activeRenderPair.store(renderPair(activePluginGeneration,
                                              destination,
                                              activePluginRuntime.load(std::memory_order_acquire)),
                                   std::memory_order_release);
            return;
        }

        if (fingerprint != desiredPluginFingerprint
            || (!pluginBuilderRunning
                && !hasPendingPluginRequest
                && fingerprint != activePluginFingerprint))
        {
            desiredPluginFingerprint = fingerprint;
            ++desiredPluginGeneration;
            pendingPluginRequests = std::move(requests);
            pendingPluginFingerprint = fingerprint;
            pendingPluginGeneration = desiredPluginGeneration;
            hasPendingPluginRequest = true;
        }

        pendingPluginSnapshot = std::move(snapshot);
        pendingSnapshotGeneration = desiredPluginGeneration;
        if (!pluginBuilderRunning)
        {
            pluginBuilderRunning = true;
            startBuilder = true;
        }
    }

    if (startBuilder)
        pluginRuntimeBuilder.addJob([this] { runPluginRuntimeBuilder(); });
}

void StudioAudioEngine::runPluginRuntimeBuilder()
{
    for (;;)
    {
        std::vector<PluginRuntimeRequest> requests;
        juce::String fingerprint;
        std::uint64_t generation = 0;
        {
            const juce::ScopedLock lock(pluginRequestLock);
            if (!hasPendingPluginRequest)
            {
                pluginBuilderRunning = false;
                return;
            }

            requests = std::move(pendingPluginRequests);
            fingerprint = pendingPluginFingerprint;
            generation = pendingPluginGeneration;
            pendingPluginFingerprint.clear();
            hasPendingPluginRequest = false;
            buildingPluginFingerprint = fingerprint;
        }

        std::vector<PluginRuntimeStatus> statuses;
        statuses.reserve(requests.size());
        PluginRuntimeGraph graph;

        for (auto& request : requests)
        {
            if (shuttingDown.load(std::memory_order_acquire))
                return;

            PluginRuntimeStatus status;
            status.trackId = request.trackId;
            status.insertId = request.insertId;
            status.name = request.name;

            if (request.bypassed)
            {
                status.state = PluginRuntimeStatus::State::bypassed;
                status.message = "Bypassed";
                statuses.push_back(std::move(status));
                continue;
            }

            if (request.missing || !request.description.has_value())
            {
                status.state = PluginRuntimeStatus::State::missing;
                status.message = "Plugin unavailable";
                statuses.push_back(std::move(status));
                continue;
            }

            status.state = PluginRuntimeStatus::State::loading;
            status.message = "Starting sandbox worker";
            {
                const juce::ScopedLock lock(pluginStatusLock);
                pluginStatuses = statuses;
                pluginStatuses.push_back(status);
            }

            auto bridge = std::make_unique<PluginBridgeClient>();
            const auto result = bridge->startPlugin(*request.description,
                                                    currentSampleRate(),
                                                    PluginBridgeSharedState::maxBlockSize,
                                                    request.state);
            if (result.failed())
            {
                auto track = std::find_if(graph.tracks.begin(),
                                          graph.tracks.end(),
                                          [&request](const auto& candidate)
                {
                    return candidate.key == runtimeKey(request.trackId);
                });
                if (track == graph.tracks.end())
                {
                    graph.tracks.push_back({ runtimeKey(request.trackId), {} });
                    track = std::prev(graph.tracks.end());
                }

                InsertRuntime failedRuntime;
                failedRuntime.insertId = request.insertId;
                failedRuntime.name = request.name;
                failedRuntime.failureDelay.delaySamples
                    = deviceBlockSize.load(std::memory_order_acquire)
                    + std::max(0, request.latencySamples);
                failedRuntime.failureDelay.buffer.setSize(
                    2,
                    failedRuntime.failureDelay.delaySamples
                        + PluginBridgeSharedState::maxBlockSize
                        + 1);
                failedRuntime.failureDelay.buffer.clear();
                track->inserts.push_back(std::move(failedRuntime));

                status.state = PluginRuntimeStatus::State::failed;
                status.message = result.getErrorMessage();
                statuses.push_back(std::move(status));
                continue;
            }

            auto track = std::find_if(graph.tracks.begin(),
                                      graph.tracks.end(),
                                      [&request](const auto& candidate)
            {
                return candidate.key == runtimeKey(request.trackId);
            });
            if (track == graph.tracks.end())
            {
                graph.tracks.push_back({ runtimeKey(request.trackId), {} });
                track = std::prev(graph.tracks.end());
            }

            InsertRuntime insertRuntime;
            insertRuntime.insertId = request.insertId;
            insertRuntime.name = request.name;
            insertRuntime.failureDelay.delaySamples
                = deviceBlockSize.load(std::memory_order_acquire)
                + bridge->reportedLatencySamples();
            insertRuntime.failureDelay.buffer.setSize(
                2,
                insertRuntime.failureDelay.delaySamples
                    + PluginBridgeSharedState::maxBlockSize
                    + 1);
            insertRuntime.failureDelay.buffer.clear();
            insertRuntime.bridge = std::move(bridge);
            track->inserts.push_back(std::move(insertRuntime));
            status.state = PluginRuntimeStatus::State::ready;
            status.message = "Sandbox ready";
            status.latencySamples = track->inserts.back().bridge->reportedLatencySamples();
            status.tailSeconds = track->inserts.back().bridge->reportedTailSeconds();
            request.latencySamples = status.latencySamples;
            request.tailSeconds = status.tailSeconds;
            statuses.push_back(std::move(status));
        }

        {
            const juce::ScopedLock lock(pluginRequestLock);
            if (generation != desiredPluginGeneration)
            {
                buildingPluginFingerprint.clear();
                continue;
            }
            if (!pendingPluginSnapshot.has_value()
                || pendingSnapshotGeneration != generation)
            {
                buildingPluginFingerprint.clear();
                continue;
            }

            configureRuntimeTiming(*pendingPluginSnapshot, requests);
            const auto runtimeDestination = chooseWritableRuntime();
            const auto snapshotDestination = chooseWritableSnapshot();
            pluginRuntimeGraphs[static_cast<std::size_t>(runtimeDestination)] = std::move(graph);
            snapshots[static_cast<std::size_t>(snapshotDestination)]
                = std::move(*pendingPluginSnapshot);
            pendingPluginSnapshot.reset();
            pluginRuntimeGenerations[static_cast<std::size_t>(runtimeDestination)].store(
                generation,
                std::memory_order_release);
            snapshotGenerations[static_cast<std::size_t>(snapshotDestination)].store(
                generation,
                std::memory_order_release);
            activePluginRuntime.store(runtimeDestination, std::memory_order_release);
            activeSnapshot.store(snapshotDestination, std::memory_order_release);
            activeRenderPair.store(renderPair(generation,
                                              snapshotDestination,
                                              runtimeDestination),
                                   std::memory_order_release);
            activePluginFingerprint = fingerprint;
            activePluginGeneration = generation;
            buildingPluginFingerprint.clear();
        }
        pluginLateBlocks.store(0, std::memory_order_relaxed);

        {
            const juce::ScopedLock lock(pluginStatusLock);
            pluginStatuses = std::move(statuses);
        }
    }
}

juce::String StudioAudioEngine::pluginRuntimeFingerprint(
    const std::vector<PluginRuntimeRequest>& requests) const
{
    juce::String fingerprint = juce::String(currentSampleRate(), 3)
        + "|"
        + juce::String(deviceBlockSize.load(std::memory_order_acquire))
        + "|";
    for (const auto& request : requests)
    {
        auto stateHash = std::uint64_t { 1469598103934665603ULL };
        const auto* bytes = static_cast<const std::uint8_t*>(request.state.getData());
        for (std::size_t index = 0; index < request.state.getSize(); ++index)
        {
            stateHash ^= bytes[index];
            stateHash *= 1099511628211ULL;
        }

        fingerprint << request.trackId
                    << ":"
                    << request.insertId
                    << ":"
                    << (request.bypassed ? "b" : "e")
                    << ":"
                    << (request.missing ? "m" : "a")
                    << ":"
                    << juce::String(request.catalogRevision)
                    << ":"
                    << juce::String::toHexString(static_cast<juce::int64>(stateHash))
                    << ":";
        if (request.description.has_value())
            fingerprint << request.description->createIdentifierString();
        fingerprint << ";";
    }
    return fingerprint;
}

std::uint64_t StudioAudioEngine::runtimeKey(const juce::String& trackId) noexcept
{
    return static_cast<std::uint64_t>(trackId.hashCode64());
}

std::uint64_t StudioAudioEngine::renderPair(std::uint64_t generation,
                                            int snapshotIndex,
                                            int runtimeIndex) noexcept
{
    return (generation << 4)
        | (static_cast<std::uint64_t>(snapshotIndex & 0x3) << 2)
        | static_cast<std::uint64_t>(runtimeIndex & 0x3);
}

int StudioAudioEngine::chooseWritableRuntime() const noexcept
{
    const auto active = activePluginRuntime.load(std::memory_order_acquire);
    const auto reading = readingPluginRuntime.load(std::memory_order_acquire);

    for (int index = 0; index < static_cast<int>(pluginRuntimeGraphs.size()); ++index)
        if (index != active && index != reading)
            return index;

    return (active + 1) % static_cast<int>(pluginRuntimeGraphs.size());
}

void StudioAudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                         int numInputChannels,
                                                         float* const* outputChannelData,
                                                         int numOutputChannels,
                                                         int numSamples,
                                                         const juce::AudioIODeviceCallbackContext&)
{
    const auto calibrationBlockActive = calibrationActive.load(
        std::memory_order_acquire);
    const auto calibrationElapsed = calibrationBlockActive
        ? calibrationSamplesElapsed.load(std::memory_order_relaxed)
        : 0;
    if (calibrationBlockActive && calibrationElapsed > 0)
    {
        const auto inputChannel = calibrationInputChannel.load(
            std::memory_order_relaxed);
        const auto* input = inputChannel < numInputChannels
            ? inputChannelData[inputChannel]
            : nullptr;
        if (input != nullptr)
        {
            for (int sample = 0; sample < numSamples; ++sample)
            {
                if (std::abs(input[sample]) < 0.2f)
                    continue;
                calibrationLatencySamples.store(
                    static_cast<int>(calibrationElapsed + sample),
                    std::memory_order_release);
                calibrationActive.store(false, std::memory_order_release);
                calibrationResultReady.store(true, std::memory_order_release);
                break;
            }
        }
    }

    auto recordingBlockActive = false;
    RecordingCallbackScope recordingScope;
    if (recordingAccepting.load(std::memory_order_acquire))
    {
        recordingCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
        recordingScope.counter = &recordingCallbacksInFlight;
        if (recordingAccepting.load(std::memory_order_acquire))
        {
            recordingBlockActive = true;
            const auto recorderCount = activeRecorderCount.load(std::memory_order_acquire);
            const auto callbackStart = playheadSample.load(std::memory_order_acquire);
            const auto captureStart = recordingCaptureStartSample.load(
                std::memory_order_relaxed);
            const auto captureEnd = recordingCaptureEndSample.load(
                std::memory_order_relaxed);
            const auto callbackEnd = callbackStart + numSamples;
            const auto firstCaptureSample = std::max(callbackStart, captureStart);
            const auto lastCaptureSample = captureEnd >= 0
                ? std::min(callbackEnd, captureEnd)
                : callbackEnd;
            const auto sourceSampleOffset = static_cast<int>(
                std::max<std::int64_t>(0, firstCaptureSample - callbackStart));
            const auto requestedCaptureSamples = static_cast<int>(
                std::max<std::int64_t>(0, lastCaptureSample - firstCaptureSample));

            if (requestedCaptureSamples > 0)
            {
                std::array<int, maximumRecordingTracks> freeSamples {};
                for (int index = 0; index < recorderCount; ++index)
                {
                    freeSamples[static_cast<std::size_t>(index)]
                        = recorders[static_cast<std::size_t>(index)]->availableSamples();
                }

                const auto captureSamples = synchronizedCaptureSamples(
                    requestedCaptureSamples,
                    std::span<const int>(freeSamples.data(),
                                         static_cast<std::size_t>(recorderCount)));
                const auto droppedSamples = requestedCaptureSamples - captureSamples;
                for (int index = 0; index < recorderCount; ++index)
                {
                    auto& recorder = recorders[static_cast<std::size_t>(index)];
                    recorder->noteDroppedSamples(droppedSamples);
                    recorder->push(inputChannelData,
                                   numInputChannels,
                                   sourceSampleOffset,
                                   captureSamples);
                }
            }
        }
    }

    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    if (numOutputChannels == 0)
    {
        outputLeftPeak.store(0.0f, std::memory_order_relaxed);
        outputRightPeak.store(0.0f, std::memory_order_relaxed);
        return;
    }

    auto leftPeakValue = 0.0f;
    auto rightPeakValue = 0.0f;
    const auto recordingSessionPresent = activeRecorderCount.load(std::memory_order_acquire) > 0;
    if (recordingBlockActive
        || (!recordingSessionPresent && playing.load(std::memory_order_acquire)))
    {
        int snapshotIndex = 0;
        int runtimeIndex = 0;
        auto pairAcquired = false;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto pair = activeRenderPair.load(std::memory_order_acquire);
            runtimeIndex = static_cast<int>(pair & 0x3);
            snapshotIndex = static_cast<int>((pair >> 2) & 0x3);
            readingSnapshot.store(snapshotIndex, std::memory_order_release);
            readingPluginRuntime.store(runtimeIndex, std::memory_order_release);
            if (pair == activeRenderPair.load(std::memory_order_acquire))
            {
                pairAcquired = true;
                break;
            }
        }
        if (!pairAcquired)
        {
            readingSnapshot.store(-1, std::memory_order_release);
            readingPluginRuntime.store(-1, std::memory_order_release);
            outputLeftPeak.store(0.0f, std::memory_order_relaxed);
            outputRightPeak.store(0.0f, std::memory_order_relaxed);
            return;
        }

        auto& snapshot = snapshots[static_cast<std::size_t>(snapshotIndex)];
        auto position = playheadSample.load(std::memory_order_acquire);
        const auto renderingRecording = recordingBlockActive;
        const auto renderLooping = snapshot.loopEnabled
            && (!renderingRecording
                || recordingLoopEnabled.load(std::memory_order_relaxed));
        const auto recordingTransportEnd = renderingRecording
            ? recordingTransportEndSample.load(std::memory_order_relaxed)
            : -1;
        auto outputOffset = 0;

        while (outputOffset < numSamples)
        {
            auto samplesThisBlock = std::min(
                numSamples - outputOffset,
                PluginBridgeSharedState::maxBlockSize);

            if (recordingTransportEnd >= 0)
            {
                if (position >= recordingTransportEnd)
                {
                    playing.store(false, std::memory_order_release);
                    break;
                }
                samplesThisBlock = static_cast<int>(std::min<std::int64_t>(
                    samplesThisBlock,
                    recordingTransportEnd - position));
            }

            if (position >= snapshot.lengthSamples && !renderingRecording)
            {
                playing.store(false, std::memory_order_release);
                break;
            }

            snapshot.masterBuffer.clear(0, 0, samplesThisBlock);
            snapshot.masterBuffer.clear(1, 0, samplesThisBlock);

            for (auto& track : snapshot.tracks)
            {
                track.processingBuffer.clear(0, 0, samplesThisBlock);
                track.processingBuffer.clear(1, 0, samplesThisBlock);

                for (auto& source : track.sources)
                {
                    renderSourceBlock(source,
                                      position,
                                      samplesThisBlock,
                                      renderLooping,
                                      snapshot.loopStartSample,
                                      snapshot.loopEndSample);

                    float* sourceChannels[] {
                        source.processingBuffer.getWritePointer(0),
                        source.processingBuffer.getWritePointer(1)
                    };
                    juce::AudioBuffer<float> sourceView(sourceChannels,
                                                        2,
                                                        samplesThisBlock);
                    if (!source.isParentContent)
                        processRuntimeChain(source.runtimeKey, sourceView);
                    applyDelayCompensation(source.processingBuffer,
                                           samplesThisBlock,
                                           source.compensation);

                    if (source.audible)
                    {
                        track.processingBuffer.addFrom(0,
                                                       0,
                                                       source.processingBuffer,
                                                       0,
                                                       0,
                                                       samplesThisBlock);
                        track.processingBuffer.addFrom(1,
                                                       0,
                                                       source.processingBuffer,
                                                       1,
                                                       0,
                                                       samplesThisBlock);
                    }
                }

                float* trackChannels[] {
                    track.processingBuffer.getWritePointer(0),
                    track.processingBuffer.getWritePointer(1)
                };
                juce::AudioBuffer<float> trackView(trackChannels,
                                                   2,
                                                   samplesThisBlock);
                processRuntimeChain(track.runtimeKey, trackView);
                applyTrackGainAndPan(track.processingBuffer,
                                     samplesThisBlock,
                                     track.volumeGain,
                                     track.pan);
                applyDelayCompensation(track.processingBuffer,
                                       samplesThisBlock,
                                       track.compensation);
                for (const auto& send : snapshot.hardwareSends)
                {
                    if (send.sourceRuntimeKey != track.runtimeKey)
                        continue;
                    if (send.outputChannel >= 0
                        && send.outputChannel < numOutputChannels
                        && outputChannelData[send.outputChannel] != nullptr)
                    {
                        for (int sample = 0; sample < samplesThisBlock; ++sample)
                        {
                            outputChannelData[send.outputChannel][outputOffset + sample]
                                += track.processingBuffer.getSample(0, sample);
                        }
                    }
                    if (send.outputChannel + 1 < numOutputChannels
                        && outputChannelData[send.outputChannel + 1] != nullptr)
                    {
                        for (int sample = 0; sample < samplesThisBlock; ++sample)
                        {
                            outputChannelData[send.outputChannel + 1][outputOffset + sample]
                                += track.processingBuffer.getSample(1, sample);
                        }
                    }
                }
                if (track.audible)
                {
                    snapshot.masterBuffer.addFrom(0,
                                                  0,
                                                  track.processingBuffer,
                                                  0,
                                                  0,
                                                  samplesThisBlock);
                    snapshot.masterBuffer.addFrom(1,
                                                  0,
                                                  track.processingBuffer,
                                                  1,
                                                  0,
                                                  samplesThisBlock);
                }
            }

            float* masterChannels[] {
                snapshot.masterBuffer.getWritePointer(0),
                snapshot.masterBuffer.getWritePointer(1)
            };
            juce::AudioBuffer<float> masterView(masterChannels,
                                                2,
                                                samplesThisBlock);
            processRuntimeChain(snapshot.masterRuntimeKey, masterView);
            if (snapshot.masterAudible)
                applyTrackGainAndPan(snapshot.masterBuffer,
                                     samplesThisBlock,
                                     snapshot.masterGain,
                                     snapshot.masterPan);
            else
                snapshot.masterBuffer.clear();

            snapshot.clickBuffer.clear(0, 0, samplesThisBlock);
            snapshot.clickBuffer.clear(1, 0, samplesThisBlock);
            if (metronomeEnabled.load(std::memory_order_relaxed))
            {
                for (int sample = 0; sample < samplesThisBlock; ++sample)
                {
                    float clickLeft = 0.0f;
                    float clickRight = 0.0f;
                    auto clickPosition = position + sample;
                    if (renderLooping
                        && clickPosition >= snapshot.loopEndSample)
                    {
                        const auto loopLength = snapshot.loopEndSample - snapshot.loopStartSample;
                        if (loopLength > 0)
                            clickPosition = snapshot.loopStartSample
                                + (clickPosition - snapshot.loopStartSample) % loopLength;
                    }
                    if (renderingRecording || clickPosition < snapshot.contentLengthSamples)
                        addMetronome(snapshot, clickPosition, clickLeft, clickRight);
                    snapshot.clickBuffer.setSample(0, sample, clickLeft);
                    snapshot.clickBuffer.setSample(1, sample, clickRight);
                }
            }
            applyDelayCompensation(snapshot.clickBuffer,
                                   samplesThisBlock,
                                   snapshot.clickCompensation);

            for (int sample = 0; sample < samplesThisBlock; ++sample)
            {
                if (outputChannelData[0] != nullptr)
                    outputChannelData[0][outputOffset + sample]
                        += snapshot.masterBuffer.getSample(0, sample);
                if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
                    outputChannelData[1][outputOffset + sample]
                        += snapshot.masterBuffer.getSample(1, sample);

                const auto clickOutput = snapshot.metronomeOutputChannel;
                if (clickOutput >= 0
                    && clickOutput < numOutputChannels
                    && outputChannelData[clickOutput] != nullptr)
                {
                    outputChannelData[clickOutput][outputOffset + sample]
                        += snapshot.clickBuffer.getSample(0, sample);
                }
                if (clickOutput + 1 < numOutputChannels
                    && outputChannelData[clickOutput + 1] != nullptr)
                {
                    outputChannelData[clickOutput + 1][outputOffset + sample]
                        += snapshot.clickBuffer.getSample(1, sample);
                }
            }

            position += samplesThisBlock;
            outputOffset += samplesThisBlock;
            if (recordingTransportEnd >= 0 && position >= recordingTransportEnd)
            {
                position = recordingTransportEnd;
                playing.store(false, std::memory_order_release);
                break;
            }
            if (renderLooping
                && position >= snapshot.loopEndSample)
            {
                const auto loopLength = snapshot.loopEndSample - snapshot.loopStartSample;
                if (loopLength > 0)
                    position = snapshot.loopStartSample
                        + (position - snapshot.loopStartSample) % loopLength;
            }
            else if (!renderingRecording && position >= snapshot.lengthSamples)
            {
                position = snapshot.lengthSamples;
                playing.store(false, std::memory_order_release);
            }
        }

        playheadSample.store(position, std::memory_order_release);
        readingPluginRuntime.store(-1, std::memory_order_release);
        readingSnapshot.store(-1, std::memory_order_release);
    }

    if (!calibrationBlockActive
        && monitoringEnabled.load(std::memory_order_acquire))
    {
        const auto firstInput = monitoringFirstInput.load(std::memory_order_relaxed);
        const auto inputCount = monitoringChannels.load(std::memory_order_relaxed);
        const auto* monitorLeft = firstInput < numInputChannels
            ? inputChannelData[firstInput]
            : nullptr;
        const auto* monitorRight = inputCount > 1 && firstInput + 1 < numInputChannels
            ? inputChannelData[firstInput + 1]
            : monitorLeft;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (outputChannelData[0] != nullptr && monitorLeft != nullptr)
                outputChannelData[0][sample] += monitorLeft[sample];
            if (numOutputChannels > 1
                && outputChannelData[1] != nullptr
                && monitorRight != nullptr)
                outputChannelData[1][sample] += monitorRight[sample];
        }
    }

    if (calibrationBlockActive)
    {
        const auto outputChannel = calibrationOutputChannel.load(
            std::memory_order_relaxed);
        if (calibrationElapsed == 0
            && outputChannel < numOutputChannels
            && outputChannelData[outputChannel] != nullptr)
            outputChannelData[outputChannel][0] += 0.8f;

        const auto elapsed = calibrationElapsed + numSamples;
        calibrationSamplesElapsed.store(elapsed, std::memory_order_relaxed);
        if (calibrationActive.load(std::memory_order_acquire)
            && elapsed >= static_cast<std::int64_t>(currentSampleRate() * 2.0))
        {
            calibrationLatencySamples.store(-1, std::memory_order_release);
            calibrationActive.store(false, std::memory_order_release);
            calibrationResultReady.store(true, std::memory_order_release);
        }
    }

    for (int sample = 0; sample < numSamples; ++sample)
    {
        for (int channel = 0; channel < numOutputChannels; ++channel)
        {
            if (outputChannelData[channel] == nullptr)
                continue;
            outputChannelData[channel][sample] = juce::jlimit(
                -1.0f,
                1.0f,
                outputChannelData[channel][sample]);
            if (channel == 0)
                leftPeakValue = std::max(leftPeakValue,
                                         std::abs(outputChannelData[channel][sample]));
            else if (channel == 1)
                rightPeakValue = std::max(rightPeakValue,
                                          std::abs(outputChannelData[channel][sample]));
        }
    }

    outputLeftPeak.store(leftPeakValue, std::memory_order_relaxed);
    outputRightPeak.store(rightPeakValue, std::memory_order_relaxed);
}

void StudioAudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate.store(device != nullptr ? device->getCurrentSampleRate() : 48000.0,
                     std::memory_order_release);
    deviceBlockSize.store(device != nullptr ? device->getCurrentBufferSizeSamples() : 512,
                          std::memory_order_release);
}

void StudioAudioEngine::audioDeviceStopped()
{
    playing.store(false, std::memory_order_release);
    outputLeftPeak.store(0.0f, std::memory_order_relaxed);
    outputRightPeak.store(0.0f, std::memory_order_relaxed);
}

void StudioAudioEngine::audioDeviceError(const juce::String& errorMessage)
{
    juce::Logger::writeToLog("audio.device: " + errorMessage);
}
}
