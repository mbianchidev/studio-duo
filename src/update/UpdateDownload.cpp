#include "UpdateDownload.h"
#include "UpdateManifest.h"

#include <juce_cryptography/juce_cryptography.h>

#include <array>

namespace studio
{
UpdateDownloadResult downloadUpdatePackage(
    juce::InputStream& input,
    const juce::File& destination,
    juce::int64 expectedBytes,
    const juce::String& expectedSha256,
    const std::function<bool()>& cancelled,
    const std::function<void(double)>& progress)
{
    using Status = UpdateDownloadResult::Status;
    if (expectedBytes <= 0 || expectedBytes > maximumUpdatePackageBytes
        || expectedSha256.length() != 64 || !expectedSha256.containsOnly("0123456789abcdef"))
        return { Status::failed, "The update size or SHA-256 metadata is invalid." };
    if (cancelled())
        return { Status::cancelled, "Update download cancelled; the installed application is unchanged." };
    if (destination.isDirectory())
        return { Status::failed, "The update package destination is a directory." };
    const auto created = destination.getParentDirectory().createDirectory();
    if (created.failed())
        return { Status::failed, "Could not create the update directory: " + created.getErrorMessage() };

    juce::TemporaryFile temporary(destination, destination.getSiblingFile(
        destination.getFileName() + ".part-" + juce::Uuid().toString()));
    auto result = [&]() -> UpdateDownloadResult
    {
        juce::FileOutputStream output(temporary.getFile());
        if (output.failedToOpen())
            return { Status::failed, "Could not create the update package: " + output.getStatus().getErrorMessage() };
        std::array<char, 64 * 1024> buffer {};
        juce::int64 received = 0;
        while (!cancelled())
        {
            const auto count = input.read(buffer.data(), static_cast<int>(buffer.size()));
            if (cancelled())
                break;
            if (count < 0)
                return { Status::failed, "The update download could not be read." };
            if (count == 0)
                break;
            if (count > expectedBytes - received)
                return { Status::failed, "The update download exceeded its declared size." };
            if (!output.write(buffer.data(), static_cast<std::size_t>(count)))
                return { Status::failed, "Could not write the update package: " + output.getStatus().getErrorMessage() };
            received += count;
            progress(static_cast<double>(received) / static_cast<double>(expectedBytes));
        }
        if (cancelled())
            return { Status::cancelled, "Update download cancelled; the installed application is unchanged." };
        output.flush();
        if (output.getStatus().failed())
            return { Status::failed, "Could not finish the update package: " + output.getStatus().getErrorMessage() };
        if (received != expectedBytes)
            return { Status::failed, "The update download was incomplete: expected "
                + juce::String(expectedBytes) + " bytes, received " + juce::String(received) + "." };
        return { Status::complete, {} };
    }();
    if (result.status == Status::complete
        && juce::SHA256(temporary.getFile()).toHexString() != expectedSha256)
        result = { Status::failed, "The update download failed SHA-256 verification." };
    if (result.status == Status::complete && cancelled())
        result = { Status::cancelled, "Update download cancelled; the installed application is unchanged." };
    if (result.status == Status::complete && !temporary.getFile().replaceFileIn(destination))
        result = { Status::failed, "Could not publish the verified update; the previous package was preserved." };
    if (!temporary.deleteTemporaryFile())
        return { Status::failed, result.message + " Could not remove the incomplete update package." };
    return result;
}
}
