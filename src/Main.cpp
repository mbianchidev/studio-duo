#include "ui/MainComponent.h"
#include "plugin_host/PluginScanWorker.h"

#include <juce_gui_extra/juce_gui_extra.h>

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
        auto worker = std::make_unique<PluginScanWorker>();
        if (worker->initialise(commandLine))
        {
            pluginScanWorker = std::move(worker);
            return;
        }

        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        pluginScanWorker.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
    }

private:
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
};
}
START_JUCE_APPLICATION(studio::StudioDuoApplication)
