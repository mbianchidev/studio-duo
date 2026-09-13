#include "UpdateInstaller.h"

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

const char* installerScript = R"SCRIPT(#!/bin/sh
set -eu

pid="$1"
archive="$2"
target="$3"
log_file="$4"
expected_version="$5"
expected_bundle_id="$6"

exec >>"$log_file" 2>&1
echo "Applying Studio Duo $expected_version update"

while /bin/kill -0 "$pid" 2>/dev/null; do
    /bin/sleep 0.2
done

parent=$(/usr/bin/dirname "$target")
stage=$(/usr/bin/mktemp -d "$parent/.studio-duo-update.XXXXXX")
backup="$parent/.Studio Duo.previous"
update_complete=0

cleanup() {
    /bin/rm -rf "$stage"
    if [ "$update_complete" -eq 0 ] && [ -d "$target" ]; then
        /usr/bin/open "$target"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

/usr/bin/ditto -x -k "$archive" "$stage"
candidate="$stage/Studio Duo.app"
test -d "$candidate"

actual_version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$candidate/Contents/Info.plist")
actual_bundle_id=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$candidate/Contents/Info.plist")
test "$actual_version" = "$expected_version"
test "$actual_bundle_id" = "$expected_bundle_id"
/usr/bin/codesign --verify --deep --strict "$candidate"

/bin/rm -rf "$backup"
/bin/mv "$target" "$backup"
if /bin/mv "$candidate" "$target"; then
    /bin/rm -rf "$backup"
    /bin/rm -f "$archive"
    update_complete=1
    /usr/bin/open "$target"
    /bin/rm -f "$0"
else
    /bin/mv "$backup" "$target"
    exit 1
fi
)SCRIPT";
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

    const auto helper = package.getSiblingFile(
        "install-studio-duo-" + version + ".sh");
    const auto logFile = package.getSiblingFile("install-update.log");
    if (!helper.replaceWithText(
            installerScript,
            false,
            false,
            "\n"))
    {
        return juce::Result::fail(
            "Studio Duo could not create the macOS update helper.");
    }
    if (!logFile.existsAsFile())
    {
        const auto result = logFile.create();
        if (result.failed())
            return juce::Result::fail(
                "Studio Duo could not create the update log: "
                + result.getErrorMessage());
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
            @"dev.mbianchi.studioduo"
        ];

        NSError* launchError = nil;
        const auto launched = [task launchAndReturnError:&launchError];
        [task release];
        if (!launched)
        {
            return juce::Result::fail(
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
