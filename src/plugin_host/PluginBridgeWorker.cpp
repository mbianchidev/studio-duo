#include "PluginBridgeWorker.h"

#include "PluginFormats.h"
#include "PluginEditorWindow.h"
#include "SampleAccurateAutomationTarget.h"
#include "ValidatedPluginStateTarget.h"
#include <algorithm>
#include <cmath>
#include <span>
#if JUCE_MAC
#include "platform/ApplicationIcon.h"
#endif

namespace studio
{
PluginBridgeWorker::PluginBridgeWorker()
    : juce::Thread("Studio Duo plugin bridge")
{
    PluginFormats::addSupportedFormats(formatManager);
    automationBoundaries.reserve(
        static_cast<std::size_t>(
            PluginBridgeSharedState::maxParameterEvents * 2)
        + static_cast<std::size_t>(
            PluginBridgeSharedState::maxBlockSize / 8 + 2));
}

PluginBridgeWorker::~PluginBridgeWorker()
{
    stopTimer();
    signalThreadShouldExit();
    stopThread(2000);
    if (plugin != nullptr)
        plugin->removeListener(this);
}

bool PluginBridgeWorker::initialise(const juce::String& commandLine)
{
    const auto worker = initialiseFromCommandLine(
        commandLine,
        pluginBridgeProcessId,
        5000);
#if JUCE_MAC
    if (worker)
        setPlatformAccessoryApplication();
#endif
    if (worker)
        startTimerHz(50);
    return worker;
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
            auto stateResult = juce::Result::ok();
            {
                const std::lock_guard lock(pluginMutex);
                if (plugin != nullptr)
                {
                    if (auto* validated =
                            dynamic_cast<ValidatedPluginStateTarget*>(
                                plugin.get()))
                    {
                        stateResult =
                            validated->saveValidatedState(state);
                    }
                    else
                    {
                        plugin->getStateInformation(state);
                    }
                }
            }
            if (stateResult.failed())
                sendStatus(
                    "state|error|"
                    + stateResult.getErrorMessage());
            else if (state.isEmpty())
                sendStatus("state|none");
            else
                sendStatus("state|data|" + state.toBase64Encoding());
        }
        else if (command == "set-parameter")
        {
            const auto parameterIndex = stream.readInt();
            const auto value = stream.readFloat();
            const std::lock_guard lock(pluginMutex);
            if (plugin != nullptr
                && parameterIndex >= 0
                && parameterIndex < plugin->getParameters().size())
                plugin->getParameters()[parameterIndex]->setValue(value);
        }
        else if (command == "show-editor")
        {
            juce::MessageManager::callAsync(
                [this] { showEditor(); });
        }
        else if (command == "hide-editor")
        {
            juce::MessageManager::callAsync(
                [this] { hideEditor(); });
        }
        else if (command == "focus-editor")
        {
            juce::MessageManager::callAsync(
                [this] { focusEditor(); });
        }
        else if (command == "resize-editor")
        {
            const auto width = stream.readInt();
            const auto height = stream.readInt();
            juce::MessageManager::callAsync(
                [this, width, height]
                {
                    resizeEditor(width, height);
                });
        }
        return;
    }

    const juce::File sharedFile(stream.readString());
    const auto pluginXml = stream.readString();
    const auto stateBase64 = stream.readString();
    const auto sampleRate = stream.readDouble();
    const auto blockSize = stream.readInt();
    const auto requestedSidechainChannels = stream.readInt();
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
        startProcessing(nullptr,
                        pluginState,
                        sampleRate,
                        blockSize,
                        requestedSidechainChannels);
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
        [this,
         pluginState,
         sampleRate,
         blockSize,
         requestedSidechainChannels](
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const juce::String& error)
        {
            if (instance == nullptr)
            {
                sendStatus("error:" + (error.isNotEmpty() ? error : "plugin-instantiation-failed"));
                return;
            }

            startProcessing(std::move(instance),
                            pluginState,
                            sampleRate,
                            blockSize,
                            requestedSidechainChannels);
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
        processBuffer.clear(0, samples);
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
        const auto events =
            std::span<const PluginBridgeParameterEvent>(
                sharedState->parameterEvents.data(),
                static_cast<std::size_t>(eventCount));
        auto* automationTarget =
            dynamic_cast<SampleAccurateAutomationTarget*>(
                plugin.get());
        if (automationTarget != nullptr
            && automationTarget->supportsSampleAccurateAutomation(
                events))
        {
            juce::AudioBuffer<float> view(
                processBuffer.getArrayOfWritePointers(),
                processingChannels,
                samples);
            automationTarget->processBlockWithAutomation(
                view,
                midiBuffer,
                events);
        }
        else
        {
            automationBoundaries.clear();
            automationBoundaries.push_back(0);
            automationBoundaries.push_back(samples);
            auto automationStride = samples;
            for (int eventIndex = 0;
                 eventIndex < eventCount;
                 ++eventIndex)
            {
                const auto& event =
                    sharedState->parameterEvents[
                        static_cast<std::size_t>(eventIndex)];
                const auto start = std::min(
                    samples,
                    static_cast<int>(event.sampleOffset));
                automationBoundaries.push_back(start);
                if ((event.flags
                     & PluginBridgeParameterEvent::rampFlag)
                    != 0)
                {
                    const auto end = std::min(
                        samples,
                        static_cast<int>(event.rampEndOffset));
                    automationBoundaries.push_back(end);
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
            if (automationStride < samples)
            {
                for (auto offset = automationStride;
                     offset < samples;
                     offset += automationStride)
                    automationBoundaries.push_back(offset);
            }
            std::sort(
                automationBoundaries.begin(),
                automationBoundaries.end());
            automationBoundaries.erase(
                std::unique(
                    automationBoundaries.begin(),
                    automationBoundaries.end()),
                automationBoundaries.end());
            for (std::size_t boundary = 0;
                 boundary + 1 < automationBoundaries.size();
                 ++boundary)
            {
                const auto offset = automationBoundaries[boundary];
                const auto next = automationBoundaries[boundary + 1];
                auto& parameters = plugin->getParameters();
                for (int eventIndex = 0;
                     eventIndex < eventCount;
                     ++eventIndex)
                {
                    const auto& event =
                        sharedState->parameterEvents[
                            static_cast<std::size_t>(eventIndex)];
                    if (event.parameterIndex
                        >= static_cast<std::uint32_t>(
                            parameters.size()))
                        continue;
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
                                + (event.rampEndValue - event.value)
                                    * progress);
                    }
                    else if (offset == start)
                    {
                        parameters[static_cast<int>(
                            event.parameterIndex)]
                            ->setValue(event.value);
                    }
                }
                juce::AudioBuffer<float> view(
                    processBuffer.getArrayOfWritePointers(),
                    processingChannels,
                    offset,
                    next - offset);
                plugin->processBlock(view, midiBuffer);
            }
        }
        for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        {
            auto& destination = sharedState->output[static_cast<std::size_t>(channel)];
            if (channel < mainOutputChannels)
                std::copy_n(processBuffer.getReadPointer(channel), samples, destination.begin());
            else
                std::fill_n(destination.begin(), samples, 0.0f);
        }
        sharedState->numOutputChannels.store(
            static_cast<std::uint32_t>(mainOutputChannels),
            std::memory_order_relaxed);
        sharedState->workerSequence.store(hostSequence, std::memory_order_release);
    }
}

void PluginBridgeWorker::startProcessing(std::unique_ptr<juce::AudioPluginInstance> newPlugin,
                                         const juce::MemoryBlock& state,
                                         double sampleRate,
                                         int blockSize,
                                         int requestedSidechainChannels)
{
    editorWindow.reset();
    if (newPlugin != nullptr)
    {
        if (requestedSidechainChannels > 0
            && newPlugin->getBusCount(true) > 1)
        {
            auto layout = newPlugin->getBusesLayout();
            layout.inputBuses.set(
                1,
                requestedSidechainChannels == 1
                    ? juce::AudioChannelSet::mono()
                    : juce::AudioChannelSet::stereo());
            if (!newPlugin->setBusesLayout(layout))
            {
                sendStatus("error:unsupported-sidechain-layout");
                return;
            }
        }
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
        {
            if (auto* validated =
                    dynamic_cast<ValidatedPluginStateTarget*>(
                        newPlugin.get()))
            {
                if (const auto result =
                        validated->restoreValidatedState(
                            state.getData(),
                            static_cast<int>(state.getSize()));
                    result.failed())
                {
                    sendStatus(
                        "error:state-restore:"
                        + result.getErrorMessage());
                    return;
                }
            }
            else
            {
                newPlugin->setStateInformation(
                    state.getData(),
                    static_cast<int>(state.getSize()));
            }
        }
        newPlugin->prepareToPlay(sampleRate, blockSize);
    }

    if (plugin != nullptr)
        plugin->removeListener(this);
    plugin = std::move(newPlugin);
    if (plugin != nullptr)
        plugin->addListener(this);
    processBuffer.setSize(processingChannels,
                          PluginBridgeSharedState::maxBlockSize,
                          false,
                          true,
                          false);
    processBuffer.clear();
    sendStatus("ready|"
               + juce::String(plugin != nullptr ? plugin->getLatencySamples() : 0)
               + "|"
               + juce::String(plugin != nullptr ? plugin->getTailLengthSeconds() : 0.0, 6)
               + "|"
               + parameterMetadata());
    startThread(juce::Thread::Priority::high);
}

juce::String PluginBridgeWorker::parameterMetadata() const
{
    juce::Array<juce::var> parameterValues;
    if (plugin != nullptr)
    {
        const auto& parameters = plugin->getParameters();
        for (int index = 0; index < parameters.size(); ++index)
        {
            const auto* parameter = parameters[index];
            auto object = std::make_unique<juce::DynamicObject>();
            object->setProperty("index", index);
            const auto* identified = dynamic_cast<
                const juce::HostedAudioProcessorParameter*>(
                parameter);
            object->setProperty(
                "id",
                identified != nullptr
                    ? identified->getParameterID()
                    : juce::String(index));
            object->setProperty("name", parameter->getName(128));
            object->setProperty("value", parameter->getValue());
            object->setProperty(
                "automatable",
                parameter->isAutomatable());
            parameterValues.add(juce::var(object.release()));
        }
    }
    const auto json = juce::JSON::toString(
        juce::var(parameterValues),
        false);
    return juce::MemoryBlock(
               json.toRawUTF8(),
               json.getNumBytesAsUTF8())
        .toBase64Encoding();
}

void PluginBridgeWorker::audioProcessorParameterChanged(
    juce::AudioProcessor*,
    int,
    float)
{
}

void PluginBridgeWorker::audioProcessorChanged(
    juce::AudioProcessor*,
    const ChangeDetails& details)
{
    if (plugin == nullptr)
        return;
    if (details.latencyChanged)
        latencyReportPending.store(true, std::memory_order_release);
    if (details.parameterInfoChanged)
        metadataReportPending.store(true, std::memory_order_release);
}

void PluginBridgeWorker::timerCallback()
{
    if (plugin == nullptr)
        return;
    if (latencyReportPending.exchange(
            false,
            std::memory_order_acq_rel))
    {
        auto message = juce::String();
        {
            const std::lock_guard lock(pluginMutex);
            if (plugin == nullptr)
                return;
            message = "latency|"
                + juce::String(plugin->getLatencySamples())
                + "|"
                + juce::String(
                    plugin->getTailLengthSeconds(),
                    6);
        }
        sendStatus(message);
    }
    if (metadataReportPending.exchange(
            false,
            std::memory_order_acq_rel))
    {
        auto metadata = juce::String();
        {
            const std::lock_guard lock(pluginMutex);
            if (plugin == nullptr)
                return;
            metadata = parameterMetadata();
        }
        sendStatus("metadata|" + metadata);
    }
}

void PluginBridgeWorker::showEditor()
{
    if (plugin == nullptr)
    {
        sendStatus("editor|error|Plugin runtime is unavailable");
        return;
    }
    if (editorWindow == nullptr)
    {
        juce::String error;
        editorWindow = PluginEditorWindow::create(
            *plugin,
            plugin->getName(),
            error);
        if (editorWindow == nullptr)
        {
            sendStatus("editor|error|" + error);
            return;
        }
    }
    editorWindow->showEditor();
    sendStatus(
        "editor|shown|"
        + juce::String(editorWindow->getWidth())
        + "|"
        + juce::String(editorWindow->getHeight()));
}

void PluginBridgeWorker::hideEditor()
{
    if (editorWindow != nullptr)
        editorWindow->hideEditor();
    sendStatus("editor|hidden");
}

void PluginBridgeWorker::focusEditor()
{
    if (editorWindow == nullptr)
    {
        showEditor();
        return;
    }
    editorWindow->focusEditor();
    sendStatus("editor|focused");
}

void PluginBridgeWorker::resizeEditor(int width, int height)
{
    if (editorWindow == nullptr)
    {
        sendStatus("editor|error|Plugin editor is not open");
        return;
    }
    if (!editorWindow->resizeEditor(width, height))
    {
        sendStatus("editor|error|Plugin editor size is invalid");
        return;
    }
    sendStatus(
        "editor|resized|"
        + juce::String(editorWindow->getWidth())
        + "|"
        + juce::String(editorWindow->getHeight()));
}

void PluginBridgeWorker::sendStatus(const juce::String& status)
{
    sendMessageToCoordinator({ status.toRawUTF8(), status.getNumBytesAsUTF8() });
}
}
