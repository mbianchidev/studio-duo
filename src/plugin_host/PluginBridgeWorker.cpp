#include "PluginBridgeWorker.h"

#include "PluginFormats.h"

namespace studio
{
PluginBridgeWorker::PluginBridgeWorker()
    : juce::Thread("Studio Duo plugin bridge")
{
    PluginFormats::addSupportedFormats(formatManager);
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
    if (message.isEmpty())
        return;

    juce::MemoryInputStream stream(message, false);
    if (mapping != nullptr)
    {
        const auto command = stream.readString();
        if (command == "get-state")
        {
            juce::MemoryBlock state;
            {
                const std::lock_guard lock(pluginMutex);
                if (plugin != nullptr)
                    plugin->getStateInformation(state);
            }
            sendStatus("state|" + state.toBase64Encoding());
        }
        return;
    }

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

        const std::lock_guard pluginLock(pluginMutex);
        const auto channels = std::min(
            static_cast<int>(sharedState->numChannels.load(std::memory_order_relaxed)),
            PluginBridgeSharedState::maxChannels);
        const auto sidechainChannels = std::min(
            static_cast<int>(
                sharedState->sidechainChannels.load(std::memory_order_relaxed)),
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
        for (int channel = 0;
             channel < std::min(sidechainChannels, sidechainInputChannels);
             ++channel)
        {
            processBuffer.copyFrom(
                mainInputChannels + channel,
                0,
                sharedState->sidechain[static_cast<std::size_t>(channel)].data(),
                samples);
        }

        midiBuffer.clear();
        const auto eventCount =
            PluginBridgeProtocol::parameterEventCount(*sharedState);
        const auto applyEvent = [this](const PluginBridgeParameterEvent& event)
        {
            auto& parameters = plugin->getParameters();
            if (event.parameterIndex < static_cast<std::uint32_t>(
                                           parameters.size()))
            {
                parameters[static_cast<int>(event.parameterIndex)]
                    ->setValue(event.value);
            }
        };
        auto processedSamples = 0;
        auto eventIndex = 0;
        while (eventIndex < eventCount)
        {
            const auto eventOffset = std::min(
                samples,
                static_cast<int>(
                    sharedState
                        ->parameterEvents[static_cast<std::size_t>(eventIndex)]
                        .sampleOffset));
            if (eventOffset > processedSamples)
            {
                juce::AudioBuffer<float> view(
                    processBuffer.getArrayOfWritePointers(),
                    processingChannels,
                    processedSamples,
                    eventOffset - processedSamples);
                plugin->processBlock(view, midiBuffer);
                processedSamples = eventOffset;
            }
            while (eventIndex < eventCount
                   && static_cast<int>(
                          sharedState
                              ->parameterEvents[static_cast<std::size_t>(
                                  eventIndex)]
                              .sampleOffset)
                       == eventOffset)
            {
                applyEvent(
                    sharedState->parameterEvents[static_cast<std::size_t>(
                        eventIndex)]);
                ++eventIndex;
            }
        }
        if (processedSamples < samples)
        {
            juce::AudioBuffer<float> view(
                processBuffer.getArrayOfWritePointers(),
                processingChannels,
                processedSamples,
                samples - processedSamples);
            plugin->processBlock(view, midiBuffer);
        }
        for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        {
            auto& destination = sharedState->output[static_cast<std::size_t>(channel)];
            if (channel < mainOutputChannels)
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
        mainInputChannels = newPlugin->getBusCount(true) > 0
            ? newPlugin->getChannelCountOfBus(true, 0)
            : inputChannels;
        sidechainInputChannels = newPlugin->getBusCount(true) > 1
            ? newPlugin->getChannelCountOfBus(true, 1)
            : 0;
        mainOutputChannels = newPlugin->getBusCount(false) > 0
            ? newPlugin->getChannelCountOfBus(false, 0)
            : outputChannels;
        processingChannels = std::max({ 2, inputChannels, outputChannels });
        if (mainInputChannels > PluginBridgeSharedState::maxChannels
            || sidechainInputChannels > PluginBridgeSharedState::maxChannels
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
    processBuffer.setSize(processingChannels,
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
