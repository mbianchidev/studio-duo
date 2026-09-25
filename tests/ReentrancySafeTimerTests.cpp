#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/ReentrancySafeTimer.h"

void reentrancySafeTimerTests()
{
    auto invocationCount = 0;
    auto callbackFinished = false;
    studio::callAfterDelaySafely(
        20,
        [&]
        {
            ++invocationCount;
            juce::Thread::sleep(100);
            juce::Timer::callPendingTimersSynchronously();
            callbackFinished = true;
        });

    for (auto attempt = 0;
         attempt < 100 && !callbackFinished;
         ++attempt)
    {
        juce::MessageManager::getInstance()
            ->runDispatchLoopUntil(10);
    }

    expect(
        callbackFinished,
        "The re-entrancy-safe one-shot timer invokes its callback.");
    expect(
        invocationCount == 1,
        "Nested message dispatch cannot re-enter a one-shot timer callback.");
}
