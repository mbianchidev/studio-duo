#include "PluginBridgeClient.h"

#include <limits>
#include <new>

namespace studio
{
PluginBridgeClient::PluginBridgeClient(
    juce::File executable)
    : workerExecutable(std::move(executable))
{
}

PluginBridgeClient::~PluginBridgeClient()
{
    stop();
}

juce::Result PluginBridgeClient::start()
{
    return startInternal(nullptr, 48000.0, 512, {}, 0);
}

juce::Result PluginBridgeClient::startPlugin(const juce::PluginDescription& description,
                                             double pluginSampleRate,
                                             int blockSize,
                                             const juce::MemoryBlock& state,
                                             int requestedSidechainChannels)
{
    return startInternal(&description,
                         pluginSampleRate,
                         blockSize,
                         state,
                         requestedSidechainChannels);
}

juce::Result PluginBridgeClient::startInternal(const juce::PluginDescription* description,
                                               double pluginSampleRate,
                                               int blockSize,
                                               const juce::MemoryBlock& state,
                                               int requestedSidechainChannels)
{
    stop();

    if (pluginSampleRate <= 0.0
        || blockSize <= 0
        || blockSize > PluginBridgeSharedState::maxBlockSize)
        return juce::Result::fail("Plugin bridge received invalid audio settings.");

    if (const auto result = createSharedFile(); result.failed())
        return result;

    connectionLost.store(false, std::memory_order_release);
    const auto executable = workerExecutable.existsAsFile()
        ? workerExecutable
        : juce::File::getSpecialLocation(
              juce::File::currentExecutableFile);
    if (!launchWorkerProcess(executable,
                             pluginBridgeProcessId,
                             5000,
                             0))
    {
        stop();
        return juce::Result::fail("Could not launch the plugin bridge worker.");
    }

    juce::MemoryBlock request;
    juce::MemoryOutputStream stream(request, true);
    stream.writeString(sharedFile.getFullPathName());
    stream.writeString(description != nullptr ? description->createXml()->toString()
                                              : juce::String());
    stream.writeString(state.toBase64Encoding());
    stream.writeDouble(pluginSampleRate);
    stream.writeInt(PluginBridgeSharedState::maxBlockSize);
    stream.writeInt(std::max(0, requestedSidechainChannels));
    if (!sendMessageToWorker(request))
    {
        stop();
        return juce::Result::fail("Could not configure the plugin bridge worker.");
    }

    {
        std::unique_lock lock(responseMutex);
        if (!responseCondition.wait_for(lock,
                                        std::chrono::seconds(5),
                                        [this]
                                        {
                                            return responseMessage.isNotEmpty()
                                                || connectionLost.load(std::memory_order_acquire);
                                        }))
        {
            stop();
            return juce::Result::fail("Plugin bridge worker did not respond.");
        }
    }

    const auto responseParts = juce::StringArray::fromTokens(responseMessage, "|", "");
    if (responseParts.isEmpty() || responseParts[0] != "ready")
    {
        const auto error = responseMessage.isNotEmpty() ? responseMessage
                                                        : "worker disconnected";
        stop();
        return juce::Result::fail("Plugin bridge setup failed: " + error);
    }

    const auto reportedLatency = responseParts.size() > 1
        ? responseParts[1].getIntValue()
        : 0;
    if (reportedLatency < 0
        || reportedLatency
            > PluginBridgeSharedState::maxSupportedLatencySamples)
    {
        stop();
        return juce::Result::fail(
            "Plugin bridge reported unsupported latency: "
            + juce::String(reportedLatency)
            + " samples.");
    }
    pluginLatencySamples.store(reportedLatency,
                               std::memory_order_release);
    pluginTailSeconds.store(responseParts.size() > 2 ? responseParts[2].getDoubleValue() : 0.0,
                            std::memory_order_release);
    if (responseParts.size() > 3)
        updateParameterMetadata(responseParts[3]);
    ready.store(true, std::memory_order_release);
    processingBlockSize = PluginBridgeSharedState::maxBlockSize;
    prepareOutputTimeline(blockSize);
    completedOutputSamples = 0;
    nextInputSequence = 0;
    inFlightSequence = -1;
    completedSequence = -1;
    completedOutputFlags = 0;
    inFlightMissedDeadline = false;
    timelineResetPending = false;
    for (auto& channel : inputAccumulator)
        channel.fill(0.0f);
    for (auto& channel : sidechainAccumulator)
        channel.fill(0.0f);
    for (auto& channel : completedOutput)
        channel.fill(0.0f);
    return juce::Result::ok();
}

void PluginBridgeClient::stop()
{
    ready.store(false, std::memory_order_release);
    if (sharedState != nullptr)
        sharedState->shutdownRequested.store(1, std::memory_order_release);

    killWorkerProcess();
    sharedState = nullptr;
    mapping.reset();
    if (sharedFile.existsAsFile())
        sharedFile.deleteFile();
    sharedFile = juce::File();
    lastOutputSequence = 0;
    completedOutputSamples = 0;
    nextInputSequence = 0;
    inFlightSequence = -1;
    inFlightStartSample = 0;
    completedSequence = -1;
    completedStartSample = 0;
    completedOutputFlags = 0;
    streamSamplePosition = 0;
    wetReplacementBlockedUntilSample = 0;
    inFlightMissedDeadline = false;
    timelineResetPending = false;
    outputTimeline.setSize(
        PluginBridgeSharedState::maxChannels,
        1,
        false,
        true,
        false);
    outputTimeline.clear();
    outputTimelineCapacity = 1;
    responseMessage.clear();
    pluginLatencySamples.store(0, std::memory_order_relaxed);
    pluginTailSeconds.store(0.0, std::memory_order_relaxed);
    {
        const std::lock_guard lock(parameterMutex);
        parameters.clear();
    }
}

juce::Result PluginBridgeClient::requestState(
    juce::MemoryBlock& state,
    std::chrono::milliseconds timeout)
{
    const std::lock_guard commandLock(commandMutex);
    state.reset();
    if (!isReady())
        return juce::Result::fail("Plugin bridge is not ready.");

    {
        const std::lock_guard lock(responseMutex);
        responseMessage.clear();
    }
    juce::MemoryBlock request;
    juce::MemoryOutputStream stream(request, true);
    stream.writeString("get-state");
    if (!sendMessageToWorker(request))
        return juce::Result::fail(
            "Could not request state from the plugin bridge.");

    {
        std::unique_lock lock(responseMutex);
        if (!responseCondition.wait_for(
                lock,
                timeout,
                [this]
                {
                    return responseMessage.isNotEmpty()
                        || connectionLost.load(std::memory_order_acquire);
                }))
            return juce::Result::fail(
                "Plugin state request timed out.");
    }

    if (connectionLost.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Plugin bridge disconnected while saving state.");
    if (responseMessage.startsWith("state|error|"))
        return juce::Result::fail(
            "Plugin state save failed: "
            + responseMessage.fromFirstOccurrenceOf(
                  "state|error|",
                  false,
                  false));
    if (responseMessage == "state|none")
        return juce::Result::ok();
    if (!responseMessage.startsWith("state|data|"))
        return juce::Result::fail(
            responseMessage.isNotEmpty()
                ? responseMessage
                : "Plugin bridge returned no state.");
    const auto encoded =
        responseMessage.fromFirstOccurrenceOf(
            "state|data|",
            false,
            false);
    if (encoded.isEmpty())
        return juce::Result::fail(
            "Plugin bridge returned empty state data.");
    if (!state.fromBase64Encoding(encoded))
        return juce::Result::fail(
            "Plugin bridge returned invalid state data.");
    return juce::Result::ok();
}

juce::Result PluginBridgeClient::showEditor()
{
    return sendEditorCommand("show-editor");
}

juce::Result PluginBridgeClient::hideEditor()
{
    return sendEditorCommand("hide-editor");
}

juce::Result PluginBridgeClient::focusEditor()
{
    return sendEditorCommand("focus-editor");
}

juce::Result PluginBridgeClient::resizeEditor(int width, int height)
{
    if (width <= 0 || height <= 0)
        return juce::Result::fail(
            "Plugin editor dimensions must be positive.");
    return sendEditorCommand("resize-editor", width, height);
}

bool PluginBridgeClient::setParameter(int parameterIndex,
                                      float normalizedValue)
{
    if (!isReady() || parameterIndex < 0)
        return false;
    juce::MemoryBlock request;
    juce::MemoryOutputStream stream(request, true);
    stream.writeString("set-parameter");
    stream.writeInt(parameterIndex);
    stream.writeFloat(juce::jlimit(0.0f, 1.0f, normalizedValue));
    return sendMessageToWorker(request);
}

juce::Result PluginBridgeClient::sendEditorCommand(
    const juce::String& command,
    int width,
    int height)
{
    const std::lock_guard commandLock(commandMutex);
    if (!isReady())
        return juce::Result::fail("Plugin bridge is not ready.");
    {
        const std::lock_guard lock(responseMutex);
        responseMessage.clear();
    }

    juce::MemoryBlock request;
    juce::MemoryOutputStream stream(request, true);
    stream.writeString(command);
    if (command == "resize-editor")
    {
        stream.writeInt(width);
        stream.writeInt(height);
    }
    if (!sendMessageToWorker(request))
        return juce::Result::fail(
            "Could not send the plugin editor command.");

    std::unique_lock lock(responseMutex);
    if (!responseCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [this]
            {
                return responseMessage.isNotEmpty()
                    || connectionLost.load(std::memory_order_acquire);
            }))
    {
        return juce::Result::fail(
            "Plugin editor command timed out.");
    }
    if (connectionLost.load(std::memory_order_acquire))
        return juce::Result::fail(
            "Plugin bridge disconnected while controlling the editor.");
    if (responseMessage.startsWith("editor|error|"))
    {
        return juce::Result::fail(
            responseMessage.fromLastOccurrenceOf("|", false, false));
    }
    return responseMessage.startsWith("editor|")
        ? juce::Result::ok()
        : juce::Result::fail(
              "Plugin bridge returned an invalid editor response.");
}

void PluginBridgeClient::processBlock(
    juce::AudioBuffer<float>& audio,
    const juce::AudioBuffer<float>* sidechain,
    std::span<const PluginBridgeParameterEvent> parameterEvents) noexcept
{
    if (!isReady() || sharedState == nullptr)
    {
        audio.clear();
        return;
    }
    const auto channels = std::min(audio.getNumChannels(), PluginBridgeSharedState::maxChannels);
    const auto samples = std::min(audio.getNumSamples(),
                                  PluginBridgeSharedState::maxBlockSize);
    for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
    {
        auto& destination = inputAccumulator[static_cast<std::size_t>(channel)];
        if (channel < channels)
            std::copy_n(audio.getReadPointer(channel), samples, destination.begin());
        else
            std::fill_n(destination.begin(), samples, 0.0f);
    }
    inputChannels = channels;
    sidechainChannels = sidechain != nullptr
        ? std::min(sidechain->getNumChannels(),
                   PluginBridgeSharedState::maxChannels)
        : 0;
    for (int channel = 0;
         channel < PluginBridgeSharedState::maxChannels;
         ++channel)
    {
        auto& destination =
            sidechainAccumulator[static_cast<std::size_t>(channel)];
        if (sidechain != nullptr && channel < sidechainChannels)
            std::copy_n(sidechain->getReadPointer(channel),
                        samples,
                        destination.begin());
        else
            std::fill_n(destination.begin(), samples, 0.0f);
    }
    pendingParameterEventCount = 0;
    for (const auto& event : parameterEvents)
    {
        auto sanitized = event;
        sanitized.sampleOffset = static_cast<std::uint32_t>(
            std::min(samples - 1,
                     static_cast<int>(sanitized.sampleOffset)));
        sanitized.value = juce::jlimit(0.0f, 1.0f, sanitized.value);
        if (pendingParameterEventCount
            < PluginBridgeSharedState::maxParameterEvents)
        {
            pendingParameterEvents[static_cast<std::size_t>(
                pendingParameterEventCount++)] = sanitized;
            continue;
        }

        const auto existing = std::find_if(
            pendingParameterEvents.begin(),
            pendingParameterEvents.end(),
            [&sanitized](const auto& candidate)
            {
                return candidate.parameterIndex == sanitized.parameterIndex;
            });
        if (existing != pendingParameterEvents.end())
            *existing = sanitized;
        else
            pendingParameterEvents.back() = sanitized;
        sharedState->parameterEventOverflowCount.fetch_add(
            1,
            std::memory_order_relaxed);
    }
    std::sort(
        pendingParameterEvents.begin(),
        pendingParameterEvents.begin() + pendingParameterEventCount,
        [](const auto& left, const auto& right)
        {
            return left.sampleOffset < right.sampleOffset;
        });

    queueDryInput(samples);
    fetchWorkerOutput();
    queueCompletedOutput();

#if STUDIO_DUO_TESTING
    if (beforeSecondFetchForTesting)
        beforeSecondFetchForTesting();
#endif
    fetchWorkerOutput();
    queueCompletedOutput();
    readTimelineOutput(audio);
    if (inFlightSequence < 0
        && sharedState->workerSequence.load(std::memory_order_acquire)
            == sharedState->hostSequence.load(std::memory_order_relaxed))
        publishInputBlock(samples);
    else
    {
        timelineResetPending = true;
        lateBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    streamSamplePosition += samples;
}

void PluginBridgeClient::fetchWorkerOutput() noexcept
{
    const auto workerSequence = sharedState->workerSequence.load(std::memory_order_acquire);
    if (workerSequence == lastOutputSequence)
        return;

    const auto outputChannels = std::min(
        static_cast<int>(
            sharedState->numOutputChannels.load(
                std::memory_order_relaxed)),
        PluginBridgeSharedState::maxChannels);
    const auto outputSize = std::min(
        static_cast<int>(sharedState->numSamples.load(std::memory_order_relaxed)),
        PluginBridgeSharedState::maxBlockSize);
    if (inFlightSequence >= 0)
    {
        for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        {
            const auto sourceChannel =
                PluginBridgeProtocol::outputSourceChannel(
                    outputChannels,
                    channel);
            auto& destination = completedOutput[static_cast<std::size_t>(channel)];
            if (sourceChannel >= 0)
                std::copy_n(sharedState->output[static_cast<std::size_t>(sourceChannel)].begin(),
                            outputSize,
                            destination.begin());
            else
                std::fill_n(destination.begin(), outputSize, 0.0f);
        }
        completedSequence = inFlightSequence;
        completedStartSample = inFlightStartSample;
        completedOutputSamples = outputSize;
        completedOutputFlags = sharedState->outputFlags.load(
            std::memory_order_relaxed);
    }

    inFlightSequence = -1;
    inFlightMissedDeadline = false;
    lastOutputSequence = workerSequence;
}

#if STUDIO_DUO_TESTING
bool PluginBridgeClient::recoversLateFirstOutputForTesting()
{
    const auto completeWorkerBlock = [](PluginBridgeSharedState& state,
                                        float outputValue,
                                        std::uint32_t outputFlags = 0)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            std::fill_n(
                state.output[static_cast<std::size_t>(channel)].begin(),
                4,
                outputValue);
        }
        state.numOutputChannels.store(2, std::memory_order_relaxed);
        state.outputFlags.store(outputFlags, std::memory_order_relaxed);
        state.workerSequence.store(
            state.hostSequence.load(std::memory_order_relaxed),
            std::memory_order_release);
    };
    const auto initialise = [](PluginBridgeClient& client,
                               PluginBridgeSharedState& state)
    {
        client.sharedState = &state;
        client.ready.store(true, std::memory_order_release);
        client.processingBlockSize = 4;
        client.prepareOutputTimeline(4);
        juce::AudioBuffer<float> first(2, 4);
        first.clear();
        first.addSample(0, 0, 0.25f);
        first.addSample(1, 0, 0.25f);
        client.processBlock(first);
    };
    const auto recovered = [](const juce::AudioBuffer<float>& audio)
    {
        return std::abs(audio.getSample(0, 0) - 0.25f) < 0.0001f
            && std::abs(audio.getSample(1, 0) - 0.25f) < 0.0001f;
    };

    auto sameCallbackState =
        std::make_unique<PluginBridgeSharedState>();
    auto sameCallbackClient =
        std::make_unique<PluginBridgeClient>();
    initialise(*sameCallbackClient, *sameCallbackState);
    sameCallbackClient->beforeSecondFetchForTesting =
        [&sameCallbackState, &completeWorkerBlock]
        {
            completeWorkerBlock(*sameCallbackState, 0.75f);
        };
    juce::AudioBuffer<float> sameCallbackOutput(2, 4);
    sameCallbackOutput.clear();
    sameCallbackClient->processBlock(sameCallbackOutput);
    sameCallbackClient->sharedState = nullptr;

    auto laterCallbackState =
        std::make_unique<PluginBridgeSharedState>();
    auto laterCallbackClient =
        std::make_unique<PluginBridgeClient>();
    initialise(*laterCallbackClient, *laterCallbackState);
    juce::AudioBuffer<float> missedDeadline(2, 4);
    missedDeadline.clear();
    missedDeadline.addSample(0, 0, 0.25f);
    missedDeadline.addSample(1, 0, 0.25f);
    laterCallbackClient->processBlock(missedDeadline);
    completeWorkerBlock(*laterCallbackState, 0.75f);
    juce::AudioBuffer<float> laterCallbackOutput(2, 4);
    laterCallbackOutput.clear();
    laterCallbackClient->processBlock(laterCallbackOutput);
    laterCallbackClient->sharedState = nullptr;

    auto onTimeState =
        std::make_unique<PluginBridgeSharedState>();
    auto onTimeClient =
        std::make_unique<PluginBridgeClient>();
    initialise(*onTimeClient, *onTimeState);
    completeWorkerBlock(*onTimeState, 0.75f);
    juce::AudioBuffer<float> onTimeOutput(2, 4);
    onTimeOutput.clear();
    onTimeClient->processBlock(onTimeOutput);
    onTimeClient->sharedState = nullptr;

    auto bypassState =
        std::make_unique<PluginBridgeSharedState>();
    auto bypassClient =
        std::make_unique<PluginBridgeClient>();
    initialise(*bypassClient, *bypassState);
    completeWorkerBlock(
        *bypassState,
        0.75f,
        PluginBridgeSharedState::bypassOutputFlag);
    juce::AudioBuffer<float> bypassOutput(2, 4);
    bypassOutput.clear();
    bypassClient->processBlock(bypassOutput);
    bypassClient->sharedState = nullptr;

    auto variableState =
        std::make_unique<PluginBridgeSharedState>();
    auto variableClient =
        std::make_unique<PluginBridgeClient>();
    variableClient->sharedState = variableState.get();
    variableClient->ready.store(true, std::memory_order_release);
    variableClient->processingBlockSize = 4;
    variableClient->prepareOutputTimeline(4);
    juce::AudioBuffer<float> variableFirst(2, 4);
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < 4; ++sample)
            variableFirst.setSample(
                channel,
                sample,
                static_cast<float>(sample + 1) * 0.1f);
    }
    variableClient->processBlock(variableFirst);
    juce::AudioBuffer<float> variableSecond(2, 2);
    variableSecond.clear();
    variableClient->processBlock(variableSecond);
    juce::AudioBuffer<float> variableThird(2, 2);
    variableThird.clear();
    variableClient->processBlock(variableThird);
    variableClient->sharedState = nullptr;

    auto largerCallbackState =
        std::make_unique<PluginBridgeSharedState>();
    auto largerCallbackClient =
        std::make_unique<PluginBridgeClient>();
    largerCallbackClient->sharedState = largerCallbackState.get();
    largerCallbackClient->ready.store(true, std::memory_order_release);
    largerCallbackClient->processingBlockSize =
        PluginBridgeSharedState::maxBlockSize;
    largerCallbackClient->prepareOutputTimeline(2);
    juce::AudioBuffer<float> largerFirst(2, 4);
    largerFirst.clear();
    largerCallbackClient->processBlock(largerFirst);
    completeWorkerBlock(*largerCallbackState, 0.75f);
    juce::AudioBuffer<float> largerSecond(2, 4);
    largerSecond.clear();
    largerCallbackClient->processBlock(largerSecond);
    largerCallbackClient->sharedState = nullptr;

    auto resetLatencyState =
        std::make_unique<PluginBridgeSharedState>();
    auto resetLatencyClient =
        std::make_unique<PluginBridgeClient>();
    resetLatencyClient->sharedState = resetLatencyState.get();
    resetLatencyClient->ready.store(true, std::memory_order_release);
    resetLatencyClient->processingBlockSize =
        PluginBridgeSharedState::maxBlockSize;
    resetLatencyClient->pluginLatencySamples.store(
        4,
        std::memory_order_release);
    resetLatencyClient->prepareOutputTimeline(4);
    juce::AudioBuffer<float> resetFirst(2, 4);
    resetFirst.clear();
    resetFirst.addSample(0, 0, 0.1f);
    resetFirst.addSample(1, 0, 0.1f);
    resetLatencyClient->processBlock(resetFirst);
    juce::AudioBuffer<float> resetSkipped(2, 4);
    resetSkipped.clear();
    resetSkipped.addSample(0, 0, 0.25f);
    resetSkipped.addSample(1, 0, 0.25f);
    resetLatencyClient->processBlock(resetSkipped);
    completeWorkerBlock(*resetLatencyState, 0.75f);
    juce::AudioBuffer<float> resetResume(2, 4);
    resetResume.clear();
    resetResume.addSample(0, 0, 0.4f);
    resetResume.addSample(1, 0, 0.4f);
    resetLatencyClient->processBlock(resetResume);
    completeWorkerBlock(*resetLatencyState, 0.0f);
    juce::AudioBuffer<float> resetWarmup(2, 4);
    resetWarmup.clear();
    resetLatencyClient->processBlock(resetWarmup);
    resetLatencyClient->sharedState = nullptr;

    return std::abs(sameCallbackOutput.getSample(0, 0) - 0.75f)
            < 0.0001f
        && recovered(laterCallbackOutput)
        && std::abs(onTimeOutput.getSample(0, 0) - 0.75f)
            < 0.0001f
        && recovered(bypassOutput)
        && bypassState->resetRequested.load(
               std::memory_order_acquire)
            == 1
        && std::abs(variableSecond.getSample(0, 0) - 0.1f)
            < 0.0001f
        && std::abs(variableSecond.getSample(0, 1) - 0.2f)
            < 0.0001f
        && std::abs(variableThird.getSample(0, 0) - 0.3f)
            < 0.0001f
        && std::abs(variableThird.getSample(0, 1) - 0.4f)
            < 0.0001f
        && std::abs(largerSecond.getSample(0, 0) - 0.75f)
            < 0.0001f
        && std::abs(largerSecond.getSample(0, 1) - 0.75f)
            < 0.0001f
        && std::abs(resetWarmup.getSample(0, 0) - 0.25f)
            < 0.0001f
        && laterCallbackState->resetRequested.load(
               std::memory_order_acquire)
            == 1
        && laterCallbackClient->lateBlockCount() > 0;
}
#endif

void PluginBridgeClient::publishInputBlock(int samples) noexcept
{
    const auto hostSequence = sharedState->hostSequence.load(std::memory_order_relaxed);
    for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        std::copy_n(inputAccumulator[static_cast<std::size_t>(channel)].begin(),
                    samples,
                    sharedState->input[static_cast<std::size_t>(channel)].begin());
    for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        std::copy_n(
            sidechainAccumulator[static_cast<std::size_t>(channel)].begin(),
            samples,
            sharedState->sidechain[static_cast<std::size_t>(channel)].begin());
    for (int event = 0; event < pendingParameterEventCount; ++event)
        sharedState->parameterEvents[static_cast<std::size_t>(event)]
            = pendingParameterEvents[static_cast<std::size_t>(event)];

    sharedState->numChannels.store(static_cast<std::uint32_t>(inputChannels),
                                   std::memory_order_relaxed);
    sharedState->sidechainChannels.store(
        static_cast<std::uint32_t>(sidechainChannels),
        std::memory_order_relaxed);
    sharedState->numSamples.store(static_cast<std::uint32_t>(samples),
                                  std::memory_order_relaxed);
    sharedState->parameterEventCount.store(
        static_cast<std::uint32_t>(pendingParameterEventCount),
        std::memory_order_relaxed);
    if (timelineResetPending)
    {
        sharedState->resetRequested.store(1, std::memory_order_relaxed);
        wetReplacementBlockedUntilSample =
            streamSamplePosition
            + bridgeQuantumSamples
            + pluginLatencySamples.load(std::memory_order_relaxed);
        timelineResetPending = false;
    }
    sharedState->hostSequence.store(hostSequence + 1, std::memory_order_release);
    inFlightSequence = nextInputSequence++;
    inFlightStartSample = streamSamplePosition;
    inFlightMissedDeadline = false;
}

void PluginBridgeClient::prepareOutputTimeline(int blockSize)
{
    bridgeQuantumSamples = std::max(1, blockSize);
    const auto maximumDelay = static_cast<std::int64_t>(
        bridgeQuantumSamples)
        + PluginBridgeSharedState::maxSupportedLatencySamples;
    outputTimelineCapacity = static_cast<int>(
        std::min<std::int64_t>(
            std::numeric_limits<int>::max(),
            maximumDelay
                + PluginBridgeSharedState::maxBlockSize * 4
                + 1));
    outputTimeline.setSize(
        PluginBridgeSharedState::maxChannels,
        std::max(1, outputTimelineCapacity),
        false,
        true,
        false);
    outputTimeline.clear();
    streamSamplePosition = 0;
    inFlightStartSample = 0;
    completedStartSample = 0;
    wetReplacementBlockedUntilSample = 0;
}

void PluginBridgeClient::queueDryInput(int samples) noexcept
{
    const auto dryDelay = bridgeQuantumSamples
        + juce::jlimit(
            0,
            PluginBridgeSharedState::maxSupportedLatencySamples,
            pluginLatencySamples.load(std::memory_order_relaxed));
    if (dryDelay
        > outputTimelineCapacity
            - PluginBridgeSharedState::maxBlockSize
            - 1)
        return;
    const auto targetStart = streamSamplePosition + dryDelay;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto position = static_cast<int>(
            (targetStart + sample) % outputTimelineCapacity);
        for (int channel = 0;
             channel < outputTimeline.getNumChannels();
             ++channel)
        {
            outputTimeline.setSample(
                channel,
                position,
                inputAccumulator[static_cast<std::size_t>(channel)]
                                [static_cast<std::size_t>(sample)]);
        }
    }
}

void PluginBridgeClient::queueCompletedOutput() noexcept
{
    if (completedSequence < 0)
        return;
    if ((completedOutputFlags
         & PluginBridgeSharedState::bypassOutputFlag)
        != 0)
    {
        timelineResetPending = true;
        completedSequence = -1;
        completedOutputFlags = 0;
        return;
    }
    if ((completedOutputFlags
         & PluginBridgeSharedState::resetAppliedFlag)
        != 0)
    {
        wetReplacementBlockedUntilSample = std::max(
            wetReplacementBlockedUntilSample,
            completedStartSample
                + bridgeQuantumSamples
                + pluginLatencySamples.load(
                    std::memory_order_relaxed));
    }
    if (timelineResetPending)
    {
        completedSequence = -1;
        return;
    }

    const auto targetStart = completedStartSample
        + bridgeQuantumSamples;
    for (int sample = 0; sample < completedOutputSamples; ++sample)
    {
        const auto absolutePosition = targetStart + sample;
        if (absolutePosition < streamSamplePosition
            || absolutePosition < wetReplacementBlockedUntilSample)
            continue;
        const auto position = static_cast<int>(
            absolutePosition % outputTimelineCapacity);
        for (int channel = 0;
             channel < outputTimeline.getNumChannels();
             ++channel)
        {
            outputTimeline.setSample(
                channel,
                position,
                completedOutput[static_cast<std::size_t>(channel)]
                               [static_cast<std::size_t>(sample)]);
        }
    }
    completedSequence = -1;
    completedOutputFlags = 0;
}

void PluginBridgeClient::readTimelineOutput(
    juce::AudioBuffer<float>& audio) noexcept
{
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto position = static_cast<int>(
            (streamSamplePosition + sample)
            % outputTimelineCapacity);
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            const auto sourceChannel = std::min(
                channel,
                outputTimeline.getNumChannels() - 1);
            audio.setSample(
                channel,
                sample,
                outputTimeline.getSample(sourceChannel, position));
        }
        for (int channel = 0;
             channel < outputTimeline.getNumChannels();
             ++channel)
            outputTimeline.setSample(channel, position, 0.0f);
    }
}

bool PluginBridgeClient::isReady() const noexcept
{
    return ready.load(std::memory_order_acquire);
}

std::uint64_t PluginBridgeClient::lateBlockCount() const noexcept
{
    return lateBlocks.load(std::memory_order_relaxed);
}

int PluginBridgeClient::reportedLatencySamples() const noexcept
{
    return pluginLatencySamples.load(std::memory_order_acquire);
}

double PluginBridgeClient::reportedTailSeconds() const noexcept
{
    return pluginTailSeconds.load(std::memory_order_acquire);
}

std::vector<PluginParameterDescriptor>
PluginBridgeClient::parameterDescriptors() const
{
    const std::lock_guard lock(parameterMutex);
    return parameters;
}

juce::String PluginBridgeClient::diagnosticState() const
{
    if (sharedState == nullptr)
        return "unmapped";

    return "host="
        + juce::String(sharedState->hostSequence.load(std::memory_order_acquire))
        + " worker="
        + juce::String(sharedState->workerSequence.load(std::memory_order_acquire))
        + " input="
        + juce::String(processingBlockSize)
        + " inflight="
        + juce::String(inFlightSequence)
        + " completed="
        + juce::String(completedSequence);
}

void PluginBridgeClient::handleMessageFromWorker(const juce::MemoryBlock& message)
{
    const auto text = message.toString();
    if (text.startsWith("latency|"))
    {
        const auto parts = juce::StringArray::fromTokens(
            text,
            "|",
            "");
        if (parts.size() > 1)
        {
            const auto latency = parts[1].getIntValue();
            if (latency < 0
                || latency
                    > PluginBridgeSharedState::maxSupportedLatencySamples)
            {
                ready.store(false, std::memory_order_release);
                return;
            }
            pluginLatencySamples.store(latency,
                                       std::memory_order_release);
        }
        if (parts.size() > 2)
            pluginTailSeconds.store(
                parts[2].getDoubleValue(),
                std::memory_order_release);
        return;
    }
    if (text.startsWith("metadata|"))
    {
        updateParameterMetadata(
            text.fromFirstOccurrenceOf(
                "metadata|",
                false,
                false));
        return;
    }
    {
        const std::lock_guard lock(responseMutex);
        responseMessage = text;
    }
    responseCondition.notify_one();
}

void PluginBridgeClient::updateParameterMetadata(
    const juce::String& encoded)
{
    juce::MemoryBlock metadata;
    if (!metadata.fromBase64Encoding(encoded))
        return;
    const auto value = juce::JSON::parse(metadata.toString());
    if (!value.isArray())
        return;
    std::vector<PluginParameterDescriptor> updated;
    updated.reserve(
        static_cast<std::size_t>(value.getArray()->size()));
    for (const auto& parameterValue : *value.getArray())
    {
        const auto* object = parameterValue.getDynamicObject();
        if (object == nullptr)
            continue;
        PluginParameterDescriptor parameter;
        parameter.index = static_cast<int>(
            object->getProperty("index"));
        parameter.id = object->getProperty("id").toString();
        parameter.name = object->getProperty("name").toString();
        parameter.value = static_cast<float>(
            static_cast<double>(object->getProperty("value")));
        parameter.automatable = static_cast<bool>(
            object->getProperty("automatable"));
        updated.push_back(std::move(parameter));
    }
    const std::lock_guard lock(parameterMutex);
    parameters = std::move(updated);
}

void PluginBridgeClient::handleConnectionLost()
{
    connectionLost.store(true, std::memory_order_release);
    ready.store(false, std::memory_order_release);
    responseCondition.notify_one();
}

juce::Result PluginBridgeClient::createSharedFile()
{
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("Studio Duo Bridges");
    if (!directory.createDirectory())
        return juce::Result::fail("Could not create the bridge transport directory.");

    sharedFile = directory.getNonexistentChildFile("bridge-", ".shared", true);
    auto stream = sharedFile.createOutputStream();
    if (stream == nullptr
        || !stream->setPosition(static_cast<juce::int64>(sizeof(PluginBridgeSharedState) - 1))
        || !stream->writeByte(0))
    {
        sharedFile.deleteFile();
        return juce::Result::fail("Could not allocate the bridge shared-memory file.");
    }
    stream->flush();
    if (stream->getStatus().failed())
    {
        const auto result = stream->getStatus();
        sharedFile.deleteFile();
        return result;
    }
    stream.reset();

    mapping = std::make_unique<juce::MemoryMappedFile>(sharedFile,
                                                       juce::MemoryMappedFile::readWrite,
                                                       false);
    if (mapping->getData() == nullptr || mapping->getSize() < sizeof(PluginBridgeSharedState))
    {
        mapping.reset();
        sharedFile.deleteFile();
        return juce::Result::fail("Could not map the plugin bridge transport.");
    }

    sharedState = new (mapping->getData()) PluginBridgeSharedState();
    lateBlocks.store(0, std::memory_order_relaxed);
    return juce::Result::ok();
}
}
