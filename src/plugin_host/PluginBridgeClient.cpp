#include "PluginBridgeClient.h"

#include <new>

namespace studio
{
PluginBridgeClient::PluginBridgeClient() = default;

PluginBridgeClient::~PluginBridgeClient()
{
    stop();
}

juce::Result PluginBridgeClient::start()
{
    return startInternal(nullptr, 48000.0, 512, {});
}

juce::Result PluginBridgeClient::startPlugin(const juce::PluginDescription& description,
                                             double pluginSampleRate,
                                             int blockSize,
                                             const juce::MemoryBlock& state)
{
    return startInternal(&description, pluginSampleRate, blockSize, state);
}

juce::Result PluginBridgeClient::startInternal(const juce::PluginDescription* description,
                                               double pluginSampleRate,
                                               int blockSize,
                                               const juce::MemoryBlock& state)
{
    stop();

    if (pluginSampleRate <= 0.0
        || blockSize <= 0
        || blockSize > PluginBridgeSharedState::maxBlockSize)
        return juce::Result::fail("Plugin bridge received invalid audio settings.");

    if (const auto result = createSharedFile(); result.failed())
        return result;

    connectionLost.store(false, std::memory_order_release);
    if (!launchWorkerProcess(juce::File::getSpecialLocation(juce::File::currentExecutableFile),
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
    stream.writeInt(blockSize);
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

    pluginLatencySamples.store(responseParts.size() > 1 ? responseParts[1].getIntValue() : 0,
                               std::memory_order_release);
    pluginTailSeconds.store(responseParts.size() > 2 ? responseParts[2].getDoubleValue() : 0.0,
                            std::memory_order_release);
    parameters.clear();
    if (responseParts.size() > 3)
    {
        juce::MemoryBlock metadata;
        if (metadata.fromBase64Encoding(responseParts[3]))
        {
            const auto value = juce::JSON::parse(
                metadata.toString());
            if (value.isArray())
            {
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
                        static_cast<double>(
                            object->getProperty("value")));
                    parameter.automatable = static_cast<bool>(
                        object->getProperty("automatable"));
                    parameters.push_back(std::move(parameter));
                }
            }
        }
    }
    ready.store(true, std::memory_order_release);
    processingBlockSize = blockSize;
    completedOutputSamples = 0;
    nextInputSequence = 0;
    inFlightSequence = -1;
    completedSequence = -1;
    inFlightMissedDeadline = false;
    for (auto& channel : inputAccumulator)
        channel.fill(0.0f);
    for (auto& channel : sidechainAccumulator)
        channel.fill(0.0f);
    for (auto& channel : completedOutput)
        channel.fill(0.0f);
    for (auto& channel : lastValidOutput)
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
    lastValidChannels = 0;
    lastValidSamples = 0;
    lastOutputSequence = 0;
    completedOutputSamples = 0;
    nextInputSequence = 0;
    inFlightSequence = -1;
    completedSequence = -1;
    inFlightMissedDeadline = false;
    responseMessage.clear();
    pluginLatencySamples.store(0, std::memory_order_relaxed);
    pluginTailSeconds.store(0.0, std::memory_order_relaxed);
    parameters.clear();
}

juce::Result PluginBridgeClient::requestState(
    juce::MemoryBlock& state,
    std::chrono::milliseconds timeout)
{
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
    if (!responseMessage.startsWith("state|"))
        return juce::Result::fail(
            responseMessage.isNotEmpty()
                ? responseMessage
                : "Plugin bridge returned no state.");
    const auto encoded =
        responseMessage.fromFirstOccurrenceOf("|", false, false);
    if (encoded.isEmpty())
        return juce::Result::ok();
    if (!state.fromBase64Encoding(encoded))
        return juce::Result::fail(
            "Plugin bridge returned invalid state data.");
    return juce::Result::ok();
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

    const auto expectedSequence = inFlightSequence;
    fetchWorkerOutput();
    writeOutputBlock(audio, expectedSequence);

    fetchWorkerOutput();
    if (inFlightSequence < 0
        && sharedState->workerSequence.load(std::memory_order_acquire)
            == sharedState->hostSequence.load(std::memory_order_relaxed))
        publishInputBlock(samples);
    else
        lateBlocks.fetch_add(1, std::memory_order_relaxed);
}

void PluginBridgeClient::fetchWorkerOutput() noexcept
{
    const auto workerSequence = sharedState->workerSequence.load(std::memory_order_acquire);
    if (workerSequence == lastOutputSequence)
        return;

    const auto outputChannels = std::min(
        static_cast<int>(sharedState->numChannels.load(std::memory_order_relaxed)),
        PluginBridgeSharedState::maxChannels);
    const auto outputSize = std::min(
        static_cast<int>(sharedState->numSamples.load(std::memory_order_relaxed)),
        processingBlockSize);
    if (inFlightSequence >= 0 && !inFlightMissedDeadline)
    {
        for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        {
            const auto sourceChannel = outputChannels > 0 ? std::min(channel,
                                                                     outputChannels - 1)
                                                          : -1;
            auto& destination = completedOutput[static_cast<std::size_t>(channel)];
            if (sourceChannel >= 0)
                std::copy_n(sharedState->output[static_cast<std::size_t>(sourceChannel)].begin(),
                            outputSize,
                            destination.begin());
            else
                std::fill_n(destination.begin(), outputSize, 0.0f);
        }
        completedSequence = inFlightSequence;
        completedOutputSamples = outputSize;
    }

    inFlightSequence = -1;
    inFlightMissedDeadline = false;
    lastOutputSequence = workerSequence;
}

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
    sharedState->hostSequence.store(hostSequence + 1, std::memory_order_release);
    inFlightSequence = nextInputSequence++;
    inFlightMissedDeadline = false;
}

void PluginBridgeClient::writeOutputBlock(juce::AudioBuffer<float>& audio,
                                          std::int64_t expectedSequence) noexcept
{
    const auto samples = audio.getNumSamples();
    if (expectedSequence >= 0 && completedSequence == expectedSequence)
    {
        lastValidOutput = completedOutput;
        lastValidChannels = PluginBridgeSharedState::maxChannels;
        lastValidSamples = completedOutputSamples;
        completedSequence = -1;
    }
    else if (expectedSequence >= 0 && inFlightSequence == expectedSequence)
        inFlightMissedDeadline = true;

    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        const auto sourceChannel = std::min(channel,
                                            PluginBridgeSharedState::maxChannels - 1);
        if (lastValidSamples > 0)
        {
            const auto copied = std::min(samples, lastValidSamples);
            audio.copyFrom(channel,
                           0,
                           lastValidOutput[static_cast<std::size_t>(sourceChannel)].data(),
                           copied);
            if (copied < samples)
                audio.clear(channel, copied, samples - copied);
        }
        else
        {
            audio.clear(channel, 0, samples);
        }
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

const std::vector<PluginParameterDescriptor>&
PluginBridgeClient::parameterDescriptors() const noexcept
{
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
    {
        const std::lock_guard lock(responseMutex);
        responseMessage = message.toString();
    }
    responseCondition.notify_one();
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
