#include "StudioAudioEngine.h"

#include <algorithm>
#include <cmath>

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

    outputFile = destination;
    recordingSampleRate = newSampleRate;
    recordingChannels = juce::jlimit(1, 2, channels);
    recordingFirstInputChannel = juce::jmax(0, firstInputChannel);
    fifo.reset();
    ringBuffer.clear();
    samplesWritten.store(0, std::memory_order_relaxed);
    samplesDropped.store(0, std::memory_order_relaxed);
    accepting.store(true, std::memory_order_release);
    startThread();
    return juce::Result::ok();
}

StudioAudioEngine::RecordingResult StudioAudioEngine::LockFreeRecorder::stop()
{
    RecordingResult result;
    result.file = outputFile;

    if (!accepting.exchange(false, std::memory_order_acq_rel) && !isThreadRunning())
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
        result.result = juce::Result::fail("Recording stopped after dropping "
                                           + juce::String(dropped)
                                           + " samples because disk writing fell behind.");

    return result;
}

void StudioAudioEngine::LockFreeRecorder::push(const float* const* inputs,
                                               int inputChannels,
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
                ringBuffer.copyFrom(channel, destinationStart, source + sourceStart, count);
            else
                ringBuffer.clear(channel, destinationStart, count);
        }
    };

    copyRegion(start1, size1, 0);
    copyRegion(start2, size2, size1);
    fifo.finishedWrite(size1 + size2);
}

bool StudioAudioEngine::LockFreeRecorder::isActive() const noexcept
{
    return accepting.load(std::memory_order_acquire);
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
            writeSucceeded = writer->writeFromAudioSampleBuffer(ringBuffer, start1, size1);
        if (writer != nullptr && size2 > 0)
            writeSucceeded = writer->writeFromAudioSampleBuffer(ringBuffer, start2, size2)
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

    const auto error = manager.initialiseWithDefaultDevices(1, 2);
    if (error.isNotEmpty())
        return juce::Result::fail("Audio device setup failed: " + error);

    deviceManager = &manager;
    deviceManager->addAudioCallback(this);
    return juce::Result::ok();
}

void StudioAudioEngine::shutdown()
{
    recorder.stop();
    playing.store(false, std::memory_order_release);

    if (deviceManager != nullptr)
    {
        deviceManager->removeAudioCallback(this);
        deviceManager = nullptr;
    }
}

juce::Result StudioAudioEngine::updateProject(const Project& project)
{
    juce::String error;
    auto snapshot = buildSnapshot(project, currentSampleRate(), error);
    if (!snapshot.has_value())
        return juce::Result::fail(error);

    const auto destination = chooseWritableSnapshot();
    snapshots[static_cast<std::size_t>(destination)] = std::move(*snapshot);
    activeSnapshot.store(destination, std::memory_order_release);
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
    return recorder.isActive();
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

juce::Result StudioAudioEngine::startRecording(const juce::File& destination,
                                               int firstInputChannel,
                                               int channels)
{
    if (deviceManager == nullptr)
        return juce::Result::fail("No audio device is available.");

    const auto* device = deviceManager->getCurrentAudioDevice();
    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
        return juce::Result::fail("Enable at least one audio input before recording.");

    const auto activeInputs = device->getActiveInputChannels().countNumberOfSetBits();
    const auto captureChannels = juce::jlimit(1, 2, channels);
    if (firstInputChannel < 0 || firstInputChannel + captureChannels > activeInputs)
        return juce::Result::fail("The selected track input is not enabled in audio I/O settings.");

    return recorder.start(destination,
                          currentSampleRate(),
                          captureChannels,
                          firstInputChannel);
}

StudioAudioEngine::RecordingResult StudioAudioEngine::stopRecording()
{
    return recorder.stop();
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

juce::Result StudioAudioEngine::renderToWav(const Project& project,
                                            const juce::File& destination,
                                            double renderSampleRate)
{
    juce::String error;
    auto snapshot = buildSnapshot(project, renderSampleRate, error);
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
    juce::String& error)
{
    RenderSnapshot snapshot;
    snapshot.sampleRate = targetSampleRate;
    snapshot.lengthSamples = static_cast<std::int64_t>(std::ceil(project.lengthSeconds() * targetSampleRate));
    snapshot.loopStartSample = static_cast<std::int64_t>(project.loopStartSeconds * targetSampleRate);
    snapshot.loopEndSample = static_cast<std::int64_t>(project.loopEndSeconds * targetSampleRate);
    snapshot.tempo = project.tempo;
    snapshot.loopEnabled = project.loopEnabled && snapshot.loopEndSample > snapshot.loopStartSample;

    const auto anySolo = std::any_of(project.tracks.cbegin(), project.tracks.cend(), [](const auto& track)
    {
        return track.solo && track.type != TrackType::master;
    });

    float masterGain = 1.0f;
    if (const auto master = std::find_if(project.tracks.cbegin(), project.tracks.cend(), [](const auto& track)
        {
            return track.type == TrackType::master;
        }); master != project.tracks.cend())
        masterGain = master->muted ? 0.0f : juce::Decibels::decibelsToGain(master->volumeDecibels);

    for (const auto& track : project.tracks)
    {
        if (track.type == TrackType::master || track.muted || (anySolo && !track.solo))
            continue;

        const auto trackGain = juce::Decibels::decibelsToGain(track.volumeDecibels) * masterGain;
        const auto leftPanGain = track.pan > 0.0f ? 1.0f - track.pan : 1.0f;
        const auto rightPanGain = track.pan < 0.0f ? 1.0f + track.pan : 1.0f;

        for (const auto& clip : track.clips)
        {
            if (clip.muted || !clip.sourceFile.existsAsFile())
                continue;

            auto audio = readAndResample(clip.sourceFile, targetSampleRate, error);
            if (!audio.has_value())
                return std::nullopt;

            RenderClip renderClip;
            renderClip.startSample = static_cast<std::int64_t>(std::llround(clip.startSeconds * targetSampleRate));
            renderClip.sourceOffsetSamples = static_cast<std::int64_t>(
                std::llround(clip.sourceOffsetSeconds * targetSampleRate));
            renderClip.lengthSamples = static_cast<std::int64_t>(
                std::llround(clip.durationSeconds * targetSampleRate));
            const auto clipGain = juce::Decibels::decibelsToGain(clip.gainDecibels) * trackGain;
            renderClip.leftGain = clipGain * leftPanGain;
            renderClip.rightGain = clipGain * rightPanGain;
            renderClip.samples = std::move(*audio);

            const auto available = static_cast<std::int64_t>(renderClip.samples.getNumSamples())
                - renderClip.sourceOffsetSamples;
            renderClip.lengthSamples = std::max<std::int64_t>(0,
                                                               std::min(renderClip.lengthSamples, available));
            if (renderClip.lengthSamples > 0)
                snapshot.clips.push_back(std::move(renderClip));
        }
    }

    return snapshot;
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

void StudioAudioEngine::mixSample(const RenderSnapshot& snapshot,
                                  std::int64_t timelineSample,
                                  float& left,
                                  float& right) noexcept
{
    for (const auto& clip : snapshot.clips)
    {
        const auto relative = timelineSample - clip.startSample;
        if (relative < 0 || relative >= clip.lengthSamples)
            continue;

        const auto sourceSample = clip.sourceOffsetSamples + relative;
        if (sourceSample < 0 || sourceSample >= clip.samples.getNumSamples())
            continue;

        const auto index = static_cast<int>(sourceSample);
        const auto sourceLeft = clip.samples.getSample(0, index);
        const auto sourceRight = clip.samples.getNumChannels() > 1
            ? clip.samples.getSample(1, index)
            : sourceLeft;
        left += sourceLeft * clip.leftGain;
        right += sourceRight * clip.rightGain;
    }
}

void StudioAudioEngine::addMetronome(const RenderSnapshot& snapshot,
                                     std::int64_t timelineSample,
                                     float& left,
                                     float& right) noexcept
{
    const auto samplesPerBeat = snapshot.sampleRate * 60.0 / snapshot.tempo;
    const auto beatPosition = std::fmod(static_cast<double>(timelineSample), samplesPerBeat);
    constexpr auto clickDurationSeconds = 0.018;
    const auto clickSamples = snapshot.sampleRate * clickDurationSeconds;
    if (beatPosition >= clickSamples)
        return;

    const auto beat = static_cast<std::int64_t>(static_cast<double>(timelineSample) / samplesPerBeat);
    const auto frequency = beat % 4 == 0 ? 1760.0 : 1320.0;
    const auto envelope = static_cast<float>(1.0 - beatPosition / clickSamples);
    const auto click = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi
                                                   * frequency
                                                   * beatPosition
                                                   / snapshot.sampleRate))
        * envelope
        * 0.18f;
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

void StudioAudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                         int numInputChannels,
                                                         float* const* outputChannelData,
                                                         int numOutputChannels,
                                                         int numSamples,
                                                         const juce::AudioIODeviceCallbackContext&)
{
    recorder.push(inputChannelData, numInputChannels, numSamples);

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
    if (playing.load(std::memory_order_acquire))
    {
        int snapshotIndex = 0;
        do
        {
            snapshotIndex = activeSnapshot.load(std::memory_order_acquire);
            readingSnapshot.store(snapshotIndex, std::memory_order_release);
        }
        while (snapshotIndex != activeSnapshot.load(std::memory_order_acquire));

        const auto& snapshot = snapshots[static_cast<std::size_t>(snapshotIndex)];
        auto position = playheadSample.load(std::memory_order_acquire);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (position >= snapshot.lengthSamples)
            {
                playing.store(false, std::memory_order_release);
                break;
            }

            float left = 0.0f;
            float right = 0.0f;
            mixSample(snapshot, position, left, right);
            if (metronomeEnabled.load(std::memory_order_relaxed))
                addMetronome(snapshot, position, left, right);

            if (outputChannelData[0] != nullptr)
                outputChannelData[0][sample] = left;
            if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
                outputChannelData[1][sample] = right;

            ++position;
            if (snapshot.loopEnabled && position >= snapshot.loopEndSample)
                position = snapshot.loopStartSample;
        }

        playheadSample.store(position, std::memory_order_release);
        readingSnapshot.store(-1, std::memory_order_release);
    }

    if (monitoringEnabled.load(std::memory_order_acquire))
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

    for (int sample = 0; sample < numSamples; ++sample)
    {
        if (outputChannelData[0] != nullptr)
        {
            outputChannelData[0][sample] = juce::jlimit(-1.0f, 1.0f, outputChannelData[0][sample]);
            leftPeakValue = std::max(leftPeakValue, std::abs(outputChannelData[0][sample]));
        }
        if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
        {
            outputChannelData[1][sample] = juce::jlimit(-1.0f, 1.0f, outputChannelData[1][sample]);
            rightPeakValue = std::max(rightPeakValue, std::abs(outputChannelData[1][sample]));
        }
    }

    outputLeftPeak.store(leftPeakValue, std::memory_order_relaxed);
    outputRightPeak.store(rightPeakValue, std::memory_order_relaxed);
}

void StudioAudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate.store(device != nullptr ? device->getCurrentSampleRate() : 48000.0,
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
