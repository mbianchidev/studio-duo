#include "UpdateInstaller.h"
#include "StudioDuoUpdateData.h"

#import <Foundation/Foundation.h>

#include <unistd.h>

namespace studio
{
namespace
{
juce::File applicationBundle()
{
    auto location = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    while (!location.isRoot())
    {
        if (location.hasFileExtension("app"))
            return location;
        location = location.getParentDirectory();
    }
    return {};
}

NSString* nsString(const juce::String& value)
{
    return [NSString stringWithUTF8String:value.toRawUTF8()];
}
}

juce::Result launchUpdateInstaller(
    const juce::File& package,
    const juce::String& version)
{
    if (!package.existsAsFile()
        || !package.hasFileExtension("zip"))
        return juce::Result::fail(
            "The downloaded macOS update package is missing or invalid.");

    const auto bundle = applicationBundle();
    if (!bundle.isDirectory())
        return juce::Result::fail(
            "Studio Duo could not locate its application bundle.");
    if (!bundle.hasWriteAccess()
        || !bundle.getParentDirectory().hasWriteAccess())
    {
        return juce::Result::fail(
            "Studio Duo cannot update this application bundle. "
            "Move it to a writable Applications folder and try again.");
    }

    if (bundle.getParentDirectory().getChildFile(".studio-duo-update.lock").exists())
        return juce::Result::fail(
            "Another update or interrupted-update recovery owns this installation. "
            "Preserve the update log and recovery directories before retrying.");
    const auto attempt = "install-studio-duo-" + version + "-" + juce::Uuid().toString();
    const auto helper = package.getSiblingFile(attempt + ".sh");
    const auto swapHelper = package.getSiblingFile(attempt + "-swap");
    const auto fail = [&](juce::String message)
    {
        for (const auto& file : { helper, swapHelper })
            if (file.exists() && !file.deleteFile())
                message += " Could not remove the unused update helper " + file.getFileName() + ".";
        return juce::Result::fail(message);
    };
    const auto bundledSwap = bundle.getChildFile("Contents/Helpers/studio-duo-update-swap");
    if (!bundledSwap.existsAsFile() || !bundledSwap.copyFileTo(swapHelper)
        || !swapHelper.setExecutePermission(true))
    {
        return fail("Studio Duo could not prepare its atomic application-exchange helper.");
    }
    const auto logFile = package.getSiblingFile("install-update.log");
    if (!helper.replaceWithText(
            juce::String::fromUTF8(studio_update::installupdate_sh, studio_update::installupdate_shSize),
            false,
            false,
            "\n"))
    {
        return fail(
            "Studio Duo could not create the macOS update helper.");
    }
    if (!logFile.existsAsFile())
    {
        const auto result = logFile.create();
        if (result.failed())
        {
            return fail(
                "Studio Duo could not create the update log: "
                + result.getErrorMessage());
        }
    }

    @autoreleasepool
    {
        auto* task = [[NSTask alloc] init];
        task.executableURL =
            [NSURL fileURLWithPath:@"/bin/sh"];
        task.arguments = @[
            nsString(helper.getFullPathName()),
            [NSString stringWithFormat:@"%d", static_cast<int>(getpid())],
            nsString(package.getFullPathName()),
            nsString(bundle.getFullPathName()),
            nsString(logFile.getFullPathName()),
            nsString(version),
            @"dev.mbianchi.studioduo",
            nsString(swapHelper.getFullPathName())
        ];

        NSError* launchError = nil;
        const auto launched = [task launchAndReturnError:&launchError];
        [task release];
        if (!launched)
        {
            return fail(
                "Studio Duo could not launch the macOS update helper: "
                + juce::String(
                    launchError != nil
                        ? launchError.localizedDescription.UTF8String
                        : "unknown error"));
        }
    }

    return juce::Result::ok();
}
}
