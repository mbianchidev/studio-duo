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

    if (responseMessage != "ready")
    {
        const auto error = responseMessage.isNotEmpty() ? responseMessage
                                                        : "worker disconnected";
        stop();
        return juce::Result::fail("Plugin bridge setup failed: " + error);
    }

    ready.store(true, std::memory_order_release);
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
    responseMessage.clear();
}

void PluginBridgeClient::processBlock(juce::AudioBuffer<float>& audio) noexcept
{
    if (!isReady() || sharedState == nullptr)
    {
        audio.clear();
        return;
    }

    const auto channels = std::min(audio.getNumChannels(), PluginBridgeSharedState::maxChannels);
    const auto samples = std::min(audio.getNumSamples(), PluginBridgeSharedState::maxBlockSize);
    const auto hostSequence = sharedState->hostSequence.load(std::memory_order_relaxed);
    const auto workerSequence = sharedState->workerSequence.load(std::memory_order_acquire);

    if (workerSequence != lastOutputSequence)
    {
        const auto outputChannels = std::min(
            static_cast<int>(sharedState->numChannels.load(std::memory_order_relaxed)),
            PluginBridgeSharedState::maxChannels);
        const auto outputSamples = std::min(
            static_cast<int>(sharedState->numSamples.load(std::memory_order_relaxed)),
            PluginBridgeSharedState::maxBlockSize);

        for (int channel = 0; channel < outputChannels; ++channel)
        {
            const auto& source = sharedState->output[static_cast<std::size_t>(channel)];
            std::copy_n(source.begin(),
                        outputSamples,
                        lastValidOutput[static_cast<std::size_t>(channel)].begin());
        }
        lastValidChannels = outputChannels;
        lastValidSamples = outputSamples;
        lastOutputSequence = workerSequence;
    }

    if (workerSequence == hostSequence && audio.getNumSamples() <= PluginBridgeSharedState::maxBlockSize)
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* source = audio.getReadPointer(channel);
            auto& destination = sharedState->input[static_cast<std::size_t>(channel)];
            std::copy_n(source, samples, destination.begin());
        }
        for (int channel = channels; channel < PluginBridgeSharedState::maxChannels; ++channel)
            std::fill_n(sharedState->input[static_cast<std::size_t>(channel)].begin(), samples, 0.0f);

        sharedState->numChannels.store(static_cast<std::uint32_t>(channels),
                                       std::memory_order_relaxed);
        sharedState->numSamples.store(static_cast<std::uint32_t>(samples),
                                      std::memory_order_relaxed);
        sharedState->hostSequence.store(hostSequence + 1, std::memory_order_release);
    }
    else
    {
        lateBlocks.fetch_add(1, std::memory_order_relaxed);
    }

    if (lastValidSamples == samples && lastValidChannels > 0)
    {
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            const auto sourceChannel = std::min(channel, lastValidChannels - 1);
            audio.copyFrom(channel,
                           0,
                           lastValidOutput[static_cast<std::size_t>(sourceChannel)].data(),
                           samples);
        }
    }
    else
    {
        audio.clear();
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
