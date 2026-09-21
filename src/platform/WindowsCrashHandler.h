#pragma once

#include <juce_core/juce_core.h>

#include <memory>

#if JUCE_WINDOWS
namespace studio
{
enum class WindowsCrashContext
{
    processStartup,
    audioDeviceProbe,
    pluginWorker,
    mainWindowStartup,
    audioStartup,
    runtime,
    shutdown
};

class WindowsCrashHandler final
{
public:
    WindowsCrashHandler();
    ~WindowsCrashHandler();

    [[nodiscard]] juce::Result initialise(const juce::File& logDirectory);
    void enableNativeDialog() noexcept;
    static WindowsCrashContext exchangeContext(
        WindowsCrashContext context) noexcept;

private:
    struct State;
    std::unique_ptr<State> state;
};
}
#endif
