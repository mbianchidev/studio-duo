#include "AudioDeviceProbe.h"

#include "logging/StudioLogger.h"

#include <chrono>
#if JUCE_WINDOWS
#include <windows.h>
#include <cstdlib>
#endif

namespace studio
{
namespace
{
struct ProbeFiles
{
    juce::File request = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile(
            "studio-duo-audio-probe-" + juce::Uuid().toString() + ".xml");
    juce::File response = request.getSiblingFile(
        request.getFileNameWithoutExtension() + "-result.xml");
    bool removeFiles = true;

    ~ProbeFiles()
    {
        if (!removeFiles)
            return;
        for (const auto& file : { request, response })
            if (!file.deleteFile())
                logError("audio.probe", "Could not remove " + file.getFileName() + ".");
    }
};
}

juce::String audioDeviceSetupName(const juce::XmlElement& setup)
{
    for (const auto* attribute :
         { "audioDeviceName", "audioInputDeviceName", "audioOutputDeviceName" })
    {
        if (auto name = setup.getStringAttribute(attribute); name.isNotEmpty())
            return name;
    }
    return {};
}

juce::Result probeAudioDeviceSetup(
    const juce::XmlElement& setup,
    const AudioDeviceProbeOptions& options)
{
    return probeAudioDeviceSetupDetailed(setup, options).result;
}

AudioDeviceProbeResult probeAudioDeviceSetupDetailed(
    const juce::XmlElement& setup, const AudioDeviceProbeOptions& options)
{
    const auto fail = [](AudioDeviceProbeFailure failure, const juce::String& message)
    {
        return AudioDeviceProbeResult { juce::Result::fail(message), failure };
    };
    if (options.timeoutMs <= 0 || !options.executable.existsAsFile())
        return fail(AudioDeviceProbeFailure::invalidRequest,
            "Audio startup check requires an executable and a positive timeout.");
    if (!setup.hasTagName("DEVICESETUP"))
        return fail(AudioDeviceProbeFailure::invalidRequest, "The audio startup check received an invalid setup.");
    if (options.cancelled != nullptr && options.cancelled->load(std::memory_order_acquire))
        return fail(AudioDeviceProbeFailure::cancelled, "The audio startup check was cancelled before driver launch.");

    ProbeFiles files;
    if (!files.request.replaceWithText(setup.toString(), false, false, "\n"))
        return fail(AudioDeviceProbeFailure::storage, "Could not write the audio startup check request.");

    const auto deviceName = audioDeviceSetupName(setup);
    logInfo(
        "audio.probe",
        "Checking " + setup.getStringAttribute("deviceType")
            + (deviceName.isEmpty() ? juce::String() : " device " + deviceName)
            + " in a separate process.");
    flushStudioLog();

    juce::ChildProcess worker;
    if (!worker.start(
            { options.executable.getFullPathName(),
              audioDeviceProbeArgument,
              files.request.getFullPathName(),
              files.response.getFullPathName() },
            0))
    {
        return fail(AudioDeviceProbeFailure::launch, "Could not launch the audio startup check process.");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
    auto cancelled = false;
    // A second isRunning() can lose a reaped POSIX child's exit status.
    auto finished = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        cancelled = options.cancelled != nullptr && options.cancelled->load(std::memory_order_acquire);
        if (cancelled)
            break;
        if (worker.waitForProcessToFinish(50))
        {
            finished = true;
            break;
        }
    }
    if (cancelled || !finished)
    {
        const auto stopped = (!worker.isRunning() || worker.kill())
            && worker.waitForProcessToFinish(2000);
        if (!stopped)
        {
            files.removeFiles = false;
            return fail(cancelled ? AudioDeviceProbeFailure::cancelled : AudioDeviceProbeFailure::timeout,
                "The audio startup check could not stop its process. "
                "Close the remaining Studio Duo worker before retrying.");
        }
        if (cancelled)
            return fail(AudioDeviceProbeFailure::cancelled, "The audio startup check was cancelled; its process was stopped.");
        return fail(AudioDeviceProbeFailure::timeout,
            "The audio startup check timed out after "
            + juce::String(options.timeoutMs)
            + " ms; the driver process was stopped.");
    }

    if (const auto exitCode = worker.getExitCode(); exitCode != 0)
    {
        return fail(AudioDeviceProbeFailure::crash,
            "The audio startup check process exited unexpectedly (code 0x"
            + juce::String::toHexString(static_cast<juce::int64>(exitCode))
            + "). The driver was not loaded into Studio Duo.");
    }

    const auto response = juce::parseXML(files.response);
    if (response == nullptr || !response->hasTagName("AUDIO_DEVICE_PROBE_RESULT"))
        return fail(AudioDeviceProbeFailure::protocol, "The audio startup check returned no valid response.");
    const auto status = response->getStringAttribute("status");
    if (status == "ok")
        return {};
    const auto error = response->getStringAttribute("error");
    if (status == "error" && error.isNotEmpty())
        return fail(AudioDeviceProbeFailure::deviceUnavailable, error);
    return fail(AudioDeviceProbeFailure::protocol, "The audio startup check returned an invalid result.");
}

std::optional<int> runAudioDeviceProbeWorker(
    const juce::StringArray& arguments,
    const std::function<juce::Result(const juce::XmlElement&)>& openDevice)
{
    if (arguments.isEmpty() || arguments[0] != audioDeviceProbeArgument)
        return std::nullopt;

#if JUCE_WINDOWS
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#endif

    if (arguments.size() != 3
        || !juce::File::isAbsolutePath(arguments[1])
        || !juce::File::isAbsolutePath(arguments[2])
        || arguments[1] == arguments[2])
    {
        logError("audio.probe", "The audio probe requires distinct absolute request and response paths.");
        return 2;
    }

    const auto setup = juce::parseXML(juce::File(arguments[1]));
    if (setup == nullptr || !setup->hasTagName("DEVICESETUP"))
    {
        logError("audio.probe", "Could not read the audio probe device setup.");
        return 2;
    }

    // Publish only after the driver and its callbacks have been closed by openDevice.
    const auto result = openDevice(*setup);
    juce::XmlElement response("AUDIO_DEVICE_PROBE_RESULT");
    response.setAttribute("status", result.wasOk() ? "ok" : "error");
    if (result.failed())
    {
        response.setAttribute("error", result.getErrorMessage());
        logError("audio.probe", result.getErrorMessage());
    }
    if (!juce::File(arguments[2]).replaceWithText(
            response.toString(), false, false, "\n"))
    {
        logError("audio.probe", "Could not write the audio probe result.");
        return 2;
    }
    return 0;
}
}
