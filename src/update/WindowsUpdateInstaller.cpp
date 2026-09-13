#include "UpdateInstaller.h"

#include <windows.h>
#include <shellapi.h>

namespace studio
{
juce::Result launchUpdateInstaller(
    const juce::File& package,
    const juce::String&)
{
    if (!package.existsAsFile()
        || !package.hasFileExtension("exe"))
        return juce::Result::fail(
            "The downloaded Windows update installer is missing or invalid.");

    const auto logFile = package.getSiblingFile("install-update.log");
    const auto parameters =
        "/VERYSILENT /NORESTART "
        "/CLOSEAPPLICATIONS /SP- /STUDIODUOUPDATE=1 /LOG=\""
        + logFile.getFullPathName()
        + "\"";

    SHELLEXECUTEINFOW launchInfo {};
    launchInfo.cbSize = sizeof(launchInfo);
    launchInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    launchInfo.lpVerb = L"open";
    launchInfo.lpFile = package.getFullPathName().toWideCharPointer();
    launchInfo.lpParameters = parameters.toWideCharPointer();
    launchInfo.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launchInfo))
    {
        return juce::Result::fail(
            "Studio Duo could not launch the Windows update installer "
            "(error "
            + juce::String(static_cast<int>(GetLastError()))
            + ").");
    }

    if (launchInfo.hProcess != nullptr)
        CloseHandle(launchInfo.hProcess);
    return juce::Result::ok();
}
}
