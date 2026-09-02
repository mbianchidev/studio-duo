#include "ui/MainComponent.h"
#include "plugin_host/PluginBridgeClient.h"
#include "plugin_host/PluginBridgeWorker.h"
#include "plugin_host/PluginScanWorker.h"
#include "plugin_host/PluginCompatibilityValidator.h"
#include "plugin_host/ScreamForgeValidation.h"
#include "platform/ApplicationIcon.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace studio
{
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
        auto bridgeWorker = std::make_unique<PluginBridgeWorker>();
        if (bridgeWorker->initialise(commandLine))
        {
            pluginBridgeWorker = std::move(bridgeWorker);
            return;
        }

        auto worker = std::make_unique<PluginScanWorker>();
        if (worker->initialise(commandLine))
        {
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
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        pluginScanWorker.reset();
        pluginBridgeWorker.reset();
        bridgeSelfTest.reset();
        pluginActivationCatalog.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
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
            juce::Logger::writeToLog("plugin.bridge.self-test: " + result.getErrorMessage());
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
            juce::Logger::writeToLog("plugin.bridge.self-test: " + diagnostics);
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
        const auto candidate = std::find_if(entries.cbegin(), entries.cend(), [](const auto& entry)
        {
            return !entry.instrument
                && entry.inputChannels > 0
                && entry.inputChannels <= PluginBridgeSharedState::maxChannels
                && entry.outputChannels > 0
                && entry.outputChannels <= PluginBridgeSharedState::maxChannels;
        });
        if (candidate == entries.cend())
        {
            juce::Logger::writeToLog("plugin.bridge.activation-test: no compatible catalog plugin");
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
            juce::Logger::writeToLog("plugin.bridge.activation-test: " + result.getErrorMessage());
            setApplicationReturnValue(1);
            quit();
            return;
        }

        juce::AudioBuffer<float> block(2, 512);
        block.clear();
        bridgeSelfTest->processBlock(block);
        juce::Thread::sleep(10);
        bridgeSelfTest->processBlock(block);
        const auto passed = bridgeSelfTest->isReady();
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
        explicit MainWindow(const juce::String& name)
            : juce::DocumentWindow(name,
                                   juce::Colour(StudioColours::window),
                                   juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, false);
            setResizeLimits(1120, 720, 3840, 2160);
            centreWithSize(1480, 900);
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<PluginScanWorker> pluginScanWorker;
    std::unique_ptr<PluginBridgeWorker> pluginBridgeWorker;
    std::unique_ptr<PluginBridgeClient> bridgeSelfTest;
    std::unique_ptr<PluginCatalog> pluginActivationCatalog;
    juce::String validationIdentifier;
};
}
START_JUCE_APPLICATION(studio::StudioDuoApplication)
