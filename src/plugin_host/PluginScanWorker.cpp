#include "PluginScanWorker.h"

namespace studio
{
PluginScanWorker::PluginScanWorker()
{
    juce::addDefaultFormatsToManager(formatManager);
}

PluginScanWorker::~PluginScanWorker()
{
    cancelPendingUpdate();
}

bool PluginScanWorker::initialise(const juce::String& commandLine)
{
    return initialiseFromCommandLine(commandLine, pluginScanProcessId, 5000);
}

void PluginScanWorker::handleMessageFromCoordinator(const juce::MemoryBlock& message)
{
    if (message.isEmpty())
        return;

    {
        const std::lock_guard lock(queueMutex);
        requests.push(message);
    }
    triggerAsyncUpdate();
}

void PluginScanWorker::handleConnectionLost()
{
    juce::JUCEApplicationBase::quit();
}

void PluginScanWorker::handleAsyncUpdate()
{
    for (;;)
    {
        juce::MemoryBlock request;
        {
            const std::lock_guard lock(queueMutex);
            if (requests.empty())
                return;

            request = std::move(requests.front());
            requests.pop();
        }

        const auto response = scan(request);
        sendMessageToCoordinator(response);
    }
}

juce::MemoryBlock PluginScanWorker::scan(const juce::MemoryBlock& request)
{
    juce::MemoryInputStream stream(request, false);
    const auto formatName = stream.readString();
    const auto fileOrIdentifier = stream.readString();

    juce::XmlElement response("PLUGIN_SCAN_RESULT");
    response.setAttribute("format", formatName);
    response.setAttribute("identifier", fileOrIdentifier);

    for (auto* format : formatManager.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, fileOrIdentifier);
        for (const auto& description : descriptions)
            response.addChildElement(description->createXml().release());
        break;
    }

    const auto xml = response.toString();
    return { xml.toRawUTF8(), xml.getNumBytesAsUTF8() };
}
}
