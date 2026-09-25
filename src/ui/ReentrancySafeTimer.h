#pragma once

#include <juce_events/juce_events.h>

#include <functional>
#include <utility>

namespace studio
{
namespace detail
{
class ReentrancySafeOneShotTimer final : private juce::Timer,
                                         private juce::DeletedAtShutdown
{
public:
    ReentrancySafeOneShotTimer(
        int delayMilliseconds,
        std::function<void()> callbackToRun)
        : callback(std::move(callbackToRun))
    {
        startTimer(delayMilliseconds);
    }

    ~ReentrancySafeOneShotTimer() override
    {
        stopTimer();
    }

private:
    void timerCallback() override
    {
        // Audio drivers and modal UI can dispatch nested messages. Remove this
        // timer before user code runs so the callback cannot re-enter itself.
        stopTimer();
        auto callbackToRun = std::move(callback);
        delete this;

        if (callbackToRun)
            callbackToRun();
    }

    std::function<void()> callback;
};
}

inline void callAfterDelaySafely(
    int delayMilliseconds,
    std::function<void()> callback)
{
    if (callback)
    {
        new detail::ReentrancySafeOneShotTimer(
            delayMilliseconds,
            std::move(callback));
    }
}
}
