#include "ui/MainComponent.h"
#include "plugin_host/PluginBridgeClient.h"
#include "plugin_host/PluginBridgeWorker.h"
#include "plugin_host/PluginScanWorker.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>

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

        juce::AudioBuffer<float> firstBlock(2, 64);
        for (int channel = 0; channel < firstBlock.getNumChannels(); ++channel)
            juce::FloatVectorOperations::fill(firstBlock.getWritePointer(channel), 0.25f, 64);

        bridgeSelfTest->processBlock(firstBlock);
        bool passed = false;
        for (int attempt = 0; attempt < 50 && !passed; ++attempt)
        {
            juce::Thread::sleep(2);
            juce::AudioBuffer<float> nextBlock(2, 64);
            for (int channel = 0; channel < nextBlock.getNumChannels(); ++channel)
                juce::FloatVectorOperations::fill(nextBlock.getWritePointer(channel), -0.5f, 64);

            bridgeSelfTest->processBlock(nextBlock);
            passed = std::abs(nextBlock.getSample(0, 0) - 0.25f) < 0.0001f
                && std::abs(nextBlock.getSample(1, 63) - 0.25f) < 0.0001f;
        }

        bridgeSelfTest->stop();
        setApplicationReturnValue(passed ? 0 : 1);
        if (!passed)
            juce::Logger::writeToLog("plugin.bridge.self-test: timed out waiting for worker output");
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
};
}
START_JUCE_APPLICATION(studio::StudioDuoApplication)
