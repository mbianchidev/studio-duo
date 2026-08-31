#include "PluginBridgeWorker.h"

namespace studio
{
PluginBridgeWorker::PluginBridgeWorker()
    : juce::Thread("Studio Duo plugin bridge")
{
    juce::addDefaultFormatsToManager(formatManager);
}

PluginBridgeWorker::~PluginBridgeWorker()
{
    signalThreadShouldExit();
    stopThread(2000);
}

bool PluginBridgeWorker::initialise(const juce::String& commandLine)
{
    return initialiseFromCommandLine(commandLine, pluginBridgeProcessId, 5000);
}

void PluginBridgeWorker::handleMessageFromCoordinator(const juce::MemoryBlock& message)
{
    if (mapping != nullptr || message.isEmpty())
        return;

    juce::MemoryInputStream stream(message, false);
    const juce::File sharedFile(stream.readString());
    const auto pluginXml = stream.readString();
    const auto stateBase64 = stream.readString();
    const auto sampleRate = stream.readDouble();
    const auto blockSize = stream.readInt();
    auto newMapping = std::make_unique<juce::MemoryMappedFile>(
        sharedFile,
        juce::MemoryMappedFile::readWrite,
        false);

    if (newMapping->getData() == nullptr
        || newMapping->getSize() < sizeof(PluginBridgeSharedState))
    {
        sendStatus("error:invalid-map");
        return;
    }

    auto* state = static_cast<PluginBridgeSharedState*>(newMapping->getData());
    if (!PluginBridgeProtocol::isValid(*state))
    {
        sendStatus("error:invalid-protocol");
        return;
    }

    mapping = std::move(newMapping);
    sharedState = state;

    juce::MemoryBlock pluginState;
    if (stateBase64.isNotEmpty() && !pluginState.fromBase64Encoding(stateBase64))
    {
        sendStatus("error:invalid-state");
        return;
    }

    if (pluginXml.isEmpty())
    {
        startProcessing(nullptr, pluginState, sampleRate, blockSize);
        return;
    }

    auto xml = juce::parseXML(pluginXml);
    juce::PluginDescription description;
    if (xml == nullptr || !description.loadFromXml(*xml))
    {
        sendStatus("error:invalid-plugin-description");
        return;
    }

    formatManager.createPluginInstanceAsync(
        description,
        sampleRate,
        blockSize,
        [this, pluginState, sampleRate, blockSize](std::unique_ptr<juce::AudioPluginInstance> instance,
                                                   const juce::String& error)
        {
            if (instance == nullptr)
            {
                sendStatus("error:" + (error.isNotEmpty() ? error : "plugin-instantiation-failed"));
                return;
            }

            startProcessing(std::move(instance), pluginState, sampleRate, blockSize);
        });
}

void PluginBridgeWorker::handleConnectionLost()
{
    signalThreadShouldExit();
    juce::JUCEApplicationBase::quit();
}

void PluginBridgeWorker::run()
{
    while (!threadShouldExit()
           && sharedState != nullptr
           && sharedState->shutdownRequested.load(std::memory_order_acquire) == 0)
    {
        const auto hostSequence = sharedState->hostSequence.load(std::memory_order_acquire);
        if (hostSequence == sharedState->workerSequence.load(std::memory_order_relaxed))
        {
            wait(1);
            continue;
        }

        if (plugin == nullptr)
        {
            PluginBridgeProtocol::processAvailableBlock(*sharedState);
            continue;
        }

        const auto channels = std::min(
            static_cast<int>(sharedState->numChannels.load(std::memory_order_relaxed)),
            PluginBridgeSharedState::maxChannels);
        const auto samples = std::min(
            static_cast<int>(sharedState->numSamples.load(std::memory_order_relaxed)),
            PluginBridgeSharedState::maxBlockSize);
        processBuffer.clear();
        for (int channel = 0; channel < channels; ++channel)
            processBuffer.copyFrom(channel,
                                   0,
                                   sharedState->input[static_cast<std::size_t>(channel)].data(),
                                   samples);

        midiBuffer.clear();
        plugin->processBlock(processBuffer, midiBuffer);
        for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        {
            auto& destination = sharedState->output[static_cast<std::size_t>(channel)];
            if (channel < processBuffer.getNumChannels())
                std::copy_n(processBuffer.getReadPointer(channel), samples, destination.begin());
            else
                std::fill_n(destination.begin(), samples, 0.0f);
        }
        sharedState->workerSequence.store(hostSequence, std::memory_order_release);
    }
}

void PluginBridgeWorker::startProcessing(std::unique_ptr<juce::AudioPluginInstance> newPlugin,
                                         const juce::MemoryBlock& state,
                                         double sampleRate,
                                         int blockSize)
{
    if (newPlugin != nullptr)
    {
        const auto inputChannels = newPlugin->getTotalNumInputChannels();
        const auto outputChannels = newPlugin->getTotalNumOutputChannels();
        if (inputChannels > PluginBridgeSharedState::maxChannels
            || outputChannels > PluginBridgeSharedState::maxChannels)
        {
            sendStatus("error:unsupported-channel-layout");
            return;
        }

        newPlugin->setRateAndBufferSizeDetails(sampleRate, blockSize);
        if (!state.isEmpty())
            newPlugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        newPlugin->prepareToPlay(sampleRate, blockSize);
    }

    plugin = std::move(newPlugin);
    processBuffer.setSize(PluginBridgeSharedState::maxChannels,
                          PluginBridgeSharedState::maxBlockSize,
                          false,
                          true,
                          false);
    processBuffer.clear();
    sendStatus("ready|"
               + juce::String(plugin != nullptr ? plugin->getLatencySamples() : 0)
               + "|"
               + juce::String(plugin != nullptr ? plugin->getTailLengthSeconds() : 0.0, 6));
    startThread(juce::Thread::Priority::high);
}

void PluginBridgeWorker::sendStatus(const juce::String& status)
{
    sendMessageToCoordinator({ status.toRawUTF8(), status.getNumBytesAsUTF8() });
}
}
