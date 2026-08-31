#include "PluginBridgeWorker.h"

namespace studio
{
PluginBridgeWorker::PluginBridgeWorker()
    : juce::Thread("Studio Duo plugin bridge")
{
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
    auto newMapping = std::make_unique<juce::MemoryMappedFile>(
        sharedFile,
        juce::MemoryMappedFile::readWrite,
        false);

    if (newMapping->getData() == nullptr
        || newMapping->getSize() < sizeof(PluginBridgeSharedState))
    {
        const juce::String response = "invalid-map";
        sendMessageToCoordinator({ response.toRawUTF8(), response.getNumBytesAsUTF8() });
        return;
    }

    auto* state = static_cast<PluginBridgeSharedState*>(newMapping->getData());
    if (!PluginBridgeProtocol::isValid(*state))
    {
        const juce::String response = "invalid-protocol";
        sendMessageToCoordinator({ response.toRawUTF8(), response.getNumBytesAsUTF8() });
        return;
    }

    mapping = std::move(newMapping);
    sharedState = state;
    const juce::String response = "ready";
    sendMessageToCoordinator({ response.toRawUTF8(), response.getNumBytesAsUTF8() });
    startThread(juce::Thread::Priority::high);
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
        if (!PluginBridgeProtocol::processAvailableBlock(*sharedState))
            wait(1);
    }
}
}
