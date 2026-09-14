#include "TestHarness.h"
#include "TestSuites.h"

#if JUCE_WINDOWS
#include "platform/WindowsCrashHandler.h"

#include <windows.h>

std::optional<int> runWindowsCrashHandlerFixture(
    const juce::StringArray& arguments)
{
    if (arguments.isEmpty() || arguments[0] != "--windows-crash-fixture")
        return std::nullopt;
    if (arguments.size() != 2
        || !juce::File::isAbsolutePath(arguments[1]))
    {
        return 2;
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    studio::WindowsCrashHandler handler;
    if (handler.initialise(juce::File(arguments[1])).failed())
        return 3;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 4;
}

void windowsCrashHandlerTests()
{
    const auto directory = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile(
            "studio-duo-crash-tests-" + juce::Uuid().toString());
    juce::ChildProcess worker;
    const auto started = worker.start(
        { juce::File::getSpecialLocation(
              juce::File::currentExecutableFile).getFullPathName(),
          "--windows-crash-fixture",
          directory.getFullPathName() },
        0);
    expect(started, "The native crash fixture can start in a child process.");
    const auto finished = started && worker.waitForProcessToFinish(10000);
    expect(finished, "Native crash reporting does not block headless workers.");
    if (started && !finished)
    {
        expect(worker.kill(), "A stuck native crash fixture can be terminated.");
        expect(
            worker.waitForProcessToFinish(2000),
            "A terminated native crash fixture is reaped.");
    }
    expect(
        finished && worker.getExitCode() == EXCEPTION_ACCESS_VIOLATION,
        "Native crash reporting preserves the Windows exception exit code.");

    const auto reports = directory.findChildFiles(
        juce::File::findFiles, false, "studio-duo-crash-*.log");
    expect(
        reports.size() == 1
            && reports[0].loadFileAsString().containsIgnoreCase("0xc0000005")
            && reports[0].loadFileAsString().contains("Module:"),
        "Native exceptions leave a flushed report without the asynchronous logger.");
    expect(
        directory.deleteRecursively(),
        "Native crash fixture reports can be cleaned up.");
}
#endif
