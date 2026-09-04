#include "StudioAudioEngine.h"

#include "mix/RoutingGraphCompiler.h"
#include "plugin_host/PluginFormats.h"
#include "plugin_host/ClapPluginInstance.h"
#include "devices/DeviceRegistry.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>

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

std::int64_t wrapLoopPosition(std::int64_t position,
                              std::int64_t loopStart,
                              std::int64_t loopEnd) noexcept
{
    const auto loopLength = loopEnd - loopStart;
    if (loopLength <= 0 || position < loopEnd)
        return position;
    return loopStart + (position - loopStart) % loopLength;
}

std::shared_ptr<juce::AudioProcessor> messageThreadOwnedProcessor(
    std::unique_ptr<juce::AudioProcessor> processor)
{
    return {
        processor.release(),
        [](juce::AudioProcessor* value)
        {
            if (value == nullptr)
                return;
            const auto* messages =
                juce::MessageManager::getInstanceWithoutCreating();
            if (messages == nullptr
                || messages->isThisTheMessageThread())
            {
                delete value;
                return;
            }
            if (!juce::MessageManager::callAsync(
                    [value] { delete value; }))
            {
                juce::Logger::writeToLog(
                    "plugin.runtime: message thread unavailable during processor teardown");
            }
        }
    };
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

struct AtomicCounterScope
{
    ~AtomicCounterScope()
    {
        counter.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::atomic<int>& counter;
};

float automatedVolumeGain(
    const std::optional<CompiledAutomationLane>& automation,
    std::int64_t sample,
    float fallback) noexcept
{
    if (!automation.has_value())
        return fallback;
    const auto normalized = juce::jlimit(
        0.0,
        1.0,
        automation->valueAt(sample));
    return juce::Decibels::decibelsToGain(
        static_cast<float>(-60.0 + normalized * 72.0));
}

float automatedPan(
    const std::optional<CompiledAutomationLane>& automation,
    std::int64_t sample,
    float fallback) noexcept
{
    return automation.has_value()
        ? static_cast<float>(
              juce::jlimit(0.0, 1.0, automation->valueAt(sample))
                  * 2.0
              - 1.0)
        : fallback;
}

bool automatedSwitch(
    const std::optional<CompiledAutomationLane>& automation,
    std::int64_t sample,
    bool fallback) noexcept
{
    return automation.has_value()
        ? automation->valueAt(sample) >= 0.5
        : fallback;
}
}

bool StudioAudioEngine::waitForPluginRuntimeTransition(
    int timeoutMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(
            std::max(1, timeoutMilliseconds));
#if JUCE_MODAL_LOOPS_PERMITTED
    if (auto* messages =
            juce::MessageManager::getInstanceWithoutCreating();
        messages != nullptr && messages->isThisTheMessageThread())
    {
        while (pluginRuntimeTransitionPending()
               && !shuttingDown.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline)
        {
            messages->runDispatchLoopUntil(5);
        }
        return !shuttingDown.load(std::memory_order_acquire)
            && !pluginRuntimeTransitionPending();
    }
#endif
    while (pluginRuntimeTransitionPending()
           && !shuttingDown.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline)
    {
        juce::Thread::sleep(1);
    }
    return !shuttingDown.load(std::memory_order_acquire)
        && !pluginRuntimeTransitionPending();
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

StudioAudioEngine::InsertRuntime::InsertRuntime()
    : parameterEvents(
          static_cast<std::size_t>(
              PluginBridgeSharedState::maxParameterEvents))
{
    automationBoundaries.reserve(
        static_cast<std::size_t>(
            PluginBridgeSharedState::maxParameterEvents * 2)
        + static_cast<std::size_t>(
            PluginBridgeSharedState::maxBlockSize / 8 + 2));
}

StudioAudioEngine::InsertRuntime::~InsertRuntime()
{
    if (inProcess != nullptr)
        inProcess->releaseResources();
}

StudioAudioEngine::~StudioAudioEngine()
{
    editorCallbacksAlive->store(false, std::memory_order_release);
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
    closeInProcessEditors();

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
#if JUCE_MODAL_LOOPS_PERMITTED
    if (auto* messages =
            juce::MessageManager::getInstanceWithoutCreating();
        messages != nullptr && messages->isThisTheMessageThread())
    {
        while (!pluginRuntimeBuilder.removeAllJobs(false, 0))
            messages->runDispatchLoopUntil(5);
    }
    else
#endif
    {
        pluginRuntimeBuilder.removeAllJobs(false, -1);
    }

    struct RuntimeCleanupState
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
    };
    const auto cleanupState =
        std::make_shared<RuntimeCleanupState>();
    pluginRuntimeBuilder.addJob(
        [this, cleanupState]
        {
            {
                const juce::ScopedLock lock(pluginRequestLock);
                for (auto& graph : pluginRuntimeGraphs)
                {
                    for (auto& track : graph.tracks)
                    {
                        for (auto& insert : track.inserts)
                        {
                            if (insert.inProcess != nullptr)
                                insert.inProcess->releaseResources();
                            insert.araDocument.reset();
                        }
                    }
                }
            }
            {
                const std::lock_guard lock(cleanupState->mutex);
                cleanupState->completed = true;
            }
            cleanupState->condition.notify_one();
        });
#if JUCE_MODAL_LOOPS_PERMITTED
    if (auto* messages =
            juce::MessageManager::getInstanceWithoutCreating();
        messages != nullptr && messages->isThisTheMessageThread())
    {
        for (;;)
        {
            {
                const std::lock_guard cleanupGuard(
                    cleanupState->mutex);
                if (cleanupState->completed)
                    break;
            }
            messages->runDispatchLoopUntil(5);
        }
    }
    else
#endif
    {
        std::unique_lock cleanupLock(cleanupState->mutex);
        cleanupState->condition.wait(
            cleanupLock,
            [cleanupState] { return cleanupState->completed; });
    }
    {
        const juce::ScopedLock lock(pluginRequestLock);
        for (auto& graph : pluginRuntimeGraphs)
            graph.tracks.clear();
    }
    activePluginRuntime.store(0, std::memory_order_release);
    readingPluginRuntime.store(-1, std::memory_order_release);
    activeSnapshot.store(0, std::memory_order_release);
    activeRenderPair.store(0, std::memory_order_release);
    readingSnapshot.store(-1, std::memory_order_release);
    for (auto& generation : snapshotGenerations)
        generation.store(0, std::memory_order_relaxed);
    for (auto& readers : snapshotReaders)
        readers.store(0, std::memory_order_relaxed);
    for (auto& generation : pluginRuntimeGenerations)
        generation.store(0, std::memory_order_relaxed);
    for (auto& readers : pluginRuntimeReaders)
        readers.store(0, std::memory_order_relaxed);
    pluginLateBlocks.store(0, std::memory_order_relaxed);
    pluginAutomationEventsDropped.store(
        0,
        std::memory_order_relaxed);
    exclusiveAudioOperation.store(0, std::memory_order_release);
    {
        const juce::ScopedLock lock(pluginRequestLock);
        pendingPluginRequests.clear();
        activePluginRequests.clear();
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
        pluginStateCapturePending = false;
        pendingEditorCloseRequired = false;
        activeSnapshotTemplate.reset();
        activeSnapshotRevision = 0;
        pluginSnapshotRefreshRevision = 0;
        pluginSnapshotRefreshPending = false;
        pluginSnapshotRefreshRunning = false;
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
    return updateProjectInternal(
        project,
        std::move(pluginRequests),
        false);
}

juce::Result StudioAudioEngine::updateProjectInternal(
    const Project& project,
    std::vector<PluginRuntimeRequest> pluginRequests,
    bool renderOwned)
{
    if (renderInProgress.load(std::memory_order_acquire)
        && !renderOwned)
        return juce::Result::fail(
            "Wait for the active render to finish.");
    metronomeEnabled.store(project.metronomeEnabled, std::memory_order_release);
    juce::String error;
    auto snapshot = buildSnapshot(project,
                                  currentSampleRate(),
                                  pluginRequests,
                                  error);
    if (!snapshot.has_value())
        return juce::Result::fail(error);

    requestPluginRuntime(
        std::move(pluginRequests),
        std::move(*snapshot),
        renderOwned);
    return juce::Result::ok();
}

void StudioAudioEngine::play() noexcept
{
    if (renderInProgress.load(std::memory_order_acquire))
        return;
    playing.store(true, std::memory_order_release);
}

void StudioAudioEngine::pause() noexcept
{
    if (renderInProgress.load(std::memory_order_acquire))
        return;
    playing.store(false, std::memory_order_release);
}

void StudioAudioEngine::stop() noexcept
{
    if (renderInProgress.load(std::memory_order_acquire))
        return;
    playing.store(false, std::memory_order_release);
    playheadSample.store(0, std::memory_order_release);
}

void StudioAudioEngine::seekSeconds(double seconds) noexcept
{
    if (renderInProgress.load(std::memory_order_acquire))
        return;
    auto target = static_cast<std::int64_t>(
        std::max(0.0, seconds) * currentSampleRate());
    {
        const juce::ScopedLock lock(pluginRequestLock);
        if (activeSnapshotTemplate != nullptr
            && activeSnapshotTemplate->loopEnabled)
        {
            target = wrapLoopPosition(
                target,
                activeSnapshotTemplate->loopStartSample,
                activeSnapshotTemplate->loopEndSample);
        }
    }
    playheadSample.store(target, std::memory_order_release);
}

bool StudioAudioEngine::resetPluginProcessing()
{
    if (recordingAccepting.load(std::memory_order_acquire)
        || activeRecorderCount.load(std::memory_order_acquire) > 0
        || recordingFinalizing.load(std::memory_order_acquire)
        || pluginStateOperationActive.load(std::memory_order_acquire))
    {
        return false;
    }

    auto expectedReset = false;
    if (!pluginResetInProgress.compare_exchange_strong(
            expectedReset,
            true,
            std::memory_order_acq_rel))
    {
        return false;
    }
    struct ResetScope
    {
        ~ResetScope()
        {
            active.store(false, std::memory_order_release);
        }

        std::atomic<bool>& active;
    } resetScope { pluginResetInProgress };
    if (!waitForPluginProcessingToStop(2000))
        return false;

    const auto resetDelay = [](auto& delay)
    {
        if (delay.state == nullptr)
            return;
        delay.state->buffer.clear();
        delay.state->writePosition = 0;
        delay.state->samplesWritten.store(
            0,
            std::memory_order_release);
        for (auto& position : delay.state->transitionPositions)
            position.store(0, std::memory_order_release);
        for (auto& mask : delay.state->transitionHandoffMasks)
            mask.store(0, std::memory_order_release);
        delay.transition.reset();
    };
    const juce::ScopedLock lock(pluginRequestLock);
    for (auto& graph : pluginRuntimeGraphs)
    {
        for (auto& track : graph.tracks)
        {
            for (auto& insert : track.inserts)
            {
                if (insert.bridge != nullptr)
                    insert.bridge->resetProcessing();
                if (insert.inProcess != nullptr)
                    insert.inProcess->reset();
                resetDelay(insert.failureDelay);
            }
        }
    }
    for (auto& snapshot : snapshots)
    {
        for (auto& track : snapshot.tracks)
        {
            for (auto& source : track.sources)
                resetDelay(source.compensation);
            for (auto& route : track.routes)
                resetDelay(route.compensation);
            resetDelay(track.compensation);
        }
        resetDelay(snapshot.clickCompensation);
    }
    return true;
}

bool StudioAudioEngine::waitForPluginProcessingToStop(
    int timeoutMilliseconds) const
{
    for (auto waited = 0;
         waited < std::max(1, timeoutMilliseconds)
         && (audioCallbacksInFlight.load(std::memory_order_acquire) > 0
             || pluginControlOperationsInFlight.load(
                    std::memory_order_acquire)
                 > 0);
         ++waited)
    {
        juce::Thread::sleep(1);
    }
    return audioCallbacksInFlight.load(std::memory_order_acquire) == 0
        && pluginControlOperationsInFlight.load(
               std::memory_order_acquire)
            == 0;
}

void StudioAudioEngine::setMetronomeEnabled(bool enabled) noexcept
{
    if (renderInProgress.load(std::memory_order_acquire))
        return;
    metronomeEnabled.store(enabled, std::memory_order_release);
}

void StudioAudioEngine::setInputMonitoring(bool enabled,
                                           int firstInputChannel,
                                           int channels) noexcept
{
    if (renderInProgress.load(std::memory_order_acquire))
        return;
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

#if defined(STUDIO_DUO_TESTING)
int StudioAudioEngine::minimumRouteBufferCapacityForTesting() const noexcept
{
    const juce::ScopedLock lock(pluginRequestLock);
    const auto& snapshot = activeSnapshotTemplate != nullptr
        ? *activeSnapshotTemplate
        : snapshots.front();
    auto capacity = PluginBridgeSharedState::maxBlockSize;
    auto foundRoute = false;
    for (const auto& track : snapshot.tracks)
    {
        for (const auto& route : track.routes)
        {
            foundRoute = true;
            capacity = std::min(capacity,
                                route.processingBuffer.getNumSamples());
        }
    }
    return foundRoute ? capacity : 0;
}

double StudioAudioEngine::activeSnapshotSampleRateForTesting() const noexcept
{
    const juce::ScopedLock lock(pluginRequestLock);
    return activeSnapshotTemplate != nullptr
        ? activeSnapshotTemplate->sampleRate
        : snapshots.front().sampleRate;
}

juce::AudioBuffer<float>
StudioAudioEngine::renderActiveBlockForTesting(int samples)
{
    juce::AudioBuffer<float> output(
        2,
        std::max(1, samples));
    output.clear();
    float* channels[] {
        output.getWritePointer(0),
        output.getWritePointer(1)
    };
    juce::AudioIODeviceCallbackContext context;
    audioDeviceIOCallbackWithContext(
        nullptr,
        0,
        channels,
        2,
        output.getNumSamples(),
        context);
    return output;
}

void StudioAudioEngine::processActiveBlockForTesting(int samples)
{
    juce::ignoreUnused(renderActiveBlockForTesting(samples));
}

std::vector<float> StudioAudioEngine::delayTransitionForTesting(
    int previousDelay,
    int nextDelay,
    int outputSamples)
{
    constexpr auto quantum = 16;
    RenderSource::DelayCompensator delay;
    configureDelayCompensator(
        delay,
        previousDelay,
        quantum);
    juce::AudioBuffer<float> history(2, 64);
    for (int sample = 0; sample < history.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(sample) / 100.0f;
        history.setSample(0, sample, value);
        history.setSample(1, sample, value);
    }
    applyDelayCompensation(
        history,
        history.getNumSamples(),
        delay);

    configureDelayCompensator(
        delay,
        nextDelay,
        quantum);
    juce::AudioBuffer<float> transition(
        2,
        std::max(1, outputSamples));
    for (int sample = 0;
         sample < transition.getNumSamples();
         ++sample)
    {
        const auto value =
            static_cast<float>(64 + sample) / 100.0f;
        transition.setSample(0, sample, value);
        transition.setSample(1, sample, value);
    }
    applyDelayCompensation(
        transition,
        transition.getNumSamples(),
        delay);

    std::vector<float> output(
        static_cast<std::size_t>(
            transition.getNumSamples()));
    std::copy_n(
        transition.getReadPointer(0),
        transition.getNumSamples(),
        output.begin());
    return output;
}

std::vector<float>
StudioAudioEngine::delayRefreshAfterTransitionForTesting(
    int previousDelay,
    int nextDelay)
{
    constexpr auto quantum = 16;
    RenderSource::DelayCompensator activeDelay;
    configureDelayCompensator(
        activeDelay,
        previousDelay,
        quantum);
    juce::AudioBuffer<float> history(2, 64);
    for (int sample = 0; sample < history.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(sample) / 100.0f;
        history.setSample(0, sample, value);
        history.setSample(1, sample, value);
    }
    applyDelayCompensation(
        history,
        history.getNumSamples(),
        activeDelay);

    configureDelayCompensator(
        activeDelay,
        nextDelay,
        quantum);
    const auto immutableTemplate = activeDelay;
    const auto transitionSamples =
        std::max(0, nextDelay) + quantum;
    juce::AudioBuffer<float> transition(
        2,
        transitionSamples);
    for (int sample = 0;
         sample < transition.getNumSamples();
         ++sample)
    {
        const auto value =
            static_cast<float>(64 + sample) / 100.0f;
        transition.setSample(0, sample, value);
        transition.setSample(1, sample, value);
    }
    applyDelayCompensation(
        transition,
        transition.getNumSamples(),
        activeDelay);

    auto republishedDelay = immutableTemplate;
    configureDelayCompensator(
        republishedDelay,
        nextDelay,
        quantum);
    juce::AudioBuffer<float> followup(2, quantum);
    for (int sample = 0; sample < followup.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(
            64 + transitionSamples + sample)
            / 100.0f;
        followup.setSample(0, sample, value);
        followup.setSample(1, sample, value);
    }
    applyDelayCompensation(
        followup,
        followup.getNumSamples(),
        republishedDelay);

    std::vector<float> output(
        static_cast<std::size_t>(quantum));
    std::copy_n(
        followup.getReadPointer(0),
        quantum,
        output.begin());
    return output;
}

std::vector<float>
StudioAudioEngine::retiredDelayGenerationForTesting()
{
    constexpr auto quantum = 16;
    RenderSource::DelayCompensator firstGeneration;
    configureDelayCompensator(
        firstGeneration,
        4,
        quantum);
    juce::AudioBuffer<float> history(2, 64);
    for (int sample = 0; sample < history.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(sample) / 100.0f;
        history.setSample(0, sample, value);
        history.setSample(1, sample, value);
    }
    applyDelayCompensation(
        history,
        history.getNumSamples(),
        firstGeneration);

    configureDelayCompensator(
        firstGeneration,
        8,
        quantum,
        0);
    juce::AudioBuffer<float> initialTransition(2, 4);
    for (int sample = 0;
         sample < initialTransition.getNumSamples();
         ++sample)
    {
        const auto value =
            static_cast<float>(64 + sample) / 100.0f;
        initialTransition.setSample(0, sample, value);
        initialTransition.setSample(1, sample, value);
    }
    applyDelayCompensation(
        initialTransition,
        initialTransition.getNumSamples(),
        firstGeneration);

    auto retiredGeneration = firstGeneration;
    auto secondGeneration = firstGeneration;
    configureDelayCompensator(
        secondGeneration,
        8,
        quantum,
        1);
    auto thirdGeneration = secondGeneration;
    configureDelayCompensator(
        thirdGeneration,
        8,
        quantum,
        2);

    juce::AudioBuffer<float> retiredOutput(2, 1);
    retiredOutput.setSample(0, 0, 0.68f);
    retiredOutput.setSample(1, 0, 0.68f);
    applyDelayCompensation(
        retiredOutput,
        1,
        retiredGeneration);

    juce::AudioBuffer<float> thirdOutput(2, 1);
    thirdOutput.setSample(0, 0, 0.69f);
    thirdOutput.setSample(1, 0, 0.69f);
    applyDelayCompensation(
        thirdOutput,
        1,
        thirdGeneration);
    return {
        retiredOutput.getSample(0, 0),
        thirdOutput.getSample(0, 0)
    };
}

std::array<int, 2>
StudioAudioEngine::snapshotPublicationSlotsForTesting()
{
    StudioAudioEngine engine;
    engine.activeSnapshot.store(0, std::memory_order_release);
    engine.activeRenderPair.store(
        renderPair(0, 0, 0),
        std::memory_order_release);
    std::atomic<bool> readerReady { false };
    std::atomic<bool> releaseReader { false };
    std::thread retiredReader(
        [&engine, &readerReady, &releaseReader]
        {
            engine.snapshotReaders[0].fetch_add(
                1,
                std::memory_order_seq_cst);
            readerReady.store(true, std::memory_order_release);
            while (!releaseReader.load(std::memory_order_acquire))
                std::this_thread::yield();
            engine.snapshotReaders[0].fetch_sub(
                1,
                std::memory_order_seq_cst);
        });
    while (!readerReady.load(std::memory_order_acquire))
        std::this_thread::yield();

    const auto secondGeneration =
        engine.chooseWritableSnapshot();
    engine.activeSnapshot.store(
        secondGeneration,
        std::memory_order_release);
    engine.activeRenderPair.store(
        renderPair(0, secondGeneration, 0),
        std::memory_order_release);
    const auto thirdGeneration =
        engine.chooseWritableSnapshot();

    releaseReader.store(true, std::memory_order_release);
    retiredReader.join();
    return { secondGeneration, thirdGeneration };
}

float StudioAudioEngine::delayPublicationHandoffForTesting()
{
    constexpr auto quantum = 16;
    RenderSource::DelayCompensator sourceGeneration;
    configureDelayCompensator(
        sourceGeneration,
        4,
        quantum);
    juce::AudioBuffer<float> history(2, 64);
    for (int sample = 0; sample < history.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(sample) / 100.0f;
        history.setSample(0, sample, value);
        history.setSample(1, sample, value);
    }
    applyDelayCompensation(
        history,
        history.getNumSamples(),
        sourceGeneration);
    configureDelayCompensator(
        sourceGeneration,
        8,
        quantum,
        0);
    juce::AudioBuffer<float> initialTransition(2, 4);
    for (int sample = 0;
         sample < initialTransition.getNumSamples();
         ++sample)
    {
        const auto value =
            static_cast<float>(64 + sample) / 100.0f;
        initialTransition.setSample(0, sample, value);
        initialTransition.setSample(1, sample, value);
    }
    applyDelayCompensation(
        initialTransition,
        initialTransition.getNumSamples(),
        sourceGeneration);

    auto destinationGeneration = sourceGeneration;
    configureDelayCompensator(
        destinationGeneration,
        8,
        quantum,
        1);

    juce::AudioBuffer<float> sourceAdvance(2, 1);
    sourceAdvance.setSample(0, 0, 0.68f);
    sourceAdvance.setSample(1, 0, 0.68f);
    applyDelayCompensation(
        sourceAdvance,
        1,
        sourceGeneration);
    mergeDelayTransitionProgress(
        destinationGeneration,
        0);

    juce::AudioBuffer<float> destinationOutput(2, 1);
    destinationOutput.setSample(0, 0, 0.69f);
    destinationOutput.setSample(1, 0, 0.69f);
    applyDelayCompensation(
        destinationOutput,
        1,
        destinationGeneration);
    return destinationOutput.getSample(0, 0);
}
#endif

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

std::vector<StudioAudioEngine::PluginRuntimeStatus>
StudioAudioEngine::pluginRuntimeStatuses()
{
    const juce::ScopedLock graphLock(pluginRequestLock);
    const auto runtimeIndex = activePluginRuntime.load(
        std::memory_order_acquire);
    const auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
        runtimeIndex)];
    auto timingChanged = false;
    auto parameterLayoutChanged = false;
    const auto layoutDiffers = [](
        const auto& previous,
        const auto& current)
    {
        if (previous.size() != current.size())
            return true;
        for (std::size_t index = 0;
             index < previous.size();
             ++index)
        {
            if (previous[index].index != current[index].index
                || previous[index].id != current[index].id
                || previous[index].automatable
                    != current[index].automatable)
                return true;
        }
        return false;
    };
    {
        const juce::ScopedLock statusLock(pluginStatusLock);
        for (auto& status : pluginStatuses)
        {
            if (status.state != PluginRuntimeStatus::State::ready)
                continue;

            for (const auto& track : graph.tracks)
            {
                const auto insert = std::find_if(
                    track.inserts.cbegin(),
                    track.inserts.cend(),
                    [&status](const auto& candidate)
                    {
                        return candidate.insertId == status.insertId;
                    });
                if (insert == track.inserts.cend())
                    continue;
                if (insert->inProcess == nullptr
                    && (insert->bridge == nullptr
                        || !insert->bridge->isReady()))
                {
                    status.state = PluginRuntimeStatus::State::failed;
                    status.message = "Sandbox worker disconnected";
                    break;
                }

                const auto latency = insert->inProcess != nullptr
                    ? insert->inProcess->getLatencySamples()
                    : insert->bridge->reportedLatencySamples();
                const auto tail = insert->inProcess != nullptr
                    ? insert->inProcess->getTailLengthSeconds()
                    : insert->bridge->reportedTailSeconds();
                if (latency != status.latencySamples
                    || std::abs(tail - status.tailSeconds) > 0.000001)
                {
                    status.latencySamples = latency;
                    status.tailSeconds = tail;
                    const auto request = std::find_if(
                        activePluginRequests.begin(),
                        activePluginRequests.end(),
                        [&status](const auto& candidate)
                        {
                            return candidate.insertId == status.insertId;
                        });
                    if (request != activePluginRequests.end())
                    {
                        request->latencySamples = latency;
                        request->tailSeconds = tail;
                        timingChanged = true;
                    }
                }
                if (insert->inProcess != nullptr)
                {
                    std::vector<PluginParameterDescriptor> parameters;
                    const auto& hosted =
                        insert->inProcess->getParameters();
                    parameters.reserve(
                        static_cast<std::size_t>(hosted.size()));
                    for (int index = 0; index < hosted.size(); ++index)
                    {
                        const auto* parameter = hosted[index];
                        const auto* identified = dynamic_cast<
                            const juce::HostedAudioProcessorParameter*>(
                            parameter);
                        const auto* ranged = dynamic_cast<
                            const juce::RangedAudioParameter*>(
                            parameter);
                        parameters.push_back({
                            index,
                            identified != nullptr
                                ? identified->getParameterID()
                                : ranged != nullptr
                                ? ranged->paramID
                                : juce::String(index),
                            parameter->getName(128),
                            parameter->getValue(),
                            parameter->isAutomatable()
                        });
                    }
                    parameterLayoutChanged =
                        parameterLayoutChanged
                        || layoutDiffers(
                            status.parameters,
                            parameters);
                    status.parameters = std::move(parameters);
                }
                else if (insert->bridge != nullptr)
                {
                    auto parameters =
                        insert->bridge->parameterDescriptors();
                    parameterLayoutChanged =
                        parameterLayoutChanged
                        || layoutDiffers(
                            status.parameters,
                            parameters);
                    status.parameters = std::move(parameters);
                }
                break;
            }
        }
    }
    if (timingChanged || parameterLayoutChanged)
    {
        const juce::ScopedLock statusLock(pluginStatusLock);
        if (parameterLayoutChanged
            && activeSnapshotTemplate != nullptr)
        {
            resolvePluginAutomationParameterIds(
                *activeSnapshotTemplate,
                pluginStatuses);
        }
        if (timingChanged)
        {
            ++pluginSnapshotRefreshRevision;
            pluginSnapshotRefreshPending = true;
        }
        if (pluginSnapshotRefreshPending)
            startPluginSnapshotRefreshLocked();
    }
    else if (pluginSnapshotRefreshPending)
        startPluginSnapshotRefreshLocked();
    const juce::ScopedLock statusLock(pluginStatusLock);
    return pluginStatuses;
}

std::vector<StudioAudioEngine::TrackMeterSnapshot>
StudioAudioEngine::trackMeterSnapshots() const
{
    const juce::ScopedLock lock(meterLock);
    std::vector<TrackMeterSnapshot> result;
    for (const auto& slot : meterSlots)
    {
        if (slot.trackId.isEmpty())
            continue;
        result.push_back({
            slot.trackId,
            slot.preFaderLeft.load(std::memory_order_relaxed),
            slot.preFaderRight.load(std::memory_order_relaxed),
            slot.postFaderLeft.load(std::memory_order_relaxed),
            slot.postFaderRight.load(std::memory_order_relaxed)
        });
    }
    return result;
}

std::vector<StudioAudioEngine::PluginStateCapture>
StudioAudioEngine::capturePluginStates(int timeoutMilliseconds)
{
    const auto timeout = std::max(1, timeoutMilliseconds);
    std::vector<PluginStateCapture> captures;
    if (renderInProgress.load(std::memory_order_acquire))
    {
        PluginStateCapture capture;
        capture.result = juce::Result::fail(
            "Wait for the active render to finish before saving plugin state.");
        captures.push_back(std::move(capture));
        return captures;
    }
    auto expectedOperation = 0;
    if (!exclusiveAudioOperation.compare_exchange_strong(
            expectedOperation,
            2,
            std::memory_order_acq_rel))
    {
        PluginStateCapture capture;
        capture.result = juce::Result::fail(
            expectedOperation == 1
                ? "Stop and finalize recording before saving plugin state."
                : "Another plugin state operation is already running.");
        captures.push_back(std::move(capture));
        return captures;
    }
    if (isRecording()
        || activeRecorderCount.load(std::memory_order_acquire) > 0
        || recordingFinalizing.load(std::memory_order_acquire))
    {
        exclusiveAudioOperation.store(0, std::memory_order_release);
        PluginStateCapture capture;
        capture.result = juce::Result::fail(
            "Stop and finalize recording before saving plugin state.");
        captures.push_back(std::move(capture));
        return captures;
    }
    struct CaptureState
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<PluginStateCapture> captures;
        bool completed = false;
    };
    const auto state = std::make_shared<CaptureState>();
    {
        const juce::ScopedLock requestGuard(pluginRequestLock);
        if (pluginBuilderRunning
            || hasPendingPluginRequest
            || desiredPluginFingerprint != activePluginFingerprint
            || pluginStateCapturePending)
        {
            PluginStateCapture capture;
            capture.result = juce::Result::fail(
                "Wait for plugin loading to finish before saving.");
            captures.push_back(std::move(capture));
            exclusiveAudioOperation.store(
                0,
                std::memory_order_release);
            return captures;
        }
        pluginStateCapturePending = true;
        pluginRuntimeBuilder.addJob(
            [this, state, timeout]
            {
                auto runtimeCaptures =
                    capturePluginStatesOnRuntimeThread(timeout);
                auto closePendingEditors = false;
                {
                    const juce::ScopedLock completionGuard(
                        pluginRequestLock);
                    pluginStateCapturePending = false;
                    closePendingEditors =
                        pendingEditorCloseRequired;
                    pendingEditorCloseRequired = false;
                }
                if (closePendingEditors)
                    closeInProcessEditorsBeforeRuntimeChange();
                exclusiveAudioOperation.store(
                    0,
                    std::memory_order_release);
                {
                    const std::lock_guard stateGuard(state->mutex);
                    state->captures = std::move(runtimeCaptures);
                    state->completed = true;
                }
                state->condition.notify_one();
            });
    }

    const auto waitMilliseconds = static_cast<int>(
        std::clamp<std::int64_t>(
            static_cast<std::int64_t>(timeout)
                * static_cast<std::int64_t>(maximumRecordingTracks),
            5000,
            120000));
    auto completed = false;
#if JUCE_MODAL_LOOPS_PERMITTED
    if (auto* messages =
            juce::MessageManager::getInstanceWithoutCreating();
        messages != nullptr && messages->isThisTheMessageThread())
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(waitMilliseconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const std::lock_guard stateGuard(state->mutex);
                if (state->completed)
                {
                    completed = true;
                    break;
                }
            }
            messages->runDispatchLoopUntil(5);
        }
    }
    else
#endif
    {
        std::unique_lock stateLock(state->mutex);
        completed = state->condition.wait_for(
            stateLock,
            std::chrono::milliseconds(waitMilliseconds),
            [state] { return state->completed; });
    }
    if (!completed)
    {
        PluginStateCapture capture;
        capture.result = juce::Result::fail(
            "Plugin state capture timed out.");
        captures.push_back(std::move(capture));
        return captures;
    }
    return std::move(state->captures);
}

std::vector<StudioAudioEngine::PluginStateCapture>
StudioAudioEngine::capturePluginStatesOnRuntimeThread(
    int timeoutMilliseconds)
{
    const auto timeout = std::max(1, timeoutMilliseconds);
    pluginStateOperationActive.store(true, std::memory_order_release);
    struct StateOperationScope
    {
        ~StateOperationScope()
        {
            active.store(false, std::memory_order_release);
        }

        std::atomic<bool>& active;
    } stateOperationScope { pluginStateOperationActive };

    std::vector<PluginStateCapture> captures;
    if (!waitForPluginProcessingToStop(timeout))
    {
        PluginStateCapture capture;
        capture.result = juce::Result::fail(
            "Audio processing did not stop before plugin state capture.");
        captures.push_back(std::move(capture));
        return captures;
    }

    auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
        activePluginRuntime.load(std::memory_order_acquire))];
    for (auto& track : graph.tracks)
    {
        for (auto& insert : track.inserts)
        {
            PluginStateCapture capture;
            capture.trackId = track.trackId;
            capture.insertId = insert.insertId;
            capture.name = insert.name;
            if (insert.bridge != nullptr)
            {
                capture.result = insert.bridge->requestState(
                    capture.state,
                    std::chrono::milliseconds(timeout));
                capture.preservePreviousState =
                    capture.result.failed()
                    && !capture.result.getErrorMessage().startsWith(
                        "Plugin state save failed:");
            }
            else if (insert.inProcess != nullptr)
            {
                juce::MemoryBlock processorState;
                if (auto* validated =
                        dynamic_cast<ValidatedPluginStateTarget*>(
                            insert.inProcess.get()))
                {
                    capture.result =
                        validated->saveValidatedState(processorState);
                    if (capture.result.failed())
                    {
                        captures.push_back(std::move(capture));
                        continue;
                    }
                }
                else
                {
                    insert.inProcess->getStateInformation(
                        processorState);
                }
                if (insert.araDocument != nullptr)
                {
                    juce::MemoryBlock araState;
                    capture.result = insert.araDocument->archive(araState);
                    if (capture.result.failed())
                    {
                        captures.push_back(std::move(capture));
                        continue;
                    }
                    capture.state = AraDocumentHost::packState(
                        processorState,
                        araState);
                }
                else
                {
                    capture.state = std::move(processorState);
                }
            }
            else
            {
                capture.result = juce::Result::fail(
                    "The plugin runtime is unavailable.");
                capture.preservePreviousState = true;
            }
            captures.push_back(std::move(capture));
        }
    }
    return captures;
}

StudioAudioEngine::AraPreservationResult
StudioAudioEngine::preserveLiveAraStates(
    std::vector<PluginRuntimeRequest>& requests)
{
    const auto hasAraRequest = std::any_of(
        requests.cbegin(),
        requests.cend(),
        [](const auto& request)
        {
            return request.bridgeMode
                == PluginBridgeMode::araCompatibility;
        });
    if (!hasAraRequest)
        return {};
    auto expectedOperation = 0;
    if (!exclusiveAudioOperation.compare_exchange_strong(
            expectedOperation,
            2,
            std::memory_order_acq_rel))
    {
        return {
            expectedOperation == 1
                ? AraPreservationResult::Status::recordingBlocked
                : AraPreservationResult::Status::failed,
            expectedOperation == 1
                ? "Stop and finalize recording before rebuilding ARA processors."
                : "Another plugin state operation is already running."
        };
    }
    struct ExclusiveOperationScope
    {
        ~ExclusiveOperationScope()
        {
            operation.store(0, std::memory_order_release);
        }

        std::atomic<int>& operation;
    } exclusiveScope { exclusiveAudioOperation };
    if (recordingAccepting.load(std::memory_order_acquire)
        || activeRecorderCount.load(std::memory_order_acquire) > 0
        || recordingFinalizing.load(std::memory_order_acquire))
    {
        return {
            AraPreservationResult::Status::recordingBlocked,
            "Stop and finalize recording before rebuilding ARA processors."
        };
    }

    pluginStateOperationActive.store(true, std::memory_order_release);
    struct StateOperationScope
    {
        ~StateOperationScope()
        {
            active.store(false, std::memory_order_release);
        }

        std::atomic<bool>& active;
    } stateOperationScope { pluginStateOperationActive };

    if (!waitForPluginProcessingToStop(2000))
    {
        return {
            AraPreservationResult::Status::failed,
            "Audio processing did not stop before preserving ARA state."
        };
    }

    auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
        activePluginRuntime.load(std::memory_order_acquire))];
    for (auto& request : requests)
    {
        if (request.bridgeMode
            != PluginBridgeMode::araCompatibility)
        {
            continue;
        }
        for (auto& track : graph.tracks)
        {
            const auto insert = std::find_if(
                track.inserts.begin(),
                track.inserts.end(),
                [&request](const auto& candidate)
                {
                    return candidate.insertId == request.insertId;
                });
            if (insert == track.inserts.end()
                || insert->inProcess == nullptr
                || insert->araDocument == nullptr)
            {
                continue;
            }

            juce::MemoryBlock processorState;
            insert->inProcess->getStateInformation(processorState);
            juce::MemoryBlock araState;
            if (const auto result =
                    insert->araDocument->archive(araState);
                result.failed())
            {
                return {
                    AraPreservationResult::Status::failed,
                    insert->name
                        + ": could not preserve live ARA state: "
                        + result.getErrorMessage()
                };
            }
            request.state = AraDocumentHost::packState(
                processorState,
                araState);
            break;
        }
    }
    return {};
}

bool StudioAudioEngine::setPluginParameter(
    const juce::String& insertId,
    int parameterIndex,
    float normalizedValue,
    juce::String& error)
{
    if (renderInProgress.load(std::memory_order_acquire))
    {
        error = "Wait for the active render to finish.";
        return false;
    }
    if (!beginPluginControlOperation())
    {
        error = "Wait for plugin state processing to finish.";
        return false;
    }
    AtomicCounterScope controlScope {
        pluginControlOperationsInFlight
    };
    if (parameterIndex < 0)
    {
        error = "Plugin parameter index cannot be negative.";
        return false;
    }
    const auto value = juce::jlimit(0.0f, 1.0f, normalizedValue);
    {
        const juce::ScopedLock lock(pluginRequestLock);
        auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
            activePluginRuntime.load(std::memory_order_acquire))];
        for (auto& track : graph.tracks)
        {
            const auto insert = std::find_if(
                track.inserts.begin(),
                track.inserts.end(),
                [&insertId](const auto& candidate)
                {
                    return candidate.insertId == insertId;
                });
            if (insert == track.inserts.end())
                continue;
            if (insert->bridge != nullptr)
            {
                if (!insert->bridge->setParameter(parameterIndex, value))
                {
                    error = "Could not send the parameter to the plugin worker.";
                    return false;
                }
            }
            else if (insert->inProcess != nullptr
                     && parameterIndex
                         < insert->inProcess->getParameters().size())
            {
                insert->inProcess->getParameters()[parameterIndex]
                    ->setValueNotifyingHost(value);
            }
            else
            {
                error = "The plugin parameter is unavailable.";
                return false;
            }

            const juce::ScopedLock statusLock(pluginStatusLock);
            for (auto& status : pluginStatuses)
            {
                if (status.insertId != insertId)
                    continue;
                const auto parameter = std::find_if(
                    status.parameters.begin(),
                    status.parameters.end(),
                    [parameterIndex](const auto& candidate)
                    {
                        return candidate.index == parameterIndex;
                    });
                if (parameter != status.parameters.end())
                    parameter->value = value;
                break;
            }
            return true;
        }
    }
    error = "The plugin runtime is unavailable.";
    return false;
}

juce::Result StudioAudioEngine::showPluginEditor(
    const juce::String& insertId)
{
    if (renderInProgress.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Wait for the active render to finish.");
    if (!beginPluginControlOperation())
        return juce::Result::fail(
            "Wait for plugin state processing to finish.");
    AtomicCounterScope controlScope {
        pluginControlOperationsInFlight
    };
    std::shared_ptr<PluginBridgeClient> bridge;
    std::shared_ptr<juce::AudioProcessor> processor;
    auto editorName = juce::String();
    {
        const juce::ScopedLock lock(pluginRequestLock);
        auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
            activePluginRuntime.load(std::memory_order_acquire))];
        for (auto& track : graph.tracks)
        {
            const auto insert = std::find_if(
                track.inserts.begin(),
                track.inserts.end(),
                [&insertId](const auto& candidate)
                {
                    return candidate.insertId == insertId;
                });
            if (insert == track.inserts.end())
                continue;
            if (insert->bridge != nullptr)
                bridge = insert->bridge;
            else
                processor = insert->inProcess;
            editorName = insert->name;
            break;
        }
    }
    if (bridge != nullptr)
        return bridge->showEditor();
    if (processor == nullptr)
        return juce::Result::fail(
            "The plugin runtime is unavailable.");
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail(
            "In-process plugin editors must open on the message thread.");
    }

    const juce::ScopedLock editorGuard(editorLock);
    auto& session = editorSessions[insertId];
    if (session.processor != processor)
    {
        session.window.reset();
        session.processor.reset();
    }
    session.processor = processor;
    if (session.window == nullptr)
    {
        juce::String error;
        session.window = PluginEditorWindow::create(
            *processor,
            editorName,
            error);
        if (session.window == nullptr)
        {
            editorSessions.erase(insertId);
            return juce::Result::fail(error);
        }
    }
    session.window->showEditor();
    return juce::Result::ok();
}

juce::Result StudioAudioEngine::hidePluginEditor(
    const juce::String& insertId)
{
    if (renderInProgress.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Wait for the active render to finish.");
    if (!beginPluginControlOperation())
        return juce::Result::fail(
            "Wait for plugin state processing to finish.");
    AtomicCounterScope controlScope {
        pluginControlOperationsInFlight
    };
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        const juce::ScopedLock editorGuard(editorLock);
        const auto session = editorSessions.find(insertId);
        if (session != editorSessions.end())
        {
            session->second.window->hideEditor();
            return juce::Result::ok();
        }
    }

    std::shared_ptr<PluginBridgeClient> bridge;
    auto foundInsert = false;
    {
        const juce::ScopedLock lock(pluginRequestLock);
        auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
            activePluginRuntime.load(std::memory_order_acquire))];
        for (auto& track : graph.tracks)
        {
            const auto insert = std::find_if(
                track.inserts.begin(),
                track.inserts.end(),
                [&insertId](const auto& candidate)
                {
                    return candidate.insertId == insertId;
                });
            if (insert == track.inserts.end())
                continue;
            foundInsert = true;
            bridge = insert->bridge;
            break;
        }
    }
    if (bridge != nullptr)
        return bridge->hideEditor();
    return foundInsert
        ? juce::Result::ok()
        : juce::Result::fail("The plugin insert is unavailable.");
}

juce::Result StudioAudioEngine::focusPluginEditor(
    const juce::String& insertId)
{
    if (renderInProgress.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Wait for the active render to finish.");
    if (!beginPluginControlOperation())
        return juce::Result::fail(
            "Wait for plugin state processing to finish.");
    AtomicCounterScope controlScope {
        pluginControlOperationsInFlight
    };
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        const juce::ScopedLock editorGuard(editorLock);
        const auto session = editorSessions.find(insertId);
        if (session != editorSessions.end())
        {
            session->second.window->focusEditor();
            return juce::Result::ok();
        }
    }
    std::shared_ptr<PluginBridgeClient> bridge;
    {
        const juce::ScopedLock lock(pluginRequestLock);
        auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
            activePluginRuntime.load(std::memory_order_acquire))];
        for (auto& track : graph.tracks)
        {
            const auto insert = std::find_if(
                track.inserts.begin(),
                track.inserts.end(),
                [&insertId](const auto& candidate)
                {
                    return candidate.insertId == insertId;
                });
            if (insert == track.inserts.end())
                continue;
            bridge = insert->bridge;
            break;
        }
    }
    if (bridge != nullptr)
        return bridge->focusEditor();
    return showPluginEditor(insertId);
}

juce::Result StudioAudioEngine::resizePluginEditor(
    const juce::String& insertId,
    int width,
    int height)
{
    if (renderInProgress.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Wait for the active render to finish.");
    if (!beginPluginControlOperation())
        return juce::Result::fail(
            "Wait for plugin state processing to finish.");
    AtomicCounterScope controlScope {
        pluginControlOperationsInFlight
    };
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        const juce::ScopedLock editorGuard(editorLock);
        const auto session = editorSessions.find(insertId);
        if (session != editorSessions.end())
        {
            return session->second.window->resizeEditor(width, height)
                ? juce::Result::ok()
                : juce::Result::fail(
                      "Plugin editor dimensions must be positive.");
        }
    }

    std::shared_ptr<PluginBridgeClient> bridge;
    {
        const juce::ScopedLock lock(pluginRequestLock);
        auto& graph = pluginRuntimeGraphs[static_cast<std::size_t>(
            activePluginRuntime.load(std::memory_order_acquire))];
        for (auto& track : graph.tracks)
        {
            const auto insert = std::find_if(
                track.inserts.begin(),
                track.inserts.end(),
                [&insertId](const auto& candidate)
                {
                    return candidate.insertId == insertId;
                });
            if (insert != track.inserts.end())
            {
                bridge = insert->bridge;
                break;
            }
        }
    }
    return bridge != nullptr
        ? bridge->resizeEditor(width, height)
        : juce::Result::fail("The plugin editor is not open.");
}

std::uint64_t StudioAudioEngine::pluginLateBlockCount() const noexcept
{
    return pluginLateBlocks.load(std::memory_order_relaxed);
}

std::uint64_t
StudioAudioEngine::pluginAutomationEventDropCount() const noexcept
{
    return pluginAutomationEventsDropped.load(
        std::memory_order_relaxed);
}

bool StudioAudioEngine::pluginRuntimeTransitionPending() const
{
    const juce::ScopedLock lock(pluginRequestLock);
    return pluginBuilderRunning
        || hasPendingPluginRequest
        || pluginSnapshotRefreshPending
        || pluginSnapshotRefreshRunning
        || desiredPluginFingerprint != activePluginFingerprint;
}

juce::Result StudioAudioEngine::forcePluginRuntimeReload(
    const Project& project,
    std::vector<PluginRuntimeRequest> pluginRequests,
    juce::String insertId)
{
    return forcePluginRuntimeReloadInternal(
        project,
        std::move(pluginRequests),
        std::move(insertId),
        false);
}

juce::Result StudioAudioEngine::forcePluginRuntimeReloadInternal(
    const Project& project,
    std::vector<PluginRuntimeRequest> pluginRequests,
    juce::String insertId,
    bool renderOwned)
{
    juce::ignoreUnused(insertId);
    juce::String previousDesiredFingerprint;
    std::uint64_t previousDesiredGeneration = 0;
    const auto previousMetronome = metronomeEnabled.load(
        std::memory_order_acquire);
    {
        const juce::ScopedLock lock(pluginRequestLock);
        previousDesiredFingerprint = desiredPluginFingerprint;
        previousDesiredGeneration = desiredPluginGeneration;
        desiredPluginFingerprint.clear();
    }
    const auto result = updateProjectInternal(
        project,
        std::move(pluginRequests),
        renderOwned);
    if (result.failed())
    {
        metronomeEnabled.store(previousMetronome,
                               std::memory_order_release);
        const juce::ScopedLock lock(pluginRequestLock);
        if (desiredPluginFingerprint.isEmpty()
            && desiredPluginGeneration == previousDesiredGeneration)
        {
            desiredPluginFingerprint = previousDesiredFingerprint;
        }
    }
    return result;
}

juce::Result StudioAudioEngine::startRecording(const std::vector<RecordingRequest>& requests,
                                               const RecordingPlan& plan)
{
    if (renderInProgress.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Wait for the active render to finish before recording.");
    auto expectedOperation = 0;
    if (!exclusiveAudioOperation.compare_exchange_strong(
            expectedOperation,
            1,
            std::memory_order_acq_rel))
    {
        return juce::Result::fail(
            expectedOperation == 2
                ? "Wait for plugin state processing to finish before recording."
                : "A recording is already in progress.");
    }
    struct RecordingReservation
    {
        ~RecordingReservation()
        {
            if (!committed)
                operation.store(0, std::memory_order_release);
        }

        std::atomic<int>& operation;
        bool committed = false;
    } reservation { exclusiveAudioOperation };
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
    reservation.committed = true;
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
    exclusiveAudioOperation.store(0, std::memory_order_release);
    resumePendingPluginRuntime();
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
                                            double renderSampleRate,
                                            std::vector<PluginRuntimeRequest> pluginRequests)
{
    juce::AudioBuffer<float> rendered;
    if (const auto result = renderToBuffer(
            project,
            rendered,
            renderSampleRate,
            std::move(pluginRequests));
        result.failed())
        return result;

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

    if (!writer->writeFromAudioSampleBuffer(
            rendered,
            0,
            rendered.getNumSamples()))
        return juce::Result::fail(
            "The WAV export stopped while writing audio.");

    writer->flush();
    return juce::Result::ok();
}

juce::Result StudioAudioEngine::renderToBuffer(
    const Project& project,
    juce::AudioBuffer<float>& destination,
    double renderSampleRate,
    std::vector<PluginRuntimeRequest> pluginRequests)
{
    if (renderSampleRate <= 0.0)
        return juce::Result::fail("Render sample rate must be positive.");
    auto expectedRender = false;
    if (!renderInProgress.compare_exchange_strong(
            expectedRender,
            true,
            std::memory_order_acq_rel))
        return juce::Result::fail("Another render is already running.");
    struct RenderScope
    {
        ~RenderScope()
        {
            active.store(false, std::memory_order_release);
        }

        std::atomic<bool>& active;
    } renderScope { renderInProgress };
    if (project.hasActivePluginInserts() && pluginRequests.empty())
        return juce::Result::fail(
            "Buffer rendering cannot omit active plugins.");

    if (!pluginRequests.empty())
    {
        if (isRecording()
            || recordingFinalizing.load(std::memory_order_acquire))
            return juce::Result::fail(
                "Stop and finalize recording before rendering.");

        auto callbackSuspended = false;
        if (deviceManager != nullptr)
        {
            deviceManager->removeAudioCallback(this);
            callbackSuspended = true;
            for (int attempt = 0;
                 attempt < 1000
                 && audioCallbacksInFlight.load(
                        std::memory_order_acquire)
                     > 0;
                 ++attempt)
                juce::Thread::sleep(1);
            if (audioCallbacksInFlight.load(
                    std::memory_order_acquire) > 0)
            {
                deviceManager->addAudioCallback(this);
                return juce::Result::fail(
                    "Audio callback did not stop before rendering.");
            }
        }

        auto renderProject = project;
        renderProject.metronomeEnabled = false;
        for (auto& route : renderProject.routingConnections)
            if (route.kind == RouteKind::controlRoom)
                route.enabled = false;

        const auto previousSampleRate = sampleRate.load(
            std::memory_order_acquire);
        const auto previousBlockSize = deviceBlockSize.load(
            std::memory_order_acquire);
        const auto previousMetronome = metronomeEnabled.load(
            std::memory_order_acquire);
        sampleRate.store(renderSampleRate, std::memory_order_release);
        constexpr auto renderBlockSize = 512;
        deviceBlockSize.store(renderBlockSize, std::memory_order_release);
        metronomeEnabled.store(false, std::memory_order_release);
        playing.store(false, std::memory_order_release);
        playheadSample.store(0, std::memory_order_release);

        const auto restoreSettings = [&](bool reloadLiveProject)
            -> juce::Result
        {
            playing.store(false, std::memory_order_release);
            sampleRate.store(previousSampleRate, std::memory_order_release);
            deviceBlockSize.store(previousBlockSize,
                                  std::memory_order_release);
            metronomeEnabled.store(previousMetronome,
                                   std::memory_order_release);
            auto restoration = juce::Result::ok();
            if (reloadLiveProject)
            {
                if (const auto result = forcePluginRuntimeReloadInternal(
                        project,
                        pluginRequests,
                        {},
                        true);
                    result.failed())
                {
                    restoration = juce::Result::fail(
                        "Could not restore the live project: "
                        + result.getErrorMessage());
                }
                else
                {
                    if (!waitForPluginRuntimeTransition(10000))
                    {
                        restoration = juce::Result::fail(
                            "Timed out while restoring the live project.");
                    }
                }
                metronomeEnabled.store(previousMetronome,
                                       std::memory_order_release);
            }
            if (callbackSuspended && deviceManager != nullptr)
                deviceManager->addAudioCallback(this);
            return restoration;
        };
        const auto restoreAfterFailure = [&](const juce::Result& failure,
                                             bool reloadLiveProject)
        {
            if (const auto restore = restoreSettings(reloadLiveProject);
                restore.failed())
            {
                return juce::Result::fail(
                    failure.getErrorMessage()
                    + " Live audio restoration failed: "
                    + restore.getErrorMessage());
            }
            return failure;
        };

        if (const auto result = updateProjectInternal(
                renderProject,
                pluginRequests,
                true);
            result.failed())
        {
            return restoreAfterFailure(result, false);
        }
        if (!waitForPluginRuntimeTransition(10000))
        {
            return restoreAfterFailure(
                juce::Result::fail(
                    "Timed out while preparing processors for rendering."),
                true);
        }
        const auto renderStatuses = pluginRuntimeStatuses();
        for (const auto& status : renderStatuses)
        {
            if (status.state == PluginRuntimeStatus::State::failed
                || status.state == PluginRuntimeStatus::State::missing)
            {
                return restoreAfterFailure(
                    juce::Result::fail(
                        status.name + ": " + status.message),
                    true);
            }
        }
        if (!waitForPluginRuntimeTransition(10000))
        {
            return restoreAfterFailure(
                    juce::Result::fail(
                        "Timed out while refreshing processor timing for rendering."),
                    true);
        }

        const auto snapshotIndex = activeSnapshot.load(
            std::memory_order_acquire);
        const auto length = snapshots[static_cast<std::size_t>(
            snapshotIndex)].lengthSamples;
        if (length <= 0
            || length
                > static_cast<std::int64_t>(
                    std::numeric_limits<int>::max()))
        {
            return restoreAfterFailure(
                juce::Result::fail(
                    "Rendered project is too long for a memory buffer."),
                true);
        }
        destination.setSize(
            2,
            static_cast<int>(length),
            false,
            true,
            false);
        destination.clear();

        const auto realtime = std::any_of(
            pluginRequests.cbegin(),
            pluginRequests.cend(),
            [](const auto& request)
            {
                return !request.bypassed
                    && !request.missing
                    && request.deviceIdentifier.isEmpty()
                    && request.bridgeMode == PluginBridgeMode::sandboxed;
            });
        juce::AudioBuffer<float> block(2, renderBlockSize);
        juce::AudioIODeviceCallbackContext context;
        playing.store(true, std::memory_order_release);
        auto offset = 0;
        while (offset < destination.getNumSamples())
        {
            const auto samples = std::min(
                renderBlockSize,
                destination.getNumSamples() - offset);
            block.clear();
            float* outputs[] {
                block.getWritePointer(0),
                block.getWritePointer(1)
            };
            audioDeviceIOCallbackWithContext(
                nullptr,
                0,
                outputs,
                2,
                samples,
                context);
            destination.copyFrom(0, offset, block, 0, 0, samples);
            destination.copyFrom(1, offset, block, 1, 0, samples);
            offset += samples;
            if (realtime)
            {
                juce::Thread::sleep(
                    std::max(
                        1,
                        static_cast<int>(
                            std::ceil(
                                static_cast<double>(samples)
                                * 1000.0
                                / renderSampleRate))));
            }
        }
        if (const auto restore = restoreSettings(true); restore.failed())
            return juce::Result::fail(
                "Render completed, but live audio restoration failed: "
                + restore.getErrorMessage());
        return juce::Result::ok();
    }

    juce::String error;
    auto snapshot = buildSnapshot(project, renderSampleRate, {}, error);
    if (!snapshot.has_value())
        return juce::Result::fail(error);
    if (snapshot->lengthSamples <= 0
        || snapshot->lengthSamples
            > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
        return juce::Result::fail("Rendered project is too long for a memory buffer.");

    destination.setSize(
        2,
        static_cast<int>(snapshot->lengthSamples),
        false,
        true,
        false);
    destination.clear();
    for (int sample = 0; sample < destination.getNumSamples(); ++sample)
    {
        float left = 0.0f;
        float right = 0.0f;
        mixSample(*snapshot, sample, left, right);
        destination.setSample(0, sample, left);
        destination.setSample(1, sample, right);
    }
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

    const auto compiledRouting = RoutingGraphCompiler::compile(project, error);
    if (!compiledRouting.has_value())
        return std::nullopt;
    std::vector<juce::String> routingOrder;
    routingOrder.reserve(compiledRouting->tracks.size());
    for (const auto& track : compiledRouting->tracks)
        routingOrder.push_back(track.id);
    const auto masterId = project.masterTrackId();

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

    for (const auto& trackId : routingOrder)
    {
        const auto* parentPointer = project.findTrack(trackId);
        if (parentPointer == nullptr)
        {
            error = "A routed track became unavailable.";
            return std::nullopt;
        }
        const auto& parent = *parentPointer;
        const auto* compiledTrack = compiledRouting->findTrack(parent.id);
        if (compiledTrack == nullptr)
        {
            error = "A compiled routing track became unavailable.";
            return std::nullopt;
        }
        const auto wholeContentSoloActive = compiledTrack->processing;
        const auto trackSoloActive = compiledTrack->audible;

        RenderTrack renderTrack;
        renderTrack.runtimeKey = runtimeKey(parent.id);
        renderTrack.volumeGain = juce::Decibels::decibelsToGain(parent.volumeDecibels);
        renderTrack.vcaGain = compiledTrack->vcaGain;
        renderTrack.pan = parent.pan;
        renderTrack.polarityInverted = compiledTrack->polarityInverted;
        renderTrack.audible = !parent.muted && trackSoloActive;
        renderTrack.processing = !parent.muted && wholeContentSoloActive;
        renderTrack.meterIndex = meterSlotFor(parent.id);
        const auto anySoloedChild = std::any_of(
            project.tracks.cbegin(),
            project.tracks.cend(),
            [&parent](const auto& child)
            {
                return child.parentTrackId == parent.id
                    && child.solo;
            });

        auto parentClips = buildClips(parent, nullptr);
        if (!parentClips.has_value())
            return std::nullopt;

        RenderSource parentSource;
        parentSource.trackId = parent.id;
        parentSource.isParentContent = true;
        parentSource.audible = renderTrack.processing
            && !anySoloedChild;
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
            const auto selectedByExpanded = !parent.versionsCollapsed;
            if (!selectedByExpanded
                && !selectedByComp
                && !selectedByPlaylist
                && !selectedBySolo)
                continue;

            auto childClips = buildClips(
                child,
                !selectedByExpanded
                        && selectedByComp
                        && !selectedBySolo
                    ? &parent.compRegions
                    : nullptr);
            if (!childClips.has_value())
                return std::nullopt;

            RenderSource source;
            source.runtimeKey = runtimeKey(child.id);
            source.trackId = child.id;
            source.volumeGain = juce::Decibels::decibelsToGain(child.volumeDecibels);
            source.pan = child.pan;
            source.audible = !parent.muted
                && !child.muted
                && (anySoloedChild
                        ? child.solo
                        : wholeContentSoloActive || child.solo);
            source.clips = std::move(*childClips);
            renderTrack.sources.push_back(std::move(source));
        }

        snapshot.tracks.push_back(std::move(renderTrack));
    }

    for (std::size_t index = 0; index < snapshot.tracks.size(); ++index)
    {
        const auto* source = project.findTrack(routingOrder[index]);
        if (source == nullptr)
            continue;

        auto destinationId = project.resolvedOutputTrackId(*source);
        if (destinationId != masterId)
        {
            const auto destination = std::find(routingOrder.cbegin(),
                                               routingOrder.cend(),
                                               destinationId);
            if (destination == routingOrder.cend())
            {
                error = "A routed bus became unavailable.";
                return std::nullopt;
            }
            snapshot.tracks[index].destinationIndex = static_cast<int>(
                std::distance(routingOrder.cbegin(), destination));
        }

        while (destinationId.isNotEmpty() && destinationId != masterId)
        {
            const auto* destination = project.findTrack(destinationId);
            if (destination == nullptr)
                break;
            const auto destinationPosition = std::find(routingOrder.cbegin(),
                                                       routingOrder.cend(),
                                                       destinationId);
            if (destinationPosition == routingOrder.cend())
                break;
            const auto destinationIndex = static_cast<std::size_t>(
                std::distance(routingOrder.cbegin(), destinationPosition));
            const auto gain = juce::Decibels::decibelsToGain(
                destination->volumeDecibels);
            snapshot.tracks[index].offlineRoutingGainLeft *= gain
                * (destination->pan > 0.0f ? 1.0f - destination->pan : 1.0f);
            snapshot.tracks[index].offlineRoutingGainRight *= gain
                * (destination->pan < 0.0f ? 1.0f + destination->pan : 1.0f);
            snapshot.tracks[index].offlineRouteAudible =
                snapshot.tracks[index].offlineRouteAudible
                && snapshot.tracks[destinationIndex].audible;
            destinationId = project.resolvedOutputTrackId(*destination);
        }
    }

    for (const auto& compiledRoute : compiledRouting->routes)
    {
        if (compiledRoute.kind == RouteKind::mainOutput
            || compiledRoute.sourceTrackIndex < 0
            || compiledRoute.sourceTrackIndex
                >= static_cast<int>(snapshot.tracks.size()))
            continue;

        RenderTrack::Route route;
        route.id = compiledRoute.id;
        route.kind = compiledRoute.kind;
        route.tap = compiledRoute.tap;
        route.destinationIndex = compiledRoute.destinationTrackIndex;
        route.destinationInsertId = compiledRoute.destinationInsertId;
        route.hardwareFirstChannel = compiledRoute.hardwareFirstChannel;
        route.hardwareChannels = compiledRoute.hardwareChannels;
        route.gain = compiledRoute.gain;
        route.pan = compiledRoute.pan;
        snapshot.tracks[static_cast<std::size_t>(
            compiledRoute.sourceTrackIndex)].routes.push_back(std::move(route));
    }

    if (const auto controlRoom = std::find_if(
            project.tracks.cbegin(),
            project.tracks.cend(),
            [](const auto& track)
            {
                return track.type == TrackType::controlRoom
                    && track.parentTrackId.isEmpty();
            });
        controlRoom != project.tracks.cend())
    {
        const auto route = std::find_if(
            project.routingConnections.cbegin(),
            project.routingConnections.cend(),
            [&masterId, controlRoom](const auto& connection)
            {
                return connection.enabled
                    && connection.kind == RouteKind::controlRoom
                    && connection.sourceTrackId == masterId
                    && connection.destination.trackId == controlRoom->id;
            });
        snapshot.controlRoomEnabled = route != project.routingConnections.cend();
        snapshot.controlRoomRuntimeKey = runtimeKey(controlRoom->id);
        snapshot.controlRoomGain = juce::Decibels::decibelsToGain(
            controlRoom->volumeDecibels);
        snapshot.controlRoomPan = controlRoom->pan;
        snapshot.controlRoomOutputChannel = controlRoom->hardwareOutputChannel;
        snapshot.controlRoomMuted = controlRoom->muted;
        snapshot.controlRoomDimmed = controlRoom->controlRoomDimmed;
        snapshot.controlRoomDimGain = juce::Decibels::decibelsToGain(
            controlRoom->controlRoomDimDecibels);
        snapshot.controlRoomMono = controlRoom->controlRoomMono;
    }
    snapshot.offlineTrackLeft.resize(snapshot.tracks.size(), 0.0f);
    snapshot.offlineTrackRight.resize(snapshot.tracks.size(), 0.0f);

    for (const auto& lane : project.automationLanes)
    {
        if (!lane.enabled)
            continue;
        auto compiled = AutomationScheduler::compile(
            project,
            lane,
            targetSampleRate,
            error);
        if (!compiled.has_value())
            return std::nullopt;

        if (lane.target.trackId == masterId)
        {
            switch (lane.target.type)
            {
                case AutomationTargetType::trackVolume:
                    snapshot.masterVolumeAutomation = *compiled;
                    break;
                case AutomationTargetType::trackPan:
                    snapshot.masterPanAutomation = *compiled;
                    break;
                case AutomationTargetType::trackMute:
                    snapshot.masterMuteAutomation = *compiled;
                    break;
                case AutomationTargetType::trackPolarity:
                case AutomationTargetType::vcaVolume:
                case AutomationTargetType::sendGain:
                case AutomationTargetType::sendPan:
                case AutomationTargetType::sendMute:
                case AutomationTargetType::controlRoomDim:
                    break;
                case AutomationTargetType::pluginParameter:
                case AutomationTargetType::deviceParameter:
                    snapshot.masterPluginAutomation.push_back({
                        lane.target.insertId,
                        lane.target.parameterId,
                        lane.target.parameterIndex,
                        *compiled
                    });
                    break;
            }
            continue;
        }

        const auto trackPosition = std::find(
            routingOrder.cbegin(),
            routingOrder.cend(),
            lane.target.trackId);
        if (lane.target.type == AutomationTargetType::vcaVolume)
        {
            const auto* vca = project.findTrack(lane.target.trackId);
            if (vca == nullptr || vca->type != TrackType::vca)
            {
                error = "VCA automation targets an unavailable VCA.";
                return std::nullopt;
            }
            for (const auto& controlledId : vca->controlledTrackIds)
            {
                const auto controlled = std::find(
                    routingOrder.cbegin(),
                    routingOrder.cend(),
                    controlledId);
                if (controlled != routingOrder.cend())
                {
                    snapshot.tracks[static_cast<std::size_t>(
                        std::distance(routingOrder.cbegin(), controlled))]
                        .vcaAutomation = *compiled;
                }
            }
            continue;
        }
        if (trackPosition == routingOrder.cend())
        {
            if (lane.target.type == AutomationTargetType::pluginParameter
                || lane.target.type
                    == AutomationTargetType::deviceParameter)
            {
                for (auto& renderTrack : snapshot.tracks)
                {
                    const auto source = std::find_if(
                        renderTrack.sources.begin(),
                        renderTrack.sources.end(),
                        [&lane](const auto& candidate)
                        {
                            return candidate.trackId
                                == lane.target.trackId;
                        });
                    if (source != renderTrack.sources.end())
                    {
                        source->pluginAutomation.push_back({
                            lane.target.insertId,
                            lane.target.parameterId,
                            lane.target.parameterIndex,
                            *compiled
                        });
                        break;
                    }
                }
            }
            continue;
        }

        auto& renderTrack = snapshot.tracks[static_cast<std::size_t>(
            std::distance(routingOrder.cbegin(), trackPosition))];
        switch (lane.target.type)
        {
            case AutomationTargetType::trackVolume:
                renderTrack.volumeAutomation = *compiled;
                break;
            case AutomationTargetType::trackPan:
                renderTrack.panAutomation = *compiled;
                break;
            case AutomationTargetType::trackMute:
                renderTrack.muteAutomation = *compiled;
                break;
            case AutomationTargetType::trackPolarity:
                renderTrack.polarityAutomation = *compiled;
                break;
            case AutomationTargetType::sendGain:
            case AutomationTargetType::sendPan:
            case AutomationTargetType::sendMute:
            {
                const auto route = std::find_if(
                    renderTrack.routes.begin(),
                    renderTrack.routes.end(),
                    [&lane](const auto& candidate)
                    {
                        return candidate.id == lane.target.routeId;
                    });
                if (route == renderTrack.routes.end())
                {
                    error = "Route automation target is unavailable.";
                    return std::nullopt;
                }
                if (lane.target.type == AutomationTargetType::sendGain)
                    route->gainAutomation = *compiled;
                else if (lane.target.type == AutomationTargetType::sendPan)
                    route->panAutomation = *compiled;
                else
                    route->muteAutomation = *compiled;
                break;
            }
            case AutomationTargetType::vcaVolume:
            case AutomationTargetType::controlRoomDim:
                break;
            case AutomationTargetType::pluginParameter:
            case AutomationTargetType::deviceParameter:
                renderTrack.pluginAutomation.push_back({
                    lane.target.insertId,
                    lane.target.parameterId,
                    lane.target.parameterIndex,
                    *compiled
                });
                break;
        }
    }

    configureRuntimeTiming(snapshot, pluginRequests);

    return snapshot;
}

void StudioAudioEngine::resolvePluginAutomationParameterIds(
    const RenderSnapshot& snapshot,
    const std::vector<PluginRuntimeStatus>& statuses) const
{
    const auto resolve = [&statuses](
        std::uint64_t key,
        auto& lanes)
    {
        for (auto& lane : lanes)
        {
            if (lane.parameterId.isEmpty())
                continue;
            const auto status = std::find_if(
                statuses.begin(),
                statuses.end(),
                [key, &lane](const auto& candidate)
                {
                    return runtimeKey(candidate.trackId) == key
                        && candidate.insertId == lane.insertId;
                });
            if (status == statuses.end())
            {
                lane.resolveParameterIndex(-1);
                continue;
            }

            lane.resolveParameterIndex(-1);
            for (const auto& parameter : status->parameters)
            {
                if (parameter.automatable
                    && parameter.id == lane.parameterId)
                {
                    lane.resolveParameterIndex(
                        parameter.index);
                    break;
                }
            }
        }
    };

    for (auto& track : snapshot.tracks)
    {
        resolve(track.runtimeKey, track.pluginAutomation);
        for (auto& source : track.sources)
            resolve(source.runtimeKey, source.pluginAutomation);
    }
    resolve(
        snapshot.masterRuntimeKey,
        snapshot.masterPluginAutomation);
}

void StudioAudioEngine::startPluginSnapshotRefreshLocked()
{
    if (!pluginSnapshotRefreshPending
        || pluginSnapshotRefreshRunning
        || activeSnapshotTemplate == nullptr)
        return;

    pluginSnapshotRefreshRunning = true;
    const auto snapshotTemplate = activeSnapshotTemplate;
    const auto requests = activePluginRequests;
    const auto generation = activePluginGeneration;
    const auto runtimeIndex = activePluginRuntime.load(
        std::memory_order_acquire);
    const auto snapshotRevision = activeSnapshotRevision;
    const auto refreshRevision =
        pluginSnapshotRefreshRevision;
    pluginRuntimeBuilder.addJob(
        [this,
         snapshotTemplate,
         requests,
         generation,
         runtimeIndex,
         snapshotRevision,
         refreshRevision]() mutable
        {
            if (shuttingDown.load(std::memory_order_acquire))
            {
                const juce::ScopedLock lock(pluginRequestLock);
                pluginSnapshotRefreshRunning = false;
                return;
            }

            auto refreshed = *snapshotTemplate;
            const juce::ScopedLock lock(pluginRequestLock);
            if (snapshotRevision == activeSnapshotRevision
                && refreshRevision
                    == pluginSnapshotRefreshRevision
                && snapshotTemplate == activeSnapshotTemplate)
            {
                const auto destination =
                    chooseWritableSnapshot();
                const auto sourceSnapshotIndex =
                    static_cast<int>(
                        (activeRenderPair.load(
                             std::memory_order_acquire)
                         >> 2)
                        & 0x3);
                configureRuntimeTiming(
                    refreshed,
                    requests,
                    destination);
                mergeSnapshotTransitionHandoffs(
                    refreshed,
                    sourceSnapshotIndex,
                    -1);
                const auto refreshedTemplate =
                    std::make_shared<RenderSnapshot>(refreshed);
                activeSnapshotTemplate = refreshedTemplate;
                ++activeSnapshotRevision;
                snapshots[static_cast<std::size_t>(destination)] =
                    std::move(refreshed);
                snapshotGenerations[
                    static_cast<std::size_t>(destination)].store(
                    generation,
                    std::memory_order_release);
                activeSnapshot.store(
                    destination,
                    std::memory_order_release);
                activeRenderPair.store(
                    renderPair(
                        generation,
                        destination,
                        runtimeIndex),
                    std::memory_order_release);
                pluginSnapshotRefreshPending = false;
            }
            pluginSnapshotRefreshRunning = false;
            if (pluginSnapshotRefreshPending
                && !shuttingDown.load(std::memory_order_acquire))
                startPluginSnapshotRefreshLocked();
        });
}

void StudioAudioEngine::configureDelayCompensator(
    RenderSource::DelayCompensator& delay,
    int samples,
    int processingQuantum,
    int transitionProgressSlot)
{
    const auto previousDelay = delay.delaySamples;
    const auto nextDelay = std::clamp(
        samples,
        0,
        std::numeric_limits<int>::max() - 1);
    const auto requiredCapacity = nextDelay + 1;
    if (delay.state != nullptr
        && previousDelay == nextDelay)
    {
        if (transitionProgressSlot >= 0
            && delay.transition.has_value())
        {
            const auto nextSlot = std::clamp(
                transitionProgressSlot,
                0,
                2);
            if (nextSlot
                != delay.transition->positionSlot)
            {
                const auto previousSlot =
                    delay.transition->positionSlot;
                auto sourceMask =
                    delay.state->transitionHandoffMasks[
                        static_cast<std::size_t>(
                            previousSlot)].load(
                        std::memory_order_acquire)
                    | (1 << previousSlot);
                auto position =
                    delay.state->transitionPositions[
                        static_cast<std::size_t>(
                            previousSlot)].load(
                        std::memory_order_acquire);
                for (auto slot = 0; slot < 3; ++slot)
                {
                    if ((sourceMask & (1 << slot)) == 0)
                        continue;
                    position = std::max(
                        position,
                        delay.state->transitionPositions[
                            static_cast<std::size_t>(
                                slot)].load(
                            std::memory_order_acquire));
                }
                delay.state->transitionPositions[
                    static_cast<std::size_t>(
                        nextSlot)].store(
                    position,
                    std::memory_order_release);
                sourceMask &= ~(1 << nextSlot);
                delay.state->transitionHandoffMasks[
                    static_cast<std::size_t>(nextSlot)].store(
                    sourceMask,
                    std::memory_order_release);
                delay.transition->positionSlot = nextSlot;
            }
        }
        return;
    }

    auto targetState = delay.state;
    if (targetState == nullptr
        || targetState->buffer.getNumSamples()
            < requiredCapacity)
    {
        targetState = std::make_shared<
            RenderSource::DelayCompensator::State>();
        const auto headroom = std::max<std::int64_t>(
            static_cast<std::int64_t>(
                processingQuantum)
                * 4,
            nextDelay);
        const auto allocatedCapacity =
            static_cast<int>(std::min<std::int64_t>(
                std::numeric_limits<int>::max(),
                static_cast<std::int64_t>(
                    requiredCapacity)
                    + headroom));
        targetState->buffer.setSize(
            2,
            std::max(1, allocatedCapacity),
            false,
            true,
            false);
        targetState->buffer.clear();
    }

    std::optional<
        RenderSource::DelayCompensator::Transition>
        transition;
    if (delay.state != nullptr
        && delay.state->buffer.getNumSamples() > 0)
    {
        transition.emplace();
        transition->sourceState = delay.state;
        transition->sourceDelaySamples = previousDelay;
        const auto availableHistory =
            targetState == delay.state
            ? std::min<std::int64_t>(
                  targetState->buffer.getNumSamples() - 1,
                  targetState->samplesWritten.load(
                      std::memory_order_acquire))
            : 0;
        transition->warmupSamples = std::max(
            0,
            nextDelay - static_cast<int>(availableHistory));
        transition->fadeSamples = std::max(
            1,
            processingQuantum);
        transition->positionSlot = transitionProgressSlot >= 0
            ? std::clamp(transitionProgressSlot, 0, 2)
            : targetState == delay.state
                  && delay.transition.has_value()
            ? (delay.transition->positionSlot + 1) % 3
            : 0;
        targetState->transitionPositions[
            static_cast<std::size_t>(
                transition->positionSlot)].store(
            0,
            std::memory_order_release);
        targetState->transitionHandoffMasks[
            static_cast<std::size_t>(
                transition->positionSlot)].store(
            0,
            std::memory_order_release);
    }
    delay.delaySamples = nextDelay;
    delay.state = std::move(targetState);
    delay.transition = std::move(transition);
}

void StudioAudioEngine::configureRuntimeTiming(
    RenderSnapshot& snapshot,
    const std::vector<PluginRuntimeRequest>& pluginRequests,
    int transitionProgressSlot) const
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
                || (!request.description.has_value()
                    && request.deviceIdentifier.isEmpty()))
                return total;

            return total
                + (request.bridgeMode == PluginBridgeMode::sandboxed
                       ? snapshot.processingQuantum
                       : 0)
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
                || (!request.description.has_value()
                    && request.deviceIdentifier.isEmpty()))
                return total;
            return total + std::max(0.0, request.tailSeconds);
        });
    };
    std::vector<int> inputLatencies(snapshot.tracks.size(), 0);
    std::vector<double> inputTails(snapshot.tracks.size(), 0.0);
    std::vector<double> outputTails(snapshot.tracks.size(), 0.0);
    auto masterInputLatency = 0;
    auto masterInputTail = 0.0;
    for (std::size_t index = 0; index < snapshot.tracks.size(); ++index)
    {
        auto& track = snapshot.tracks[index];
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

        const auto alignedInputLatency = std::max(maximumSourceLatency,
                                                 inputLatencies[index]);
        for (auto& source : track.sources)
            configureDelayCompensator(
                source.compensation,
                alignedInputLatency - source.runtimeLatencySamples,
                snapshot.processingQuantum,
                transitionProgressSlot);

        track.runtimeLatencySamples = alignedInputLatency
            + latencyForKey(track.runtimeKey);
        outputTails[index] = std::max(maximumSourceTail, inputTails[index])
            + tailForKey(track.runtimeKey);
        const auto propagateTo = [&](int destinationIndex)
        {
            if (destinationIndex < 0)
                return;
            const auto destination = static_cast<std::size_t>(destinationIndex);
            inputLatencies[destination] = std::max(inputLatencies[destination],
                                                  track.runtimeLatencySamples);
            inputTails[destination] = std::max(inputTails[destination],
                                              outputTails[index]);
        };
        if (track.destinationIndex >= 0)
            propagateTo(track.destinationIndex);
        else
        {
            masterInputLatency = std::max(masterInputLatency,
                                          track.runtimeLatencySamples);
            masterInputTail = std::max(masterInputTail, outputTails[index]);
        }
        for (const auto& route : track.routes)
            if (route.kind == RouteKind::send
                || route.kind == RouteKind::sidechain)
                propagateTo(route.destinationIndex);
    }

    for (auto& track : snapshot.tracks)
    {
        const auto destinationLatency = track.destinationIndex >= 0
            ? inputLatencies[static_cast<std::size_t>(track.destinationIndex)]
            : masterInputLatency;
        configureDelayCompensator(
            track.compensation,
            destinationLatency - track.runtimeLatencySamples,
            snapshot.processingQuantum,
            transitionProgressSlot);
        for (auto& route : track.routes)
        {
            const auto routeDestinationLatency = route.destinationIndex >= 0
                ? inputLatencies[static_cast<std::size_t>(
                      route.destinationIndex)]
                : track.runtimeLatencySamples;
            configureDelayCompensator(
                route.compensation,
                routeDestinationLatency - track.runtimeLatencySamples,
                snapshot.processingQuantum,
                transitionProgressSlot);
            route.processingBuffer.clear();
        }
    }

    const auto masterLatency = latencyForKey(snapshot.masterRuntimeKey);
    configureDelayCompensator(
        snapshot.clickCompensation,
        masterInputLatency + masterLatency,
        snapshot.processingQuantum,
        transitionProgressSlot);
    snapshot.lengthSamples = snapshot.contentLengthSamples
        + masterInputLatency
        + masterLatency
        + static_cast<std::int64_t>(
            std::ceil((masterInputTail + tailForKey(snapshot.masterRuntimeKey))
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
            stretch.presetCheaper(channels,
                                  static_cast<float>(targetSampleRate));
        else
            stretch.presetDefault(channels,
                                  static_cast<float>(targetSampleRate));
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

void StudioAudioEngine::mixSample(RenderSnapshot& snapshot,
                                  std::int64_t timelineSample,
                                  float& left,
                                  float& right) noexcept
{
    if (!snapshot.masterAudible)
        return;

    std::fill(snapshot.offlineTrackLeft.begin(),
              snapshot.offlineTrackLeft.end(),
              0.0f);
    std::fill(snapshot.offlineTrackRight.begin(),
              snapshot.offlineTrackRight.end(),
              0.0f);
    auto masterLeft = 0.0f;
    auto masterRight = 0.0f;

    const auto routeSample = [&snapshot, timelineSample](
                                 const RenderTrack& track,
                                 RouteTap tap,
                                 float sourceLeft,
                                 float sourceRight)
    {
        if (!track.audible)
            return;
        for (const auto& route : track.routes)
        {
            if (route.kind != RouteKind::send
                || route.tap != tap
                || route.destinationIndex < 0)
                continue;
            if (automatedSwitch(
                    route.muteAutomation,
                    timelineSample,
                    false))
                continue;
            const auto gain = automatedVolumeGain(
                route.gainAutomation,
                timelineSample,
                route.gain);
            const auto pan = automatedPan(
                route.panAutomation,
                timelineSample,
                route.pan);
            const auto leftGain = gain
                * (pan > 0.0f ? 1.0f - pan : 1.0f);
            const auto rightGain = gain
                * (pan < 0.0f ? 1.0f + pan : 1.0f);
            const auto destination = static_cast<std::size_t>(
                route.destinationIndex);
            snapshot.offlineTrackLeft[destination] += sourceLeft * leftGain;
            snapshot.offlineTrackRight[destination] += sourceRight * rightGain;
        }
    };

    for (std::size_t trackIndex = 0;
         trackIndex < snapshot.tracks.size();
         ++trackIndex)
    {
        const auto& track = snapshot.tracks[trackIndex];
        if (!track.processing)
            continue;

        auto trackLeft = snapshot.offlineTrackLeft[trackIndex];
        auto trackRight = snapshot.offlineTrackRight[trackIndex];
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

        routeSample(track,
                    RouteTap::preFader,
                    trackLeft,
                    trackRight);
        const auto pan = automatedPan(
            track.panAutomation,
            timelineSample,
            track.pan);
        const auto leftPanGain = pan > 0.0f ? 1.0f - pan : 1.0f;
        const auto rightPanGain = pan < 0.0f ? 1.0f + pan : 1.0f;
        const auto polarity = automatedSwitch(
            track.polarityAutomation,
            timelineSample,
            track.polarityInverted)
            ? -1.0f
            : 1.0f;
        const auto trackGain = automatedVolumeGain(
            track.volumeAutomation,
            timelineSample,
            track.volumeGain);
        const auto vcaGain = automatedVolumeGain(
            track.vcaAutomation,
            timelineSample,
            track.vcaGain);
        trackLeft *= trackGain
            * vcaGain
            * polarity
            * leftPanGain;
        trackRight *= trackGain
            * vcaGain
            * polarity
            * rightPanGain;
        const auto trackMuted = !track.audible
            || automatedSwitch(
                   track.muteAutomation,
                   timelineSample,
                   false);
        if (!trackMuted)
        {
            routeSample(track,
                        RouteTap::postFader,
                        trackLeft,
                        trackRight);
        }
        if (trackMuted)
            continue;
        if (track.destinationIndex >= 0)
        {
            const auto destination = static_cast<std::size_t>(
                track.destinationIndex);
            snapshot.offlineTrackLeft[destination] += trackLeft;
            snapshot.offlineTrackRight[destination] += trackRight;
        }
        else
        {
            masterLeft += trackLeft;
            masterRight += trackRight;
        }
    }

    if (automatedSwitch(
            snapshot.masterMuteAutomation,
            timelineSample,
            false))
        return;
    const auto masterGain = automatedVolumeGain(
        snapshot.masterVolumeAutomation,
        timelineSample,
        snapshot.masterGain);
    const auto masterPan = automatedPan(
        snapshot.masterPanAutomation,
        timelineSample,
        snapshot.masterPan);
    const auto masterLeftGain = masterGain
        * (masterPan > 0.0f ? 1.0f - masterPan : 1.0f);
    const auto masterRightGain = masterGain
        * (masterPan < 0.0f ? 1.0f + masterPan : 1.0f);
    left += masterLeft * masterLeftGain;
    right += masterRight * masterRightGain;
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
        if (loopEnabled)
            position = wrapLoopPosition(
                position,
                loopStartSample,
                loopEndSample);
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

int StudioAudioEngine::mergeDelayTransitionProgress(
    RenderSource::DelayCompensator& delay,
    int sourceFilter) noexcept
{
    if (delay.state == nullptr
        || !delay.transition.has_value())
        return 0;

    const auto destinationSlot =
        delay.transition->positionSlot;
    if (destinationSlot < 0 || destinationSlot >= 3)
    {
        jassertfalse;
        return 0;
    }
    const auto sourceMask =
        delay.state->transitionHandoffMasks[
            static_cast<std::size_t>(destinationSlot)].load(
            std::memory_order_acquire);
    if (sourceMask == 0
        || (sourceFilter >= 0
            && (sourceMask & (1 << sourceFilter)) == 0))
        return 0;

    auto destinationPosition =
        delay.state->transitionPositions[
            static_cast<std::size_t>(destinationSlot)].load(
            std::memory_order_relaxed);
    auto sourcePosition = destinationPosition;
    for (auto slot = 0; slot < 3; ++slot)
    {
        if ((sourceMask & (1 << slot)) == 0)
            continue;
        sourcePosition = std::max(
            sourcePosition,
            delay.state->transitionPositions[
                static_cast<std::size_t>(slot)].load(
                std::memory_order_acquire));
    }
    while (destinationPosition < sourcePosition
           && !delay.state->transitionPositions[
                   static_cast<std::size_t>(
                       destinationSlot)].compare_exchange_weak(
                   destinationPosition,
                   sourcePosition,
                   std::memory_order_acq_rel,
                   std::memory_order_relaxed))
    {
    }
    return sourceMask;
}

void StudioAudioEngine::mergeSnapshotTransitionHandoffs(
    RenderSnapshot& snapshot,
    int sourceFilter,
    int maximumSourceReaders) noexcept
{
    const auto merge = [this,
                        sourceFilter,
                        maximumSourceReaders](auto& delay)
    {
        const auto sourceMask =
            mergeDelayTransitionProgress(
                delay,
                sourceFilter);
        if (sourceMask == 0)
            return;

        auto clearMask = 0;
        for (auto slot = 0; slot < 3; ++slot)
        {
            const auto slotMask = 1 << slot;
            if ((sourceMask & slotMask) == 0
                || (sourceFilter >= 0
                    && slot != sourceFilter)
                || snapshotReaders[
                       static_cast<std::size_t>(slot)].load(
                       std::memory_order_seq_cst)
                    > maximumSourceReaders)
                continue;
            clearMask |= slotMask;
        }
        if (clearMask == 0)
            return;
        const auto destinationSlot =
            delay.transition->positionSlot;
        delay.state->transitionHandoffMasks[
            static_cast<std::size_t>(destinationSlot)].fetch_and(
            ~clearMask,
            std::memory_order_acq_rel);
    };

    for (auto& track : snapshot.tracks)
    {
        for (auto& source : track.sources)
            merge(source.compensation);
        merge(track.compensation);
        for (auto& route : track.routes)
            merge(route.compensation);
    }
    merge(snapshot.clickCompensation);
}

void StudioAudioEngine::applyDelayCompensation(
    juce::AudioBuffer<float>& buffer,
    int samples,
    RenderSource::DelayCompensator& delay) noexcept
{
    if (delay.state == nullptr
        || delay.state->buffer.getNumSamples()
            <= delay.delaySamples)
        return;

    mergeDelayTransitionProgress(delay);
    for (int sample = 0; sample < samples; ++sample)
    {
        std::array<float, 2> input {
            buffer.getSample(0, sample),
            buffer.getSample(1, sample)
        };
        const auto readState = [&input](
            const RenderSource::DelayCompensator::State& state,
            int delaySamples,
            std::array<float, 2>& output)
        {
            const auto capacity = state.buffer.getNumSamples();
            auto readPosition =
                state.writePosition - delaySamples;
            if (readPosition < 0)
                readPosition += capacity;
            for (int channel = 0; channel < 2; ++channel)
            {
                output[static_cast<std::size_t>(channel)] =
                    delaySamples > 0
                    ? state.buffer.getSample(
                          channel,
                          readPosition)
                    : input[static_cast<std::size_t>(channel)];
            }
        };
        const auto writeState = [&input](
            RenderSource::DelayCompensator::State& state)
        {
            const auto capacity = state.buffer.getNumSamples();
            for (int channel = 0; channel < 2; ++channel)
            {
                state.buffer.setSample(
                    channel,
                    state.writePosition,
                    input[static_cast<std::size_t>(channel)]);
            }
            state.writePosition =
                (state.writePosition + 1) % capacity;
            state.samplesWritten.fetch_add(
                1,
                std::memory_order_release);
        };

        std::array<float, 2> output {};
        readState(*delay.state, delay.delaySamples, output);
        auto* transition = delay.transition.has_value()
            ? &*delay.transition
            : nullptr;
        if (transition != nullptr
            && (transition->positionSlot < 0
                || transition->positionSlot >= 3))
        {
            jassertfalse;
            transition = nullptr;
        }
        mergeDelayTransitionProgress(delay);
        const auto transitionPosition = transition != nullptr
            ? delay.state->transitionPositions[
                  static_cast<std::size_t>(
                      transition->positionSlot)].load(
                  std::memory_order_relaxed)
            : 0;
        const auto transitionSamples = transition != nullptr
            ? transition->warmupSamples
                + transition->fadeSamples
            : 0;
        if (transition != nullptr
            && transition->sourceState != nullptr
            && transitionPosition < transitionSamples)
        {
            std::array<float, 2> sourceOutput {};
            readState(
                *transition->sourceState,
                transition->sourceDelaySamples,
                sourceOutput);
            const auto mix =
                transitionPosition < transition->warmupSamples
                ? 0.0f
                : transition->fadeSamples <= 1
                ? 1.0f
                : static_cast<float>(
                      transitionPosition
                      - transition->warmupSamples)
                    / static_cast<float>(
                        transition->fadeSamples - 1);
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto index =
                    static_cast<std::size_t>(channel);
                output[index] = sourceOutput[index]
                    + (output[index] - sourceOutput[index])
                        * mix;
            }
            if (transition->sourceState.get()
                != delay.state.get())
                writeState(*transition->sourceState);
            delay.state->transitionPositions[
                static_cast<std::size_t>(
                    transition->positionSlot)].fetch_add(
                1,
                std::memory_order_relaxed);
        }
        writeState(*delay.state);
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample(
                channel,
                sample,
                output[static_cast<std::size_t>(channel)]);
    }
}

void StudioAudioEngine::processRuntimeChain(std::uint64_t key,
                                            juce::AudioBuffer<float>& buffer,
                                            const juce::AudioBuffer<float>* sidechain,
                                            const std::vector<RenderSource::PluginAutomation>*
                                                automation,
                                            std::int64_t timelineSample) noexcept
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
            insert.parameterEventCount = 0;
            if (automation != nullptr)
            {
                const auto addEvent = [this, &insert](
                                          int parameterIndex,
                                          int sampleOffset,
                                          double value,
                                          int rampEndOffset,
                                          double rampEndValue,
                                          bool ramp)
                {
                    if (parameterIndex < 0
                        || insert.parameterEventCount
                            >= PluginBridgeSharedState::maxParameterEvents)
                    {
                        pluginAutomationEventsDropped.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        return;
                    }
                    auto& event = insert.parameterEvents[
                        static_cast<std::size_t>(
                            insert.parameterEventCount++)];
                    event.parameterIndex = static_cast<std::uint32_t>(
                        parameterIndex);
                    event.sampleOffset = static_cast<std::uint32_t>(
                        sampleOffset);
                    event.value = static_cast<float>(
                        juce::jlimit(0.0, 1.0, value));
                    event.flags = ramp
                        ? PluginBridgeParameterEvent::rampFlag
                        : 0;
                    event.rampEndOffset =
                        static_cast<std::uint32_t>(
                            std::max(sampleOffset, rampEndOffset));
                    event.rampEndValue = static_cast<float>(
                        juce::jlimit(0.0, 1.0, rampEndValue));
                };
                for (const auto& lane : *automation)
                {
                    if (lane.insertId != insert.insertId)
                        continue;
                    const auto parameterIndex =
                        lane.resolvedParameterIndex();
                    if (lane.lane.interpolation
                        == AutomationInterpolation::linear)
                    {
                        auto segmentStart = 0;
                        const auto addSegment = [&](int segmentEnd)
                        {
                            if (segmentEnd <= segmentStart)
                                return;
                            addEvent(
                                parameterIndex,
                                segmentStart,
                                lane.lane.valueAt(
                                    timelineSample + segmentStart),
                                segmentEnd,
                                lane.lane.valueAt(
                                    timelineSample + segmentEnd),
                                true);
                            segmentStart = segmentEnd;
                        };
                        const auto blockEnd =
                            timelineSample + buffer.getNumSamples();
                        for (const auto& point : lane.lane.points)
                        {
                            if (point.sample <= timelineSample
                                || point.sample >= blockEnd)
                                continue;
                            addSegment(static_cast<int>(
                                point.sample - timelineSample));
                        }
                        addSegment(buffer.getNumSamples());
                    }
                    else
                    {
                        addEvent(parameterIndex,
                                 0,
                                 lane.lane.valueAt(timelineSample),
                                 0,
                                 lane.lane.valueAt(timelineSample),
                                 false);
                        const auto blockEnd =
                            timelineSample + buffer.getNumSamples();
                        for (const auto& point : lane.lane.points)
                        {
                            if (point.sample <= timelineSample
                                || point.sample >= blockEnd)
                                continue;
                            addEvent(
                                parameterIndex,
                                static_cast<int>(
                                    point.sample - timelineSample),
                                point.value + lane.lane.trimOffset,
                                static_cast<int>(
                                    point.sample - timelineSample),
                                point.value + lane.lane.trimOffset,
                                false);
                        }
                    }
                }
                std::sort(
                    insert.parameterEvents.begin(),
                    insert.parameterEvents.begin()
                        + insert.parameterEventCount,
                    [](const auto& left, const auto& right)
                    {
                        return left.sampleOffset == right.sampleOffset
                            ? left.parameterIndex < right.parameterIndex
                            : left.sampleOffset < right.sampleOffset;
                    });
            }

            if (insert.bridge != nullptr && insert.bridge->isReady())
            {
                const auto before = insert.bridge->lateBlockCount();
                insert.bridge->processBlock(
                    buffer,
                    sidechain,
                    std::span<const PluginBridgeParameterEvent>(
                        insert.parameterEvents.data(),
                        static_cast<std::size_t>(
                            insert.parameterEventCount)));
                const auto after = insert.bridge->lateBlockCount();
                if (after > before)
                    pluginLateBlocks.fetch_add(after - before, std::memory_order_relaxed);
            }
            else if (insert.inProcess != nullptr)
            {
                if (insert.parameterEventCount == 0)
                {
                    processInProcessRuntime(insert, buffer, sidechain);
                    continue;
                }
                const auto events =
                    std::span<const PluginBridgeParameterEvent>(
                        insert.parameterEvents.data(),
                        static_cast<std::size_t>(
                            insert.parameterEventCount));
                if (processInProcessRuntime(
                        insert,
                        buffer,
                        sidechain,
                        0,
                        events))
                {
                    continue;
                }

                auto& boundaries = insert.automationBoundaries;
                boundaries.clear();
                boundaries.push_back(0);
                boundaries.push_back(buffer.getNumSamples());
                auto automationStride = buffer.getNumSamples();
                for (int eventIndex = 0;
                     eventIndex < insert.parameterEventCount;
                     ++eventIndex)
                {
                    const auto& event = insert.parameterEvents[
                        static_cast<std::size_t>(eventIndex)];
                    const auto start = std::min(
                        buffer.getNumSamples(),
                        static_cast<int>(event.sampleOffset));
                    boundaries.push_back(start);
                    if ((event.flags
                         & PluginBridgeParameterEvent::rampFlag)
                        != 0)
                    {
                        const auto end = std::min(
                            buffer.getNumSamples(),
                            static_cast<int>(event.rampEndOffset));
                        boundaries.push_back(end);
                        const auto span = std::max(1, end - start);
                        const auto delta = std::abs(
                            event.rampEndValue - event.value);
                        const auto toleranceStride = delta > 0.0f
                            ? static_cast<int>(std::floor(
                                  0.002f
                                  * static_cast<float>(span)
                                  / delta))
                            : span;
                        if (span >= 8)
                        {
                            automationStride = std::min(
                                automationStride,
                                std::clamp(
                                    std::max(1, toleranceStride),
                                    8,
                                    span));
                        }
                    }
                }
                if (automationStride < buffer.getNumSamples())
                {
                    for (auto offset = automationStride;
                         offset < buffer.getNumSamples();
                         offset += automationStride)
                        boundaries.push_back(offset);
                }
                std::sort(boundaries.begin(), boundaries.end());
                boundaries.erase(
                    std::unique(boundaries.begin(), boundaries.end()),
                    boundaries.end());
                for (std::size_t boundary = 0;
                     boundary + 1 < boundaries.size();
                     ++boundary)
                {
                    const auto offset = boundaries[boundary];
                    const auto next = boundaries[boundary + 1];
                    auto& parameters =
                        insert.inProcess->getParameters();
                    for (int eventIndex = 0;
                         eventIndex < insert.parameterEventCount;
                         ++eventIndex)
                    {
                        const auto& event = insert.parameterEvents[
                            static_cast<std::size_t>(eventIndex)];
                        if (event.parameterIndex
                            >= static_cast<std::uint32_t>(
                                parameters.size()))
                        {
                            continue;
                        }
                        const auto start = static_cast<int>(
                            event.sampleOffset);
                        const auto end = static_cast<int>(
                            event.rampEndOffset);
                        if ((event.flags
                             & PluginBridgeParameterEvent::rampFlag)
                            != 0
                            && offset >= start
                            && offset <= end
                            && end > start)
                        {
                            const auto progress = static_cast<float>(
                                offset - start)
                                / static_cast<float>(end - start);
                            parameters[static_cast<int>(
                                event.parameterIndex)]
                                ->setValue(
                                    event.value
                                    + (event.rampEndValue
                                       - event.value)
                                        * progress);
                        }
                        else if (offset == start)
                        {
                            parameters[static_cast<int>(
                                event.parameterIndex)]
                                ->setValue(event.value);
                        }
                    }
                    juce::AudioBuffer<float> segment(
                        buffer.getArrayOfWritePointers(),
                        buffer.getNumChannels(),
                        offset,
                        next - offset);
                    processInProcessRuntime(
                        insert,
                        segment,
                        sidechain,
                        offset);
                }
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

bool StudioAudioEngine::processInProcessRuntime(
    InsertRuntime& insert,
    juce::AudioBuffer<float>& buffer,
    const juce::AudioBuffer<float>* sidechain,
    int sidechainSampleOffset,
    std::span<const PluginBridgeParameterEvent> automation) noexcept
{
    auto& processor = *insert.inProcess;
    auto* automationTarget =
        dynamic_cast<SampleAccurateAutomationTarget*>(&processor);
    if (!automation.empty() && automationTarget == nullptr)
        return false;
    const auto mainChannels = processor.getBusCount(true) > 0
        ? processor.getChannelCountOfBus(true, 0)
        : processor.getTotalNumInputChannels();
    const auto outputChannels = processor.getBusCount(false) > 0
        ? processor.getChannelCountOfBus(false, 0)
        : processor.getTotalNumOutputChannels();
    const auto useScratch = processor.getTotalNumInputChannels()
            > buffer.getNumChannels()
        || processor.getTotalNumOutputChannels()
            > buffer.getNumChannels()
        || outputChannels != buffer.getNumChannels()
        || (sidechain != nullptr && processor.getBusCount(true) > 1);
    if (!useScratch)
    {
        insert.midi.clear();
        if (automationTarget != nullptr && !automation.empty())
        {
            if (!automationTarget
                     ->processBlockWithAutomationOrFallback(
                         buffer,
                         insert.midi,
                         automation))
                return false;
        }
        else
            processor.processBlock(buffer, insert.midi);
        return true;
    }

    auto& scratch = insert.inProcessBuffer;
    scratch.clear(0, buffer.getNumSamples());
    for (int channel = 0;
         channel < std::min(mainChannels, buffer.getNumChannels());
         ++channel)
    {
        const auto destination =
            processor.getChannelIndexInProcessBlockBuffer(
                true,
                0,
                channel);
        scratch.copyFrom(destination,
                         0,
                         buffer,
                         channel,
                         0,
                         buffer.getNumSamples());
    }
    if (sidechain != nullptr && processor.getBusCount(true) > 1)
    {
        const auto sidechainChannels =
            processor.getChannelCountOfBus(true, 1);
        for (int channel = 0;
             channel
                 < std::min(sidechainChannels,
                            sidechain->getNumChannels());
             ++channel)
        {
            const auto destination =
                processor.getChannelIndexInProcessBlockBuffer(
                    true,
                    1,
                    channel);
            scratch.copyFrom(destination,
                             0,
                             *sidechain,
                             channel,
                             sidechainSampleOffset,
                             buffer.getNumSamples());
        }
    }
    insert.midi.clear();
    juce::AudioBuffer<float> scratchView(
        scratch.getArrayOfWritePointers(),
        scratch.getNumChannels(),
        buffer.getNumSamples());
    if (automationTarget != nullptr && !automation.empty())
    {
        if (!automationTarget
                 ->processBlockWithAutomationOrFallback(
                     scratchView,
                     insert.midi,
                     automation))
            return false;
    }
    else
        processor.processBlock(scratchView, insert.midi);
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto outputChannel =
            PluginBridgeProtocol::outputSourceChannel(
                outputChannels,
                channel);
        if (outputChannel >= 0)
        {
            const auto source =
                processor.getChannelIndexInProcessBlockBuffer(
                    false,
                    0,
                    outputChannel);
            buffer.copyFrom(channel,
                            0,
                            scratch,
                            source,
                            0,
                            buffer.getNumSamples());
        }
        else
        {
            buffer.clear(channel, 0, buffer.getNumSamples());
        }
    }
    return true;
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
    for (;;)
    {
        const auto pair =
            activeRenderPair.load(std::memory_order_acquire);
        const auto active =
            static_cast<int>((pair >> 2) & 0x3);
        for (int index = 0;
             index < static_cast<int>(snapshots.size());
             ++index)
        {
            if (index != active
                && snapshotReaders[
                       static_cast<std::size_t>(index)].load(
                       std::memory_order_seq_cst)
                    == 0)
                return index;
        }
        juce::Thread::sleep(1);
    }
}

int StudioAudioEngine::meterSlotFor(const juce::String& trackId)
{
    const juce::ScopedLock lock(meterLock);
    for (std::size_t index = 0; index < meterSlots.size(); ++index)
        if (meterSlots[index].trackId == trackId)
            return static_cast<int>(index);
    for (std::size_t index = 0; index < meterSlots.size(); ++index)
    {
        if (meterSlots[index].trackId.isNotEmpty())
            continue;
        meterSlots[index].trackId = trackId;
        return static_cast<int>(index);
    }
    return -1;
}

void StudioAudioEngine::closeInProcessEditorsAsync()
{
    const auto* manager =
        juce::MessageManager::getInstanceWithoutCreating();
    if (manager == nullptr)
    {
        const juce::ScopedLock editorGuard(editorLock);
        editorSessions.clear();
        return;
    }
    if (manager->isThisTheMessageThread())
    {
        closeInProcessEditors();
        return;
    }
    const auto alive = editorCallbacksAlive;
    juce::MessageManager::callAsync(
        [this, alive]
        {
            if (alive->load(std::memory_order_acquire))
                closeInProcessEditors();
        });
}

void StudioAudioEngine::closeInProcessEditors()
{
    const auto clear = [this]
    {
        const juce::ScopedLock editorGuard(editorLock);
        editorSessions.clear();
    };
    const auto* manager =
        juce::MessageManager::getInstanceWithoutCreating();
    if (manager == nullptr || manager->isThisTheMessageThread())
    {
        clear();
        return;
    }

    const juce::MessageManagerLock messageLock;
    if (messageLock.lockWasGained())
        clear();
    else
        juce::Logger::writeToLog(
            "plugin.editor: could not acquire the message thread for cleanup");
}

bool StudioAudioEngine::closeInProcessEditorsBeforeRuntimeChange()
{
    const auto* manager =
        juce::MessageManager::getInstanceWithoutCreating();
    if (manager == nullptr || manager->isThisTheMessageThread())
    {
        closeInProcessEditors();
        return true;
    }

    struct CloseState
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
    };
    const auto state = std::make_shared<CloseState>();
    const auto alive = editorCallbacksAlive;
    if (!juce::MessageManager::callAsync(
            [this, alive, state]
            {
                if (alive->load(std::memory_order_acquire))
                    closeInProcessEditors();
                {
                    const std::lock_guard stateGuard(state->mutex);
                    state->completed = true;
                }
                state->condition.notify_one();
            }))
    {
        return false;
    }

    std::unique_lock stateLock(state->mutex);
    while (!state->completed)
    {
        if (shuttingDown.load(std::memory_order_acquire))
            return false;
        state->condition.wait_for(
            stateLock,
            std::chrono::milliseconds(5));
    }
    return true;
}

void StudioAudioEngine::resumePendingPluginRuntime()
{
    if (shuttingDown.load(std::memory_order_acquire))
        return;
    auto startBuilder = false;
    {
        const juce::ScopedLock lock(pluginRequestLock);
        if (hasPendingPluginRequest && !pluginBuilderRunning)
        {
            pluginBuilderRunning = true;
            startBuilder = true;
        }
    }
    if (startBuilder)
        pluginRuntimeBuilder.addJob(
            [this] { runPluginRuntimeBuilder(); });
}

bool StudioAudioEngine::beginPluginControlOperation() noexcept
{
    if (exclusiveAudioOperation.load(std::memory_order_acquire) == 2)
        return false;
    pluginControlOperationsInFlight.fetch_add(
        1,
        std::memory_order_acq_rel);
    if (exclusiveAudioOperation.load(std::memory_order_acquire) != 2)
        return true;
    pluginControlOperationsInFlight.fetch_sub(
        1,
        std::memory_order_acq_rel);
    return false;
}

void StudioAudioEngine::requestPluginRuntime(
    std::vector<PluginRuntimeRequest> requests,
    RenderSnapshot snapshot,
    bool renderOwned)
{
    if (shuttingDown.load(std::memory_order_acquire))
        return;
    if (renderInProgress.load(std::memory_order_acquire)
        && !renderOwned)
        return;
    const auto fingerprint = pluginRuntimeFingerprint(requests);
    auto startBuilder = false;
    auto closeEditors = false;
    {
        const juce::ScopedLock lock(pluginRequestLock);
        if (fingerprint == activePluginFingerprint
            && fingerprint == desiredPluginFingerprint
            && !pluginBuilderRunning
            && !hasPendingPluginRequest)
        {
            const auto runtime =
                activePluginRuntime.load(std::memory_order_acquire);
            std::vector<PluginRuntimeStatus> statuses;
            {
                const juce::ScopedLock statusLock(pluginStatusLock);
                statuses = pluginStatuses;
            }
            resolvePluginAutomationParameterIds(
                snapshot,
                statuses);
            const auto destination = chooseWritableSnapshot();
            configureRuntimeTiming(
                snapshot,
                activePluginRequests,
                destination);
            activeSnapshotTemplate =
                std::make_shared<RenderSnapshot>(snapshot);
            ++activeSnapshotRevision;
            ++pluginSnapshotRefreshRevision;
            pluginSnapshotRefreshPending = false;
            snapshots[static_cast<std::size_t>(destination)] = std::move(snapshot);
            snapshotGenerations[static_cast<std::size_t>(destination)].store(
                activePluginGeneration,
                std::memory_order_release);
            activeSnapshot.store(destination, std::memory_order_release);
            activeRenderPair.store(renderPair(activePluginGeneration,
                                              destination,
                                              runtime),
                                   std::memory_order_release);
            return;
        }

        if (fingerprint != desiredPluginFingerprint
            || (!pluginBuilderRunning
                && !hasPendingPluginRequest
                && fingerprint != activePluginFingerprint))
        {
            if (exclusiveAudioOperation.load(
                    std::memory_order_acquire)
                == 2)
            {
                pendingEditorCloseRequired = true;
            }
            else
            {
                closeEditors = true;
                pendingEditorCloseRequired = false;
            }
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

    if (closeEditors)
        closeInProcessEditorsAsync();
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
        auto closeEditorsBeforeBuild = false;
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
            closeEditorsBeforeBuild = pendingEditorCloseRequired;
            pendingEditorCloseRequired = false;
        }

        if (closeEditorsBeforeBuild
            && !closeInProcessEditorsBeforeRuntimeChange()
            && shuttingDown.load(std::memory_order_acquire))
        {
            return;
        }

        const auto preserved = preserveLiveAraStates(requests);
        if (preserved.status
            != AraPreservationResult::Status::success)
        {
            juce::Logger::writeToLog(
                "plugin.ara.preserve: "
                + preserved.error);
            const juce::ScopedLock lock(pluginRequestLock);
            if (generation == desiredPluginGeneration)
            {
                if (preserved.status
                    == AraPreservationResult::Status::recordingBlocked)
                {
                    pendingPluginRequests = std::move(requests);
                    pendingPluginFingerprint = fingerprint;
                    pendingPluginGeneration = generation;
                    hasPendingPluginRequest = true;
                    buildingPluginFingerprint.clear();
                    if (exclusiveAudioOperation.load(
                            std::memory_order_acquire)
                        == 1)
                    {
                        pluginBuilderRunning = false;
                        return;
                    }
                    continue;
                }
                desiredPluginFingerprint =
                    activePluginFingerprint;
                desiredPluginGeneration = activePluginGeneration;
                pendingPluginSnapshot.reset();
                pendingSnapshotGeneration = 0;
                buildingPluginFingerprint.clear();
            }
            continue;
        }

        std::vector<PluginRuntimeStatus> statuses;
        statuses.reserve(requests.size());
        PluginRuntimeGraph graph;
        auto inProcessFormats =
            std::make_shared<juce::AudioPluginFormatManager>();
        PluginFormats::addSupportedFormats(*inProcessFormats);

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

            if (request.missing
                || (!request.description.has_value()
                    && request.deviceIdentifier.isEmpty()))
            {
                status.state = PluginRuntimeStatus::State::missing;
                status.message = "Plugin unavailable";
                statuses.push_back(std::move(status));
                continue;
            }
            if (request.recoveryDisabled)
            {
                status.state = PluginRuntimeStatus::State::failed;
                status.message = "Disabled after an unclean in-process session";
                statuses.push_back(std::move(status));
                continue;
            }

            status.state = PluginRuntimeStatus::State::loading;
            status.message = request.bridgeMode == PluginBridgeMode::sandboxed
                ? "Starting sandbox worker"
                : "Starting reduced-isolation plugin";
            {
                const juce::ScopedLock lock(pluginStatusLock);
                pluginStatuses = statuses;
                pluginStatuses.push_back(status);
            }

            if (request.bridgeMode != PluginBridgeMode::sandboxed)
            {
                if (request.bridgeMode == PluginBridgeMode::araCompatibility
                    && (!request.description.has_value()
                        || !request.description->hasARAExtension))
                {
                    status.state = PluginRuntimeStatus::State::failed;
                    status.message = "Plugin does not advertise ARA 2";
                    statuses.push_back(std::move(status));
                    continue;
                }

                juce::String creationError;
                std::shared_ptr<juce::AudioProcessor> processor;
                juce::AudioPluginInstance* externalInstance = nullptr;
                juce::MemoryBlock processorState = request.state;
                juce::MemoryBlock araState;
                if (request.bridgeMode
                    == PluginBridgeMode::araCompatibility)
                {
                    AraDocumentHost::unpackState(
                        request.state,
                        processorState,
                        araState);
                }
                if (request.deviceIdentifier.isNotEmpty())
                {
                    processor = messageThreadOwnedProcessor(
                        DeviceRegistry::create(
                            request.deviceIdentifier));
                }
                else
                {
                    std::unique_ptr<juce::AudioPluginInstance> instance;
                    if (request.description->pluginFormatName == "CLAP")
                    {
                        instance = ClapPluginInstance::create(
                            *request.description,
                            currentSampleRate(),
                            PluginBridgeSharedState::maxBlockSize,
                            creationError);
                    }
                    else
                    {
                        struct InstanceCreationState
                        {
                            std::mutex mutex;
                            std::condition_variable condition;
                            std::unique_ptr<juce::AudioPluginInstance>
                                instance;
                            juce::String error;
                            bool completed = false;
                        };
                        const auto creation =
                            std::make_shared<InstanceCreationState>();
                        const auto started = juce::MessageManager::callSync(
                            [formats = inProcessFormats,
                             creation,
                             description = *request.description,
                             runtimeSampleRate = currentSampleRate()]
                            {
                                formats->createPluginInstanceAsync(
                                    description,
                                    runtimeSampleRate,
                                    PluginBridgeSharedState::maxBlockSize,
                                    [creation](
                                        std::unique_ptr<
                                            juce::AudioPluginInstance>
                                            createdInstance,
                                        const juce::String& error)
                                    {
                                        {
                                            const std::lock_guard lock(
                                                creation->mutex);
                                            creation->instance =
                                                std::move(
                                                    createdInstance);
                                            creation->error = error;
                                            creation->completed = true;
                                        }
                                        creation->condition.notify_one();
                                    });
                                return true;
                            });
                        if (!started.has_value())
                        {
                            creationError =
                                "The message thread is unavailable.";
                        }
                        else
                        {
                            std::unique_lock lock(creation->mutex);
                            const auto completed =
                                creation->condition.wait_for(
                                    lock,
                                    std::chrono::seconds(5),
                                    [this, creation]
                                    {
                                        return creation->completed
                                            || shuttingDown.load(
                                                std::memory_order_acquire);
                                    });
                            if (creation->completed)
                            {
                                instance =
                                    std::move(creation->instance);
                                creationError = creation->error;
                            }
                            else if (!completed)
                            {
                                creationError =
                                    "Timed out while creating the in-process plugin.";
                            }
                        }
                    }
                    if (shuttingDown.load(std::memory_order_acquire))
                        return;
                    externalInstance = instance.get();
                    processor = messageThreadOwnedProcessor(
                        std::move(instance));
                }
                if (processor == nullptr)
                {
                    status.state = PluginRuntimeStatus::State::failed;
                    status.message = creationError.isNotEmpty()
                        ? creationError
                        : "In-process plugin instantiation failed";
                    statuses.push_back(std::move(status));
                    continue;
                }
                if (request.sidechainChannels > 0
                    && processor->getBusCount(true) > 1)
                {
                    auto layout = processor->getBusesLayout();
                    layout.inputBuses.set(
                        1,
                        request.sidechainChannels == 1
                            ? juce::AudioChannelSet::mono()
                            : juce::AudioChannelSet::stereo());
                    if (!processor->setBusesLayout(layout))
                    {
                        status.state = PluginRuntimeStatus::State::failed;
                        status.message =
                            "Processor rejected the requested sidechain layout.";
                        statuses.push_back(std::move(status));
                        continue;
                    }
                }
                if (!processorState.isEmpty())
                {
                    if (auto* validated =
                            dynamic_cast<ValidatedPluginStateTarget*>(
                                processor.get()))
                    {
                        if (const auto stateResult =
                                validated->restoreValidatedState(
                                    processorState.getData(),
                                    static_cast<int>(
                                        processorState.getSize()));
                            stateResult.failed())
                        {
                            status.state =
                                PluginRuntimeStatus::State::failed;
                            status.message =
                                stateResult.getErrorMessage();
                            statuses.push_back(std::move(status));
                            continue;
                        }
                    }
                    else
                    {
                        processor->setStateInformation(
                            processorState.getData(),
                            static_cast<int>(
                                processorState.getSize()));
                    }
                }
                std::unique_ptr<AraDocumentHost> araDocument;
                if (request.bridgeMode == PluginBridgeMode::araCompatibility)
                {
                    araDocument = std::make_unique<AraDocumentHost>();
                    if (externalInstance == nullptr)
                    {
                        processor->releaseResources();
                        status.state = PluginRuntimeStatus::State::failed;
                        status.message =
                            "ARA mode requires an external plugin instance.";
                        statuses.push_back(std::move(status));
                        continue;
                    }
                    if (const auto result = araDocument->bind(
                            *externalInstance,
                            request.araDocument,
                            araState);
                        result.failed())
                    {
                        status.state = PluginRuntimeStatus::State::failed;
                        status.message = result.getErrorMessage();
                        statuses.push_back(std::move(status));
                        continue;
                    }
                }
                processor->prepareToPlay(
                    currentSampleRate(),
                    PluginBridgeSharedState::maxBlockSize);

                auto track = std::find_if(
                    graph.tracks.begin(),
                    graph.tracks.end(),
                    [&request](const auto& candidate)
                    {
                        return candidate.key == runtimeKey(request.trackId);
                    });
                if (track == graph.tracks.end())
                {
                    graph.tracks.push_back({
                        runtimeKey(request.trackId),
                        request.trackId,
                        {}
                    });
                    track = std::prev(graph.tracks.end());
                }
                track->trackId = request.trackId;

                InsertRuntime insertRuntime;
                insertRuntime.insertId = request.insertId;
                insertRuntime.name = request.name;
                insertRuntime.failureDelay.delaySamples =
                    std::max(0, processor->getLatencySamples());
                insertRuntime.failureDelay.state =
                    std::make_shared<
                        RenderSource::DelayCompensator::State>();
                insertRuntime.failureDelay.state->buffer.setSize(
                    2,
                    insertRuntime.failureDelay.delaySamples
                        + PluginBridgeSharedState::maxBlockSize
                        + 1);
                insertRuntime.failureDelay.state->buffer.clear();
                insertRuntime.inProcessBuffer.setSize(
                    std::max({
                        2,
                        processor->getTotalNumInputChannels(),
                        processor->getTotalNumOutputChannels()
                    }),
                    PluginBridgeSharedState::maxBlockSize,
                    false,
                    true,
                    false);
                insertRuntime.inProcessBuffer.clear();
                status.latencySamples = processor->getLatencySamples();
                status.tailSeconds = processor->getTailLengthSeconds();
                {
                    const auto& parameters = processor->getParameters();
                    for (int index = 0; index < parameters.size(); ++index)
                    {
                        const auto* parameter = parameters[index];
                        const auto* hosted = dynamic_cast<
                            const juce::HostedAudioProcessorParameter*>(
                            parameter);
                        const auto* ranged = dynamic_cast<
                            const juce::RangedAudioParameter*>(
                            parameter);
                        status.parameters.push_back({
                            index,
                            hosted != nullptr
                                ? hosted->getParameterID()
                                : ranged != nullptr
                                ? ranged->paramID
                                : juce::String(index),
                            parameter->getName(128),
                            parameter->getValue(),
                            parameter->isAutomatable()
                        });
                    }
                }
                request.latencySamples = status.latencySamples;
                request.tailSeconds = status.tailSeconds;
                insertRuntime.inProcess = std::move(processor);
                insertRuntime.araDocument = std::move(araDocument);
                track->inserts.push_back(std::move(insertRuntime));
                status.state = PluginRuntimeStatus::State::ready;
                status.message =
                    request.deviceIdentifier.isNotEmpty()
                    ? "Bundled device ready"
                    : request.bridgeMode == PluginBridgeMode::araCompatibility
                    ? "ARA compatibility ready - reduced isolation"
                    : "Trusted in-process ready";
                statuses.push_back(std::move(status));
                continue;
            }

            auto bridge = std::make_unique<PluginBridgeClient>();
            const auto result = bridge->startPlugin(*request.description,
                                                    currentSampleRate(),
                                                    juce::jlimit(
                                                        1,
                                                        PluginBridgeSharedState::maxBlockSize,
                                                        deviceBlockSize.load(
                                                            std::memory_order_acquire)),
                                                    request.state,
                                                    request.sidechainChannels);
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
                    graph.tracks.push_back({
                        runtimeKey(request.trackId),
                        request.trackId,
                        {}
                    });
                    track = std::prev(graph.tracks.end());
                }
                track->trackId = request.trackId;

                InsertRuntime failedRuntime;
                failedRuntime.insertId = request.insertId;
                failedRuntime.name = request.name;
                failedRuntime.failureDelay.delaySamples
                    = deviceBlockSize.load(std::memory_order_acquire)
                    + std::max(0, request.latencySamples);
                failedRuntime.failureDelay.state =
                    std::make_shared<
                        RenderSource::DelayCompensator::State>();
                failedRuntime.failureDelay.state->buffer.setSize(
                    2,
                    failedRuntime.failureDelay.delaySamples
                        + PluginBridgeSharedState::maxBlockSize
                        + 1);
                failedRuntime.failureDelay.state->buffer.clear();
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
                graph.tracks.push_back({
                    runtimeKey(request.trackId),
                    request.trackId,
                    {}
                });
                track = std::prev(graph.tracks.end());
            }
            track->trackId = request.trackId;

            InsertRuntime insertRuntime;
            insertRuntime.insertId = request.insertId;
            insertRuntime.name = request.name;
            insertRuntime.failureDelay.delaySamples
                = deviceBlockSize.load(std::memory_order_acquire)
                + bridge->reportedLatencySamples();
            insertRuntime.failureDelay.state =
                std::make_shared<
                    RenderSource::DelayCompensator::State>();
            insertRuntime.failureDelay.state->buffer.setSize(
                2,
                insertRuntime.failureDelay.delaySamples
                    + PluginBridgeSharedState::maxBlockSize
                    + 1);
            insertRuntime.failureDelay.state->buffer.clear();
            insertRuntime.bridge = std::move(bridge);
            track->inserts.push_back(std::move(insertRuntime));
            status.state = PluginRuntimeStatus::State::ready;
            status.message = "Sandbox ready";
            status.latencySamples = track->inserts.back().bridge->reportedLatencySamples();
            status.tailSeconds = track->inserts.back().bridge->reportedTailSeconds();
            status.parameters =
                track->inserts.back().bridge->parameterDescriptors();
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

            resolvePluginAutomationParameterIds(
                *pendingPluginSnapshot,
                statuses);
            const auto runtimeDestination = chooseWritableRuntime();
            const auto snapshotDestination = chooseWritableSnapshot();
            configureRuntimeTiming(
                *pendingPluginSnapshot,
                requests,
                snapshotDestination);
            activeSnapshotTemplate =
                std::make_shared<RenderSnapshot>(
                    *pendingPluginSnapshot);
            ++activeSnapshotRevision;
            ++pluginSnapshotRefreshRevision;
            pluginSnapshotRefreshPending = false;
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
            {
                const juce::ScopedLock statusLock(pluginStatusLock);
                pluginStatuses = std::move(statuses);
            }
            activePluginFingerprint = fingerprint;
            activePluginGeneration = generation;
            activePluginRequests = requests;
            buildingPluginFingerprint.clear();
        }
        pluginLateBlocks.store(0, std::memory_order_relaxed);
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
                    << pluginBridgeModeToString(request.bridgeMode)
                    << ":"
                    << (request.recoveryDisabled ? "r" : "n")
                    << ":"
                    << juce::String(request.catalogRevision)
                    << ":"
                    << juce::String::toHexString(static_cast<juce::int64>(stateHash))
                    << ":";
        fingerprint << request.deviceIdentifier
                    << ":"
                    << request.sidechainChannels
                    << ":";
        if (request.araDocument != nullptr)
            fingerprint << request.araDocument->revision;
        fingerprint << ":";
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
    for (;;)
    {
        const auto pair =
            activeRenderPair.load(std::memory_order_acquire);
        const auto active = static_cast<int>(pair & 0x3);
        for (int index = 0;
             index
                 < static_cast<int>(pluginRuntimeGraphs.size());
             ++index)
        {
            if (index != active
                && pluginRuntimeReaders[
                       static_cast<std::size_t>(index)].load(
                       std::memory_order_seq_cst)
                    == 0)
                return index;
        }
        juce::Thread::sleep(1);
    }
}

void StudioAudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                         int numInputChannels,
                                                         float* const* outputChannelData,
                                                         int numOutputChannels,
                                                         int numSamples,
                                                         const juce::AudioIODeviceCallbackContext&)
{
    audioCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    AtomicCounterScope callbackScope { audioCallbacksInFlight };

    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[channel], numSamples);

    if (pluginResetInProgress.load(std::memory_order_acquire))
        return;

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

    if (pluginStateOperationActive.load(std::memory_order_acquire))
    {
        outputLeftPeak.store(0.0f, std::memory_order_relaxed);
        outputRightPeak.store(0.0f, std::memory_order_relaxed);
        return;
    }

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
        for (;;)
        {
            const auto pair = activeRenderPair.load(std::memory_order_acquire);
            runtimeIndex = static_cast<int>(pair & 0x3);
            snapshotIndex = static_cast<int>((pair >> 2) & 0x3);
            snapshotReaders[
                static_cast<std::size_t>(snapshotIndex)].fetch_add(
                1,
                std::memory_order_seq_cst);
            pluginRuntimeReaders[
                static_cast<std::size_t>(runtimeIndex)].fetch_add(
                1,
                std::memory_order_seq_cst);
            if (pair == activeRenderPair.load(std::memory_order_acquire))
                break;
            pluginRuntimeReaders[
                static_cast<std::size_t>(runtimeIndex)].fetch_sub(
                1,
                std::memory_order_seq_cst);
            snapshotReaders[
                static_cast<std::size_t>(snapshotIndex)].fetch_sub(
                1,
                std::memory_order_seq_cst);
        }
        readingSnapshot.store(snapshotIndex, std::memory_order_release);
        readingPluginRuntime.store(runtimeIndex, std::memory_order_release);
        struct RenderPairScope
        {
            ~RenderPairScope()
            {
                readingRuntime.store(-1, std::memory_order_release);
                readingSnapshot.store(-1, std::memory_order_release);
                runtimeReaders.fetch_sub(
                    1,
                    std::memory_order_seq_cst);
                snapshotReaders.fetch_sub(
                    1,
                    std::memory_order_seq_cst);
            }
            std::atomic<int>& snapshotReaders;
            std::atomic<int>& runtimeReaders;
            std::atomic<int>& readingSnapshot;
            std::atomic<int>& readingRuntime;
        } renderPairScope {
            snapshotReaders[static_cast<std::size_t>(snapshotIndex)],
            pluginRuntimeReaders[
                static_cast<std::size_t>(runtimeIndex)],
            readingSnapshot,
            readingPluginRuntime
        };

        auto& snapshot = snapshots[static_cast<std::size_t>(snapshotIndex)];
        mergeSnapshotTransitionHandoffs(
            snapshot,
            -1,
            0);
        auto position = playheadSample.load(std::memory_order_acquire);
        const auto renderingRecording = recordingBlockActive;
        const auto renderLooping = snapshot.loopEnabled
            && (!renderingRecording
                || recordingLoopEnabled.load(std::memory_order_relaxed));
        const auto recordingTransportEnd = renderingRecording
            ? recordingTransportEndSample.load(std::memory_order_relaxed)
            : -1;
        if (renderLooping)
        {
            position = wrapLoopPosition(
                position,
                snapshot.loopStartSample,
                snapshot.loopEndSample);
        }
        auto outputOffset = 0;

        while (outputOffset < numSamples)
        {
            auto samplesThisBlock = std::min(
                numSamples - outputOffset,
                PluginBridgeSharedState::maxBlockSize);
            if (renderLooping)
            {
                samplesThisBlock = static_cast<int>(
                    std::min<std::int64_t>(
                        samplesThisBlock,
                        snapshot.loopEndSample - position));
            }

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
                track.sidechainBuffer.clear(0, 0, samplesThisBlock);
                track.sidechainBuffer.clear(1, 0, samplesThisBlock);
                for (auto& route : track.routes)
                {
                    route.processingBuffer.clear(0, 0, samplesThisBlock);
                    route.processingBuffer.clear(1, 0, samplesThisBlock);
                }
            }

            for (auto& track : snapshot.tracks)
            {
                if (!track.processing)
                    continue;

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
                        processRuntimeChain(
                            source.runtimeKey,
                            sourceView,
                            nullptr,
                            &source.pluginAutomation,
                            position);
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
                processRuntimeChain(track.runtimeKey,
                                    trackView,
                                    &track.sidechainBuffer,
                                    &track.pluginAutomation,
                                    position);
                const auto publishMeter = [&](bool postFader)
                {
                    if (track.meterIndex < 0
                        || track.meterIndex
                            >= static_cast<int>(meterSlots.size()))
                        return;
                    auto left = 0.0f;
                    auto right = 0.0f;
                    for (int sample = 0; sample < samplesThisBlock; ++sample)
                    {
                        left = std::max(
                            left,
                            std::abs(track.processingBuffer.getSample(
                                0,
                                sample)));
                        right = std::max(
                            right,
                            std::abs(track.processingBuffer.getSample(
                                1,
                                sample)));
                    }
                    auto& meter = meterSlots[static_cast<std::size_t>(
                        track.meterIndex)];
                    if (postFader)
                    {
                        meter.postFaderLeft.store(left,
                                                  std::memory_order_relaxed);
                        meter.postFaderRight.store(right,
                                                   std::memory_order_relaxed);
                    }
                    else
                    {
                        meter.preFaderLeft.store(left,
                                                 std::memory_order_relaxed);
                        meter.preFaderRight.store(right,
                                                  std::memory_order_relaxed);
                    }
                };
                publishMeter(false);
                const auto processRoutes = [&](RouteTap tap)
                {
                    for (auto& route : track.routes)
                    {
                        if (route.tap != tap
                            || (route.kind != RouteKind::sidechain
                                && !track.audible))
                            continue;

                        route.processingBuffer.copyFrom(
                            0,
                            0,
                            track.processingBuffer,
                            0,
                            0,
                            samplesThisBlock);
                        route.processingBuffer.copyFrom(
                            1,
                            0,
                            track.processingBuffer,
                            1,
                            0,
                            samplesThisBlock);
                        if (!route.gainAutomation.has_value()
                            && !route.panAutomation.has_value()
                            && !route.muteAutomation.has_value())
                        {
                            applyTrackGainAndPan(route.processingBuffer,
                                                 samplesThisBlock,
                                                 route.gain,
                                                 route.pan);
                        }
                        else
                        {
                            for (int sample = 0;
                                 sample < samplesThisBlock;
                                 ++sample)
                            {
                                const auto absoluteSample = position + sample;
                                if (automatedSwitch(
                                        route.muteAutomation,
                                        absoluteSample,
                                        false))
                                {
                                    route.processingBuffer.setSample(
                                        0,
                                        sample,
                                        0.0f);
                                    route.processingBuffer.setSample(
                                        1,
                                        sample,
                                        0.0f);
                                    continue;
                                }
                                const auto gain = automatedVolumeGain(
                                    route.gainAutomation,
                                    absoluteSample,
                                    route.gain);
                                const auto pan = automatedPan(
                                    route.panAutomation,
                                    absoluteSample,
                                    route.pan);
                                route.processingBuffer.applyGain(
                                    0,
                                    sample,
                                    1,
                                    gain
                                        * (pan > 0.0f ? 1.0f - pan
                                                      : 1.0f));
                                route.processingBuffer.applyGain(
                                    1,
                                    sample,
                                    1,
                                    gain
                                        * (pan < 0.0f ? 1.0f + pan
                                                      : 1.0f));
                            }
                        }
                        applyDelayCompensation(route.processingBuffer,
                                               samplesThisBlock,
                                               route.compensation);

                        if (route.destinationIndex >= 0
                            && route.destinationIndex
                                < static_cast<int>(snapshot.tracks.size()))
                        {
                            auto& destination =
                                snapshot.tracks[static_cast<std::size_t>(
                                    route.destinationIndex)];
                            auto& destinationBuffer =
                                route.kind == RouteKind::sidechain
                                ? destination.sidechainBuffer
                                : destination.processingBuffer;
                            destinationBuffer.addFrom(
                                0,
                                0,
                                route.processingBuffer,
                                0,
                                0,
                                samplesThisBlock);
                            destinationBuffer.addFrom(
                                1,
                                0,
                                route.processingBuffer,
                                1,
                                0,
                                samplesThisBlock);
                        }

                        if (route.kind == RouteKind::hardwareOutput)
                        {
                            for (int channel = 0;
                                 channel < route.hardwareChannels;
                                 ++channel)
                            {
                                const auto outputChannel =
                                    route.hardwareFirstChannel + channel;
                                if (outputChannel < 0
                                    || outputChannel >= numOutputChannels
                                    || outputChannelData[outputChannel] == nullptr)
                                    continue;
                                const auto sourceChannel = std::min(channel, 1);
                                for (int sample = 0;
                                     sample < samplesThisBlock;
                                     ++sample)
                                {
                                    outputChannelData[outputChannel]
                                                     [outputOffset + sample]
                                        += route.processingBuffer.getSample(
                                            sourceChannel,
                                            sample);
                                }
                            }
                        }
                    }
                };
                processRoutes(RouteTap::preFader);
                if (!track.volumeAutomation.has_value()
                    && !track.panAutomation.has_value()
                    && !track.muteAutomation.has_value()
                    && !track.polarityAutomation.has_value()
                    && !track.vcaAutomation.has_value())
                {
                    applyTrackGainAndPan(track.processingBuffer,
                                         samplesThisBlock,
                                         track.volumeGain
                                             * track.vcaGain
                                             * (track.polarityInverted
                                                    ? -1.0f
                                                    : 1.0f),
                                         track.pan);
                }
                else
                {
                    for (int sample = 0; sample < samplesThisBlock; ++sample)
                    {
                        const auto absoluteSample = position + sample;
                        if (automatedSwitch(
                                track.muteAutomation,
                                absoluteSample,
                                false))
                        {
                            track.processingBuffer.setSample(0, sample, 0.0f);
                            track.processingBuffer.setSample(1, sample, 0.0f);
                            continue;
                        }
                        const auto gain = automatedVolumeGain(
                            track.volumeAutomation,
                            absoluteSample,
                            track.volumeGain)
                            * automatedVolumeGain(
                                track.vcaAutomation,
                                absoluteSample,
                                track.vcaGain)
                            * (automatedSwitch(
                                   track.polarityAutomation,
                                   absoluteSample,
                                   track.polarityInverted)
                                   ? -1.0f
                                   : 1.0f);
                        const auto pan = automatedPan(
                            track.panAutomation,
                            absoluteSample,
                            track.pan);
                        track.processingBuffer.applyGain(
                            0,
                            sample,
                            1,
                            gain * (pan > 0.0f ? 1.0f - pan : 1.0f));
                        track.processingBuffer.applyGain(
                            1,
                            sample,
                            1,
                            gain * (pan < 0.0f ? 1.0f + pan : 1.0f));
                    }
                }
                publishMeter(true);
                processRoutes(RouteTap::postFader);
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
                    auto& destination = track.destinationIndex >= 0
                        ? snapshot.tracks[static_cast<std::size_t>(
                              track.destinationIndex)].processingBuffer
                        : snapshot.masterBuffer;
                    destination.addFrom(0,
                                        0,
                                        track.processingBuffer,
                                        0,
                                        0,
                                        samplesThisBlock);
                    destination.addFrom(1,
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
            processRuntimeChain(
                snapshot.masterRuntimeKey,
                masterView,
                nullptr,
                &snapshot.masterPluginAutomation,
                position);
            if (snapshot.masterAudible)
            {
                if (!snapshot.masterVolumeAutomation.has_value()
                    && !snapshot.masterPanAutomation.has_value()
                    && !snapshot.masterMuteAutomation.has_value())
                {
                    applyTrackGainAndPan(snapshot.masterBuffer,
                                         samplesThisBlock,
                                         snapshot.masterGain,
                                         snapshot.masterPan);
                }
                else
                {
                    for (int sample = 0; sample < samplesThisBlock; ++sample)
                    {
                        const auto absoluteSample = position + sample;
                        if (automatedSwitch(
                                snapshot.masterMuteAutomation,
                                absoluteSample,
                                false))
                        {
                            snapshot.masterBuffer.setSample(0, sample, 0.0f);
                            snapshot.masterBuffer.setSample(1, sample, 0.0f);
                            continue;
                        }
                        const auto gain = automatedVolumeGain(
                            snapshot.masterVolumeAutomation,
                            absoluteSample,
                            snapshot.masterGain);
                        const auto pan = automatedPan(
                            snapshot.masterPanAutomation,
                            absoluteSample,
                            snapshot.masterPan);
                        snapshot.masterBuffer.applyGain(
                            0,
                            sample,
                            1,
                            gain * (pan > 0.0f ? 1.0f - pan : 1.0f));
                        snapshot.masterBuffer.applyGain(
                            1,
                            sample,
                            1,
                            gain * (pan < 0.0f ? 1.0f + pan : 1.0f));
                    }
                }
            }
            else
                snapshot.masterBuffer.clear();

            snapshot.controlRoomBuffer.clear(0, 0, samplesThisBlock);
            snapshot.controlRoomBuffer.clear(1, 0, samplesThisBlock);
            if (snapshot.controlRoomEnabled && !snapshot.controlRoomMuted)
            {
                snapshot.controlRoomBuffer.copyFrom(
                    0,
                    0,
                    snapshot.masterBuffer,
                    0,
                    0,
                    samplesThisBlock);
                snapshot.controlRoomBuffer.copyFrom(
                    1,
                    0,
                    snapshot.masterBuffer,
                    1,
                    0,
                    samplesThisBlock);
                if (snapshot.controlRoomMono)
                {
                    for (int sample = 0; sample < samplesThisBlock; ++sample)
                    {
                        const auto mono = (snapshot.controlRoomBuffer.getSample(
                                               0,
                                               sample)
                                           + snapshot.controlRoomBuffer.getSample(
                                               1,
                                               sample))
                            * 0.5f;
                        snapshot.controlRoomBuffer.setSample(0, sample, mono);
                        snapshot.controlRoomBuffer.setSample(1, sample, mono);
                    }
                }
                processRuntimeChain(snapshot.controlRoomRuntimeKey,
                                    snapshot.controlRoomBuffer);
                applyTrackGainAndPan(
                    snapshot.controlRoomBuffer,
                    samplesThisBlock,
                    snapshot.controlRoomGain
                        * (snapshot.controlRoomDimmed
                               ? snapshot.controlRoomDimGain
                               : 1.0f),
                    snapshot.controlRoomPan);
            }

            snapshot.clickBuffer.clear(0, 0, samplesThisBlock);
            snapshot.clickBuffer.clear(1, 0, samplesThisBlock);
            if (metronomeEnabled.load(std::memory_order_relaxed))
            {
                for (int sample = 0; sample < samplesThisBlock; ++sample)
                {
                    float clickLeft = 0.0f;
                    float clickRight = 0.0f;
                    auto clickPosition = position + sample;
                    if (renderLooping)
                        clickPosition = wrapLoopPosition(
                            clickPosition,
                            snapshot.loopStartSample,
                            snapshot.loopEndSample);
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

                if (snapshot.controlRoomEnabled)
                {
                    const auto controlOutput =
                        snapshot.controlRoomOutputChannel;
                    if (controlOutput >= 0
                        && controlOutput < numOutputChannels
                        && outputChannelData[controlOutput] != nullptr)
                    {
                        outputChannelData[controlOutput][outputOffset + sample]
                            += snapshot.controlRoomBuffer.getSample(0, sample);
                    }
                    if (controlOutput + 1 < numOutputChannels
                        && outputChannelData[controlOutput + 1] != nullptr)
                    {
                        outputChannelData[controlOutput + 1]
                                         [outputOffset + sample]
                            += snapshot.controlRoomBuffer.getSample(1, sample);
                    }
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
                position = wrapLoopPosition(
                    position,
                    snapshot.loopStartSample,
                    snapshot.loopEndSample);
            }
            else if (!renderingRecording && position >= snapshot.lengthSamples)
            {
                position = snapshot.lengthSamples;
                playing.store(false, std::memory_order_release);
            }
        }

        playheadSample.store(position, std::memory_order_release);
        for (;;)
        {
            const auto currentPair =
                activeRenderPair.load(std::memory_order_acquire);
            const auto currentSnapshotIndex = static_cast<int>(
                (currentPair >> 2) & 0x3);
            if (currentSnapshotIndex == snapshotIndex)
                break;
            snapshotReaders[
                static_cast<std::size_t>(
                    currentSnapshotIndex)].fetch_add(
                1,
                std::memory_order_seq_cst);
            if (currentPair
                == activeRenderPair.load(
                    std::memory_order_acquire))
            {
                mergeSnapshotTransitionHandoffs(
                    snapshots[static_cast<std::size_t>(
                        currentSnapshotIndex)],
                    snapshotIndex,
                    1);
                snapshotReaders[
                    static_cast<std::size_t>(
                        currentSnapshotIndex)].fetch_sub(
                    1,
                    std::memory_order_seq_cst);
                break;
            }
            snapshotReaders[
                static_cast<std::size_t>(
                    currentSnapshotIndex)].fetch_sub(
                1,
                std::memory_order_seq_cst);
        }
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
