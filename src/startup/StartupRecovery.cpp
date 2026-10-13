#include "StartupRecovery.h"

#include <limits>
#include <memory>

#if JUCE_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace studio
{
namespace
{
juce::int64 currentPid()
{
#if JUCE_WINDOWS
    return static_cast<juce::int64>(GetCurrentProcessId());
#else
    return static_cast<juce::int64>(getpid());
#endif
}

bool processIsRunning(juce::int64 pid)
{
#if JUCE_WINDOWS
    constexpr auto maximumPid = static_cast<juce::int64>((std::numeric_limits<DWORD>::max)());
#else
    constexpr auto maximumPid = static_cast<juce::int64>((std::numeric_limits<pid_t>::max)());
#endif
    if (pid <= 0 || pid > maximumPid)
        return false;
#if JUCE_WINDOWS
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const auto wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
}

StartupRecovery::StartupRecovery(juce::File folder, ProcessCheck check)
    : directory(std::move(folder)),
      activeMarker(directory.getChildFile("active-" + juce::Uuid().toString() + ".json")),
      processIsAlive(check ? std::move(check) : ProcessCheck(processIsRunning))
{
}

StartupRecoveryCheck StartupRecovery::inspectAndMarkActive()
{
    StartupRecoveryCheck check;
    if (active)
    {
        check.result = juce::Result::fail("This startup session already has an active recovery marker.");
        return check;
    }
    if (!directory.createDirectory())
    {
        check.result = juce::Result::fail("Could not create the startup recovery directory.");
        return check;
    }
    previousMarkers.clear();
    for (const auto& file : directory.findChildFiles(juce::File::findFiles, false, "active-*.json"))
    {
        const auto value = file.getSize() <= 8192 ? juce::JSON::parse(file.loadFileAsString()) : juce::var();
        const auto* marker = value.getDynamicObject();
        const auto pidValue = marker != nullptr ? marker->getProperty("pid") : juce::var();
        const auto valid = marker != nullptr
            && static_cast<int>(marker->getProperty("schemaVersion")) == 1
            && (pidValue.isInt() || pidValue.isInt64());
        if (!valid || !processIsAlive(static_cast<juce::int64>(pidValue)))
            previousMarkers.push_back(file);
    }
    check.unclean = !previousMarkers.empty();
    check.previousSessions = static_cast<int>(previousMarkers.size());
    auto marker = std::make_unique<juce::DynamicObject>();
    marker->setProperty("schemaVersion", 1);
    marker->setProperty("pid", currentPid());
    marker->setProperty("startedAt", juce::Time::getCurrentTime().toISO8601(true));
    if (!activeMarker.replaceWithText(juce::JSON::toString(juce::var(marker.release())), false, false, "\n"))
    {
        check.result = juce::Result::fail("Could not write the active startup recovery marker.");
        return check;
    }
    active = true;
    return check;
}

juce::Result StartupRecovery::acknowledgePreviousSessions()
{
    for (const auto& file : previousMarkers)
        if (!file.deleteFile())
            return juce::Result::fail("Could not acknowledge a previous startup recovery marker.");
    previousMarkers.clear();
    return juce::Result::ok();
}

juce::Result StartupRecovery::finishCleanShutdown()
{
    if (!active)
        return juce::Result::ok();
    if (!activeMarker.deleteFile())
        return juce::Result::fail("Could not clear the current session's recovery marker.");
    active = false;
    return juce::Result::ok();
}

juce::File StartupRecovery::markerFile() const { return activeMarker; }
}
