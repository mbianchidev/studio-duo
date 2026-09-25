#include "ui/MainComponent.h"
#include "ui/MainWindowSizing.h"
#include "ui/ReentrancySafeTimer.h"
#include "plugin_host/PluginBridgeClient.h"
#include "plugin_host/PluginBridgeWorker.h"
#include "plugin_host/PluginScanWorker.h"
#include "plugin_host/PluginCompatibilityValidator.h"
#include "plugin_host/ScreamForgeValidation.h"
#include "platform/ApplicationIcon.h"
#include "logging/StudioLogger.h"
#include "audio/AudioDeviceProbe.h"
#include "platform/WindowsCrashHandler.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace studio
{
namespace
{
juce::File startupProjectFromArguments(
    const juce::StringArray& arguments)
{
    for (const auto& argument : arguments)
    {
        if (!argument.startsWith("-")
            && argument.endsWithIgnoreCase(
                ".studioduo"))
            return juce::File(argument);
    }
    return {};
}
}

class StudioDuoApplication final : public juce::JUCEApplication
{
public:
    [[nodiscard]] const juce::String getApplicationName() override
    {
        return "Studio Duo";
    }

    [[nodiscard]] const juce::String getApplicationVersion() override
    {
        return STUDIO_DUO_VERSION;
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String& commandLine) override
    {
        fileLogger = StudioLogger::createDefault();
        juce::Logger::setCurrentLogger(fileLogger.get());
        logInfo(
            "app.startup",
            "Studio Duo " + getApplicationVersion()
                + " process started on " + juce::SystemStats::getOperatingSystemName());
        fileLogger->flush();
#if JUCE_WINDOWS
        if (const auto result = crashHandler.initialise(StudioLogger::defaultLogDirectory());
            result.failed())
        {
            logError("app.crash", result.getErrorMessage());
            fileLogger->flush();
        }
#endif
        const auto arguments = getCommandLineParameterArray();
#if JUCE_WINDOWS
        if (!arguments.isEmpty()
            && arguments[0] == audioDeviceProbeArgument)
        {
            WindowsCrashHandler::exchangeContext(
                WindowsCrashContext::audioDeviceProbe);
        }
        else if (commandLine.contains(pluginBridgeProcessId)
                 || commandLine.contains(pluginScanProcessId)
                 || commandLine.contains("--validate-plugin")
                 || commandLine.contains("--validate-scream-forge")
                 || commandLine.contains("--bridge-plugin-self-test"))
        {
            WindowsCrashHandler::exchangeContext(
                WindowsCrashContext::pluginWorker);
        }
#endif
        if (const auto result = runAudioDeviceProbeWorker(arguments, probeNativeAudioDeviceSetup))
        {
            setApplicationReturnValue(*result);
            quit();
            return;
        }

        auto bridgeWorker = std::make_unique<PluginBridgeWorker>();
        if (bridgeWorker->initialise(commandLine))
        {
            logDebug(
                "app.lifecycle",
                "Plugin bridge worker started.");
            pluginBridgeWorker = std::move(bridgeWorker);
            return;
        }

        auto worker = std::make_unique<PluginScanWorker>();
        if (worker->initialise(commandLine))
        {
            logDebug(
                "app.lifecycle",
                "Plugin scan worker started.");
            pluginScanWorker = std::move(worker);
            return;
        }

        if (commandLine.contains("--bridge-self-test"))
        {
            bridgeSelfTest = std::make_unique<PluginBridgeClient>();
            juce::MessageManager::callAsync([this] { runBridgeSelfTest(); });
            return;
        }

        if (commandLine.contains("--bridge-plugin-self-test"))
        {
            bridgeSelfTest = std::make_unique<PluginBridgeClient>();
            pluginActivationCatalog = std::make_unique<PluginCatalog>();
            validationIdentifier = commandLine
                .fromFirstOccurrenceOf(
                    "--bridge-plugin-self-test",
                    false,
                    false)
                .trim()
                .unquoted();
            juce::MessageManager::callAsync([this] { runPluginActivationSelfTest(); });
            return;
        }
        if (commandLine.contains("--validate-scream-forge"))
        {
            pluginActivationCatalog = std::make_unique<PluginCatalog>();
            juce::MessageManager::callAsync(
                [this] { runScreamForgeValidation(); });
            return;
        }
        if (commandLine.contains("--validate-plugin"))
        {
            validationIdentifier = commandLine
                .fromFirstOccurrenceOf("--validate-plugin", false, false)
                .trim()
                .unquoted();
            pluginActivationCatalog = std::make_unique<PluginCatalog>();
            juce::MessageManager::callAsync(
                [this] { runPluginValidation(); });
            return;
        }

#if JUCE_MAC
        applyPlatformApplicationIcon();
#endif
        const auto startupSelfTest = arguments.contains("--startup-self-test");
#if JUCE_WINDOWS
        if (!startupSelfTest)
            crashHandler.enableNativeDialog();
#endif
        logInfo(
            "app.lifecycle",
            "Studio Duo "
                + getApplicationVersion()
                + " started on "
                + juce::SystemStats::getOperatingSystemName());
#if JUCE_WINDOWS
        WindowsCrashHandler::exchangeContext(
            WindowsCrashContext::mainWindowStartup);
#endif
        logInfo("app.startup", "Constructing the main window.");
        fileLogger->flush();
        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(),
            !startupSelfTest && !arguments.contains("--safe-audio"),
            startupProjectFromArguments(arguments));
        logInfo("app.startup", "The main window is ready.");
        fileLogger->flush();
#if JUCE_WINDOWS
        WindowsCrashHandler::exchangeContext(
            WindowsCrashContext::runtime);
#endif
        if (startupSelfTest)
        {
            callAfterDelaySafely(750, [this]
            {
                const auto* content = mainWindow != nullptr
                    ? dynamic_cast<MainComponent*>(mainWindow->getContentComponent())
                    : nullptr;
                const auto ready = mainWindow != nullptr
                    && mainWindow->getPeer() != nullptr
                    && mainWindow->isVisible()
                    && content != nullptr
                    && !content->hasAudioDeviceManager();
                setApplicationReturnValue(ready ? 0 : 1);
                if (ready)
                    logInfo("app.self-test", "Main-window startup self-test completed.");
                else
                    logError("app.self-test", "Safe startup did not create a visible, driver-free window.");
                fileLogger->flush();
                systemRequestedQuit();
            });
        }
    }

    void shutdown() override
    {
#if JUCE_WINDOWS
        WindowsCrashHandler::exchangeContext(
            WindowsCrashContext::shutdown);
#endif
        const auto mainApplication = mainWindow != nullptr;
        mainWindow.reset();
        pluginScanWorker.reset();
        pluginBridgeWorker.reset();
        bridgeSelfTest.reset();
        pluginActivationCatalog.reset();
        if (mainApplication)
            logInfo(
                "app.lifecycle",
                "Studio Duo shutdown completed.");
        juce::Logger::setCurrentLogger(nullptr);
        fileLogger.reset();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr
            && !mainWindow->prepareForShutdown())
            return;
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
    }

    void unhandledException(
        const std::exception* exception,
        const juce::String& sourceFile,
        int lineNumber) override
    {
        logError(
            "app.exception",
            juce::String(
                exception != nullptr
                    ? exception->what()
                    : "Unknown exception")
                + " at "
                + juce::File(sourceFile).getFileName()
                + ":"
                + juce::String(lineNumber));
        if (fileLogger != nullptr)
            fileLogger->flush();
        juce::JUCEApplication::unhandledException(
            exception,
            sourceFile,
            lineNumber);
    }

private:
    void runBridgeSelfTest()
    {
        if (bridgeSelfTest == nullptr)
        {
            setApplicationReturnValue(1);
            quit();
            return;
        }

        const auto result = bridgeSelfTest->start();
        if (result.failed())
        {
            logError(
                "plugin.bridge.self-test",
                result.getErrorMessage());
            setApplicationReturnValue(1);
            quit();
            return;
        }

        juce::AudioBuffer<float> firstBlock(2, 512);
        for (int channel = 0; channel < firstBlock.getNumChannels(); ++channel)
            juce::FloatVectorOperations::fill(firstBlock.getWritePointer(channel), 0.25f, 512);

        bridgeSelfTest->processBlock(firstBlock);
        juce::Thread::sleep(50);
        juce::AudioBuffer<float> outputBlock(2, 512);
        outputBlock.clear();
        bridgeSelfTest->processBlock(outputBlock);
        const auto passed = std::abs(outputBlock.getSample(0, 0) - 0.25f) < 0.0001f
            && std::abs(outputBlock.getSample(1, 511) - 0.25f) < 0.0001f;

        const auto diagnostics = bridgeSelfTest->diagnosticState();
        bridgeSelfTest->stop();
        setApplicationReturnValue(passed ? 0 : 1);
        if (!passed)
            logError(
                "plugin.bridge.self-test",
                diagnostics);
        quit();
    }

    void runPluginActivationSelfTest()
    {
        if (bridgeSelfTest == nullptr || pluginActivationCatalog == nullptr)
        {
            setApplicationReturnValue(1);
            quit();
            return;
        }

        const auto entries = pluginActivationCatalog->entries();
        const auto candidate = std::find_if(entries.cbegin(), entries.cend(), [this](const auto& entry)
        {
            return (validationIdentifier.isEmpty()
                    || entry.identifier == validationIdentifier
                    || entry.name.containsIgnoreCase(validationIdentifier))
                && !entry.instrument
                && entry.inputChannels > 0
                && entry.inputChannels <= PluginBridgeSharedState::maxChannels
                && entry.outputChannels > 0
                && entry.outputChannels <= PluginBridgeSharedState::maxChannels;
        });
        if (candidate == entries.cend())
        {
            logError(
                "plugin.bridge.activation-test",
                "No compatible catalog plugin.");
            setApplicationReturnValue(2);
            quit();
            return;
        }

        const auto description = pluginActivationCatalog->descriptionForIdentifier(candidate->identifier);
        if (!description.has_value())
        {
            setApplicationReturnValue(1);
            quit();
            return;
        }

        const auto result = bridgeSelfTest->startPlugin(*description, 48000.0, 512);
        if (result.failed())
        {
            logError(
                "plugin.bridge.activation-test",
                result.getErrorMessage());
            setApplicationReturnValue(1);
            quit();
            return;
        }

        juce::AudioBuffer<float> block(2, 512);
        auto audioPassed = false;
        auto phase = 0.0;
        for (int blockIndex = 0; blockIndex < 32 && !audioPassed; ++blockIndex)
        {
            for (int sample = 0; sample < block.getNumSamples(); ++sample)
            {
                const auto value = static_cast<float>(
                    std::sin(phase) * 0.1);
                phase += juce::MathConstants<double>::twoPi
                    * 220.0
                    / 48000.0;
                for (int channel = 0; channel < block.getNumChannels(); ++channel)
                    block.setSample(channel, sample, value);
            }
            bridgeSelfTest->processBlock(block);
            for (int channel = 0; channel < block.getNumChannels(); ++channel)
            {
                for (int sample = 0; sample < block.getNumSamples(); ++sample)
                {
                    const auto value = block.getSample(channel, sample);
                    audioPassed = audioPassed
                        || (std::isfinite(value)
                            && std::abs(value) > 0.000001f);
                }
            }
            juce::Thread::sleep(2);
        }
        const auto editorResult = bridgeSelfTest->showEditor();
        if (editorResult.wasOk())
            juce::Thread::sleep(50);
        const auto hideResult = editorResult.wasOk()
            ? bridgeSelfTest->hideEditor()
            : editorResult;
        const auto passed = bridgeSelfTest->isReady()
            && audioPassed
            && editorResult.wasOk()
            && hideResult.wasOk();
        if (!passed)
        {
            logError(
                "plugin.bridge.activation-test",
                editorResult.failed()
                    ? editorResult.getErrorMessage()
                    : hideResult.failed()
                        ? hideResult.getErrorMessage()
                        : juce::String(
                            "Sandbox produced no audio."));
        }
        bridgeSelfTest->stop();
        setApplicationReturnValue(passed ? 0 : 1);
        quit();
    }

    void runPluginValidation()
    {
        if (pluginActivationCatalog == nullptr
            || validationIdentifier.isEmpty())
        {
            setApplicationReturnValue(2);
            quit();
            return;
        }
        const auto entries = pluginActivationCatalog->entries();
        const auto entry = std::find_if(
            entries.cbegin(),
            entries.cend(),
            [this](const auto& candidate)
            {
                return !candidate.bundledDevice
                    && (candidate.identifier == validationIdentifier
                        || candidate.name.equalsIgnoreCase(
                            validationIdentifier));
            });
        if (entry == entries.cend())
        {
            std::cout << "{\"status\":\"not-installed\"}\n";
            setApplicationReturnValue(2);
            quit();
            return;
        }
        const auto description =
            pluginActivationCatalog->descriptionForIdentifier(
                entry->identifier);
        if (!description.has_value())
        {
            setApplicationReturnValue(1);
            quit();
            return;
        }
        const auto report =
            PluginCompatibilityValidator::validate(*description);
        std::cout << juce::JSON::toString(report.toVar(), true) << '\n';
        setApplicationReturnValue(report.status == "pass" ? 0 : 1);
        quit();
    }

    void runScreamForgeValidation()
    {
        if (pluginActivationCatalog == nullptr)
        {
            setApplicationReturnValue(1);
            quit();
            return;
        }
        std::vector<juce::PluginDescription> descriptions;
        for (const auto& entry : pluginActivationCatalog->entries())
        {
            if (entry.bundledDevice)
                continue;
            const auto description =
                pluginActivationCatalog->descriptionForIdentifier(
                    entry.identifier);
            if (description.has_value()
                && ScreamForgeValidation::matches(*description))
                descriptions.push_back(*description);
        }
        const auto result =
            ScreamForgeValidation::validateInstalled(descriptions);
        std::cout << juce::JSON::toString(result.toVar(), true) << '\n';
        setApplicationReturnValue(
            result.status == "pass"
                ? 0
                : result.status == "not-installed" ? 2 : 1);
        quit();
    }

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(
            const juce::String& name,
            bool startAudioOnLaunch,
            juce::File startupProject)
            : juce::DocumentWindow(name,
                                   juce::Colour(StudioColours::window),
                                   juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(
                new MainComponent(
                    startAudioOnLaunch,
                    std::move(startupProject)),
                true);
            setBackgroundColour(
                juce::Colour(
                    StudioColours::window));
            setResizable(true, false);

            juce::BorderSize<int> nativeFrame;
            if (auto* peer = getPeer())
            {
                if (const auto frame = peer->getFrameSizeIfPresent())
                    nativeFrame = *frame;
            }

            if (const auto* display = juce::Desktop::getInstance()
                                          .getDisplays()
                                          .getPrimaryDisplay())
            {
                const auto initialBounds =
                    calculateInitialMainWindowBounds(
                        display->userBounds.toNearestInt(),
                        nativeFrame);
                if (!initialBounds.isEmpty())
                {
                    setResizeLimits(
                        juce::jmin(1120, initialBounds.getWidth()),
                        juce::jmin(720, initialBounds.getHeight()),
                        3840,
                        2160);
                    setBoundsConstrained(initialBounds);
                }
                else
                {
                    setResizeLimits(960, 600, 3840, 2160);
                    setSize(1120, 720);
                }
            }
            else
            {
                setResizeLimits(960, 600, 3840, 2160);
                setSize(1120, 720);
            }
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        bool prepareForShutdown()
        {
            if (auto* content =
                    dynamic_cast<MainComponent*>(getContentComponent()))
            {
                return content->prepareForShutdown();
            }
            return true;
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<PluginScanWorker> pluginScanWorker;
    std::unique_ptr<PluginBridgeWorker> pluginBridgeWorker;
    std::unique_ptr<PluginBridgeClient> bridgeSelfTest;
    std::unique_ptr<PluginCatalog> pluginActivationCatalog;
    std::unique_ptr<StudioLogger> fileLogger;
    juce::String validationIdentifier;
#if JUCE_WINDOWS
    WindowsCrashHandler crashHandler;
#endif
};
}
START_JUCE_APPLICATION(studio::StudioDuoApplication)
