#include "UpdateService.h"

#include "UpdateInstaller.h"
#include "logging/StudioLogger.h"

#include <juce_cryptography/juce_cryptography.h>

#include <array>
#include <utility>

namespace studio
{
namespace
{
constexpr auto maximumManifestBytes = 1024 * 1024;

class ActiveWebStreamGuard final
{
public:
    ActiveWebStreamGuard(
        juce::CriticalSection& lockToUse,
        juce::WebInputStream*& activeStreamToUse,
        juce::InputStream* input)
        : lock(lockToUse),
          activeStream(activeStreamToUse)
    {
        const juce::ScopedLock scopedLock(lock);
        activeStream =
            dynamic_cast<juce::WebInputStream*>(input);
    }

    ~ActiveWebStreamGuard()
    {
        const juce::ScopedLock scopedLock(lock);
        activeStream = nullptr;
    }

private:
    juce::CriticalSection& lock;
    juce::WebInputStream*& activeStream;
};

bool numericValue(const juce::var& value)
{
    return value.isInt() || value.isInt64() || value.isDouble();
}

juce::Result writeJsonAtomically(
    const juce::File& destination,
    const juce::var& value)
{
    const auto directoryResult =
        destination.getParentDirectory().createDirectory();
    if (directoryResult.failed())
        return directoryResult;

    const auto temporary = destination.getSiblingFile(
        destination.getFileName()
        + ".tmp-"
        + juce::Uuid().toString());
    if (!temporary.replaceWithText(
            juce::JSON::toString(value, true),
            false,
            false,
            "\n"))
    {
        return juce::Result::fail(
            "Could not write the temporary update settings file.");
    }

    const auto replaced = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);
    if (!replaced)
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not publish the update settings file.");
    }
    return juce::Result::ok();
}

juce::String requestHeaders(
    const juce::String& currentVersion,
    const juce::String& accept)
{
    return "User-Agent: Studio-Duo/"
        + currentVersion
        + "\r\nAccept: "
        + accept
        + "\r\nCache-Control: no-cache\r\n";
}
}

UpdateService::UpdateService(
    juce::String currentVersionValue,
    juce::String manifestUrlValue,
    juce::File storageDirectoryValue)
    : juce::Thread("Studio Duo updater"),
      currentVersion(std::move(currentVersionValue)),
      manifestUrl(std::move(manifestUrlValue)),
      storageDirectory(std::move(storageDirectoryValue)),
      settingsFile(
          storageDirectory.getChildFile("update-settings.json")),
      downloadsDirectory(
          storageDirectory.getChildFile("Updates"))
{
    state.currentVersion = currentVersion;
    state.message = "Updates are checked when Studio Duo starts.";
    if (platform == UpdatePlatform::windowsPortable)
    {
        state.phase = UpdatePhase::unsupported;
        state.message =
            "Automatic updates are unavailable in the portable Windows build. "
            "Install Studio Duo with Setup to enable in-app updates.";
        return;
    }
    if (platform == UpdatePlatform::unsupported)
    {
        state.phase = UpdatePhase::unsupported;
        state.message =
            "Automatic updates are not available on this platform.";
        return;
    }
    if (!processLock.enter(0))
    {
        state.phase = UpdatePhase::unsupported;
        state.message =
            "Updates are being managed by another Studio Duo window.";
        return;
    }
    ownsProcessLock = true;
    cleanupIncompleteDownloads();
    loadSettings();

    threadStarted =
        startThread(juce::Thread::Priority::background);
    if (!threadStarted)
    {
        publishFailure(
            "Studio Duo could not start the background update service.");
    }
}

UpdateService::~UpdateService()
{
    if (threadStarted)
    {
        signalThreadShouldExit();
        {
            const juce::ScopedLock lock(networkLock);
            if (activeWebStream != nullptr)
                activeWebStream->cancel();
        }
        notify();
        if (!stopThread(12000))
        {
            logError(
                "update.shutdown",
                "The update worker did not stop cleanly.");
        }
    }
    cancelPendingUpdate();
    if (ownsProcessLock)
        processLock.exit();
}

juce::File UpdateService::defaultStorageDirectory()
{
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Studio Duo");
}

UpdateSnapshot UpdateService::snapshot() const
{
    const juce::ScopedLock lock(stateLock);
    return state;
}

void UpdateService::addListener(Listener* listener)
{
    listeners.add(listener);
}

void UpdateService::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

void UpdateService::checkForUpdates()
{
    if (!ownsProcessLock)
    {
        return;
    }

    {
        const juce::ScopedLock lock(stateLock);
        if (state.phase == UpdatePhase::checking
            || state.phase == UpdatePhase::downloading)
            return;
        state.phase = UpdatePhase::checking;
        state.message = "Checking for updates...";
        state.progress = 0.0;
    }
    triggerAsyncUpdate();
    requestedOperation.store(Operation::check);
    notify();
}

void UpdateService::downloadUpdate()
{
    {
        const juce::ScopedLock lock(stateLock);
        if (!availableRelease.has_value()
            || state.phase == UpdatePhase::checking
            || state.phase == UpdatePhase::downloading
            || state.phase == UpdatePhase::ready)
            return;
        state.phase = UpdatePhase::downloading;
        state.message = "Starting the update download...";
        state.progress = 0.0;
    }
    triggerAsyncUpdate();
    requestedOperation.store(Operation::download);
    notify();
}

juce::Result UpdateService::setAutomaticDownloads(bool enabled)
{
    if (!ownsProcessLock)
        return juce::Result::fail(state.message);

    const auto previous = automaticDownloads.exchange(enabled);
    {
        const juce::ScopedLock lock(stateLock);
        state.automaticDownloads = enabled;
    }

    const auto result = saveSettings();
    if (result.failed())
    {
        automaticDownloads.store(previous);
        {
            const juce::ScopedLock lock(stateLock);
            state.automaticDownloads = previous;
        }
        triggerAsyncUpdate();
        logError("update.settings", result.getErrorMessage());
        return result;
    }

    triggerAsyncUpdate();
    if (enabled)
        downloadUpdate();
    return juce::Result::ok();
}

juce::Result UpdateService::launchReadyUpdate()
{
    if (!ownsProcessLock)
        return juce::Result::fail(state.message);

    std::optional<PendingUpdate> pending;
    {
        const juce::ScopedLock lock(stateLock);
        pending = pendingUpdate;
    }
    if (!pending.has_value())
        return juce::Result::fail(
            "No downloaded Studio Duo update is ready to install.");
    if (!pending->file.existsAsFile()
        || pending->file.getSize() != pending->sizeBytes)
    {
        invalidatePendingUpdate(
            "The downloaded update package is missing or incomplete.");
        return juce::Result::fail(
            "The downloaded update package is missing or incomplete. "
            "Check for updates and download it again.");
    }

    const auto actualHash =
        juce::SHA256(pending->file).toHexString().toLowerCase();
    if (actualHash != pending->sha256)
    {
        invalidatePendingUpdate(
            "The downloaded update package failed SHA-256 verification.");
        return juce::Result::fail(
            "The downloaded update package failed integrity verification. "
            "It was removed; download it again.");
    }

    const auto result = launchUpdateInstaller(
        pending->file,
        pending->version);
    if (result.failed())
    {
        logError("update.install", result.getErrorMessage());
        return result;
    }

    logInfo(
        "update.install",
        "Launched Studio Duo "
            + pending->version
            + " update installer.");
    return juce::Result::ok();
}

void UpdateService::run()
{
    while (!threadShouldExit())
    {
        const auto operation =
            requestedOperation.exchange(Operation::none);
        if (operation == Operation::none)
        {
            wait(-1);
            continue;
        }

        if (operation == Operation::check)
        {
            performCheck();
            continue;
        }

        std::optional<UpdateRelease> release;
        {
            const juce::ScopedLock lock(stateLock);
            release = availableRelease;
        }
        if (release.has_value())
            performDownload(*release);
    }
}

void UpdateService::handleAsyncUpdate()
{
    const auto current = snapshot();
    listeners.call(
        &Listener::updateStateChanged,
        current);
}

void UpdateService::performCheck()
{
    int statusCode = 0;
    auto input = juce::URL(manifestUrl).createInputStream(
        juce::URL::InputStreamOptions(
            juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(10000)
            .withNumRedirectsToFollow(5)
            .withStatusCode(&statusCode)
            .withExtraHeaders(
                requestHeaders(currentVersion, "application/json")));
    if (threadShouldExit())
        return;
    if (input == nullptr)
    {
        publishCheckFailure(
            "Studio Duo could not connect to the update service.");
        return;
    }
    [[maybe_unused]] ActiveWebStreamGuard activeStreamGuard(
        networkLock,
        activeWebStream,
        input.get());
    if (statusCode != 200)
    {
        publishCheckFailure(
            "The update service returned HTTP "
            + juce::String(statusCode)
            + ".");
        return;
    }

    juce::MemoryBlock manifestData;
    input->readIntoMemoryBlock(
        manifestData,
        maximumManifestBytes + 1);
    if (threadShouldExit())
        return;
    if (manifestData.getSize() > maximumManifestBytes)
    {
        publishCheckFailure(
            "The update manifest is larger than the supported limit.");
        return;
    }

    juce::String parseError;
    const auto release = parseUpdateManifest(
        juce::String::fromUTF8(
            static_cast<const char*>(manifestData.getData()),
            static_cast<int>(manifestData.getSize())),
        platform,
        parseError);
    if (!release.has_value())
    {
        publishCheckFailure(parseError);
        return;
    }

    std::optional<PendingUpdate> pending;
    {
        const juce::ScopedLock lock(stateLock);
        pending = pendingUpdate;
    }
    const auto pendingMatchesRelease =
        pending.has_value()
        && pending->version == release->version
        && pending->file.getFileName()
            == release->asset.fileName
        && pending->sizeBytes == release->asset.sizeBytes
        && pending->sha256 == release->asset.sha256;
    const auto pendingIsNewer =
        pending.has_value()
        && isNewerSemanticVersion(
            pending->version,
            release->version);
    if (pendingMatchesRelease || pendingIsNewer)
    {
        {
            const juce::ScopedLock lock(stateLock);
            availableRelease =
                release->version == pending->version
                    ? release
                    : std::optional<UpdateRelease> {};
            state.phase = UpdatePhase::ready;
            state.availableVersion = pending->version;
            state.releaseNotesUrl = pending->releaseNotesUrl;
            state.message =
                "Studio Duo "
                + pending->version
                + " is downloaded. Restart when you are ready to update.";
            state.progress = 1.0;
        }
        triggerAsyncUpdate();
        return;
    }
    if (pending.has_value())
    {
        {
            const juce::ScopedLock lock(stateLock);
            pendingUpdate.reset();
        }
        const auto saveResult = saveSettings();
        if (saveResult.failed())
        {
            {
                const juce::ScopedLock lock(stateLock);
                pendingUpdate = pending;
            }
            publishCheckFailure(saveResult.getErrorMessage());
            return;
        }
        if (pending->file.existsAsFile()
            && !pending->file.deleteFile())
        {
            logError(
                "update.cleanup",
                "Could not remove superseded update package "
                    + pending->file.getFileName()
                    + ".");
        }
    }

    if (!isNewerSemanticVersion(
            release->version,
            currentVersion))
    {
        {
            const juce::ScopedLock lock(stateLock);
            availableRelease.reset();
            state.phase = UpdatePhase::upToDate;
            state.availableVersion.clear();
            state.releaseNotesUrl = release->releaseNotesUrl;
            state.message =
                "Studio Duo " + currentVersion + " is up to date.";
            state.progress = 1.0;
        }
        logInfo(
            "update.check",
            "Studio Duo " + currentVersion + " is up to date.");
        triggerAsyncUpdate();
        return;
    }

    {
        const juce::ScopedLock lock(stateLock);
        availableRelease = *release;
        state.phase = UpdatePhase::available;
        state.availableVersion = release->version;
        state.releaseNotesUrl = release->releaseNotesUrl;
        state.message =
            "Studio Duo "
            + release->version
            + " is available.";
        state.progress = 0.0;
    }
    logInfo(
        "update.check",
        "Studio Duo "
            + release->version
            + " is available.");
    triggerAsyncUpdate();

    if (automaticDownloads.load())
        performDownload(*release);
}

void UpdateService::performDownload(const UpdateRelease& release)
{
    publishState(
        UpdatePhase::downloading,
        "Downloading Studio Duo "
            + release.version
            + "...",
        0.0);

    const auto directoryResult = downloadsDirectory.createDirectory();
    if (directoryResult.failed())
    {
        publishFailure(
            "Studio Duo could not create its update directory: "
            + directoryResult.getErrorMessage());
        return;
    }

    const auto destination =
        downloadsDirectory.getChildFile(release.asset.fileName);
    const auto temporary = downloadsDirectory.getChildFile(
        release.asset.fileName
        + ".part-"
        + juce::Uuid().toString());

    int statusCode = 0;
    auto input = juce::URL(release.asset.url).createInputStream(
        juce::URL::InputStreamOptions(
            juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(10000)
            .withNumRedirectsToFollow(5)
            .withStatusCode(&statusCode)
            .withExtraHeaders(
                requestHeaders(
                    currentVersion,
                    "application/octet-stream")));
    if (threadShouldExit())
        return;
    if (input == nullptr)
    {
        publishFailure(
            "Studio Duo could not connect to the update download.");
        return;
    }
    [[maybe_unused]] ActiveWebStreamGuard activeStreamGuard(
        networkLock,
        activeWebStream,
        input.get());
    if (statusCode != 200)
    {
        publishFailure(
            "The update download returned HTTP "
            + juce::String(statusCode)
            + ".");
        return;
    }

    auto output =
        std::make_unique<juce::FileOutputStream>(temporary);
    if (output->failedToOpen())
    {
        publishFailure(
            "Studio Duo could not create the update package: "
            + output->getStatus().getErrorMessage());
        return;
    }

    std::array<char, 64 * 1024> buffer {};
    std::int64_t bytesDownloaded = 0;
    auto lastPublishedProgress = -1.0;
    while (!threadShouldExit())
    {
        const auto bytesRead = input->read(
            buffer.data(),
            static_cast<int>(buffer.size()));
        if (bytesRead <= 0)
            break;
        if (!output->write(
                buffer.data(),
                static_cast<std::size_t>(bytesRead)))
        {
            output->flush();
            output.reset();
            temporary.deleteFile();
            publishFailure(
                "Studio Duo could not write the update package.");
            return;
        }

        bytesDownloaded += bytesRead;
        if (bytesDownloaded > release.asset.sizeBytes)
        {
            output->flush();
            output.reset();
            temporary.deleteFile();
            publishFailure(
                "The update download exceeded its declared size.");
            return;
        }
        const auto progress = juce::jlimit(
            0.0,
            1.0,
            static_cast<double>(bytesDownloaded)
                / static_cast<double>(release.asset.sizeBytes));
        if (progress - lastPublishedProgress >= 0.005)
        {
            lastPublishedProgress = progress;
            publishState(
                UpdatePhase::downloading,
                "Downloading Studio Duo "
                    + release.version
                    + "... "
                    + juce::String(
                        static_cast<int>(progress * 100.0))
                    + "%",
                progress);
        }
    }

    output->flush();
    const auto outputStatus = output->getStatus();
    output.reset();
    if (threadShouldExit())
    {
        temporary.deleteFile();
        return;
    }
    if (outputStatus.failed())
    {
        temporary.deleteFile();
        publishFailure(
            "Studio Duo could not finish the update download: "
            + outputStatus.getErrorMessage());
        return;
    }
    if (bytesDownloaded != release.asset.sizeBytes)
    {
        temporary.deleteFile();
        publishFailure(
            "The update download was incomplete: expected "
            + juce::String(release.asset.sizeBytes)
            + " bytes, received "
            + juce::String(bytesDownloaded)
            + ".");
        return;
    }

    const auto actualHash =
        juce::SHA256(temporary).toHexString().toLowerCase();
    if (actualHash != release.asset.sha256)
    {
        temporary.deleteFile();
        publishFailure(
            "The update download failed SHA-256 verification.");
        return;
    }

    if (destination.existsAsFile()
        && !destination.deleteFile())
    {
        temporary.deleteFile();
        publishFailure(
            "Studio Duo could not replace the previous update package.");
        return;
    }
    if (!temporary.moveFileTo(destination))
    {
        temporary.deleteFile();
        publishFailure(
            "Studio Duo could not publish the downloaded update.");
        return;
    }

    {
        const juce::ScopedLock lock(stateLock);
        pendingUpdate = PendingUpdate {
            release.version,
            release.releaseNotesUrl,
            release.asset.sha256,
            release.asset.sizeBytes,
            destination
        };
    }
    const auto saveResult = saveSettings();
    if (saveResult.failed())
    {
        {
            const juce::ScopedLock lock(stateLock);
            pendingUpdate.reset();
        }
        destination.deleteFile();
        publishFailure(saveResult.getErrorMessage());
        return;
    }

    publishState(
        UpdatePhase::ready,
        "Studio Duo "
            + release.version
            + " is downloaded. Restart when you are ready to update.",
        1.0);
    logInfo(
        "update.download",
        "Studio Duo "
            + release.version
            + " update downloaded and verified.");
}

void UpdateService::publishFailure(const juce::String& message)
{
    publishState(UpdatePhase::failed, message, 0.0);
    logError("update", message);
}

void UpdateService::publishCheckFailure(
    const juce::String& message)
{
    {
        const juce::ScopedLock lock(stateLock);
        if (pendingUpdate.has_value())
        {
            state.phase = UpdatePhase::ready;
            state.availableVersion = pendingUpdate->version;
            state.releaseNotesUrl = pendingUpdate->releaseNotesUrl;
            state.message =
                "Studio Duo "
                + pendingUpdate->version
                + " is downloaded. A check for newer updates failed: "
                + message;
            state.progress = 1.0;
        }
        else
        {
            state.phase = UpdatePhase::failed;
            state.message = message;
            state.progress = 0.0;
        }
    }
    logError("update.check", message);
    triggerAsyncUpdate();
}

void UpdateService::publishState(
    UpdatePhase phase,
    const juce::String& message,
    double progress)
{
    {
        const juce::ScopedLock lock(stateLock);
        state.phase = phase;
        state.message = message;
        state.progress = progress;
    }
    triggerAsyncUpdate();
}

juce::Result UpdateService::saveSettings()
{
    if (!ownsProcessLock)
    {
        return juce::Result::fail(
            "Updates are being managed by another Studio Duo window.");
    }

    const juce::ScopedLock settingsSaveLock(settingsLock);
    bool autoDownload = true;
    std::optional<PendingUpdate> pending;
    {
        const juce::ScopedLock lock(stateLock);
        autoDownload = state.automaticDownloads;
        pending = pendingUpdate;
    }

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schemaVersion", 1);
    root->setProperty("automaticDownloads", autoDownload);
    if (pending.has_value())
    {
        auto pendingValue = std::make_unique<juce::DynamicObject>();
        pendingValue->setProperty("version", pending->version);
        pendingValue->setProperty(
            "releaseNotesUrl",
            pending->releaseNotesUrl);
        pendingValue->setProperty("sha256", pending->sha256);
        pendingValue->setProperty("sizeBytes", pending->sizeBytes);
        pendingValue->setProperty(
            "fileName",
            pending->file.getFileName());
        root->setProperty(
            "pendingUpdate",
            juce::var(pendingValue.release()));
    }

    return writeJsonAtomically(
        settingsFile,
        juce::var(root.release()));
}

void UpdateService::loadSettings()
{
    state.automaticDownloads = true;
    automaticDownloads.store(true);
    if (!settingsFile.existsAsFile())
        return;

    const auto rootValue =
        juce::JSON::parse(settingsFile.loadFileAsString());
    const auto* root = rootValue.getDynamicObject();
    if (root == nullptr
        || !numericValue(root->getProperty("schemaVersion"))
        || static_cast<int>(
               root->getProperty("schemaVersion"))
            != 1)
    {
        logError(
            "update.settings",
            "Update settings are corrupt or unsupported; defaults are in use.");
        return;
    }

    const auto autoValue =
        root->getProperty("automaticDownloads");
    if (autoValue.isBool())
    {
        const auto enabled = static_cast<bool>(autoValue);
        state.automaticDownloads = enabled;
        automaticDownloads.store(enabled);
    }

    const auto* pendingObject =
        root->getProperty("pendingUpdate").getDynamicObject();
    if (pendingObject == nullptr
        || platform == UpdatePlatform::unsupported)
        return;

    PendingUpdate pending;
    pending.version =
        pendingObject->getProperty("version").toString().trim();
    pending.releaseNotesUrl =
        pendingObject->getProperty("releaseNotesUrl").toString().trim();
    pending.sha256 =
        pendingObject->getProperty("sha256").toString().trim().toLowerCase();
    const auto sizeValue =
        pendingObject->getProperty("sizeBytes");
    const auto fileName =
        pendingObject->getProperty("fileName").toString().trim();
    const auto expectedFileName =
        expectedUpdateAssetFileName(platform, pending.version);
    if (!parseSemanticVersion(pending.version).has_value()
        || !numericValue(sizeValue)
        || fileName != expectedFileName
        || fileName != juce::File(fileName).getFileName()
        || pending.sha256.length() != 64
        || !pending.sha256.containsOnly(
            "0123456789abcdef"))
    {
        logError(
            "update.settings",
            "The pending update record is invalid and was ignored.");
        const auto saveResult = saveSettings();
        if (saveResult.failed())
            logError("update.settings", saveResult.getErrorMessage());
        return;
    }

    pending.sizeBytes =
        static_cast<juce::int64>(sizeValue);
    pending.file = downloadsDirectory.getChildFile(fileName);
    if (!isNewerSemanticVersion(
            pending.version,
            currentVersion))
    {
        const auto removed =
            !pending.file.existsAsFile()
            || pending.file.deleteFile();
        if (!removed)
        {
            logError(
                "update.settings",
                "Could not remove an obsolete update package; "
                "cleanup will be retried on the next launch.");
            return;
        }
        const auto saveResult = saveSettings();
        if (saveResult.failed())
            logError("update.settings", saveResult.getErrorMessage());
        return;
    }
    if (pending.sizeBytes <= 0
        || !pending.file.existsAsFile()
        || pending.file.getSize() != pending.sizeBytes)
    {
        if (pending.file.existsAsFile())
            pending.file.deleteFile();
        logError(
            "update.settings",
            "The pending update package is missing or incomplete.");
        const auto saveResult = saveSettings();
        if (saveResult.failed())
            logError("update.settings", saveResult.getErrorMessage());
        return;
    }

    pendingUpdate = pending;
    state.phase = UpdatePhase::ready;
    state.availableVersion = pending.version;
    state.releaseNotesUrl = pending.releaseNotesUrl;
    state.message =
        "Studio Duo "
        + pending.version
        + " is downloaded. Restart when you are ready to update.";
    state.progress = 1.0;
}

void UpdateService::cleanupIncompleteDownloads()
{
    if (!downloadsDirectory.isDirectory())
        return;

    for (const auto& file : downloadsDirectory.findChildFiles(
             juce::File::findFiles,
             false,
             "*.part-*"))
    {
        if (!file.deleteFile())
        {
            logError(
                "update.cleanup",
                "Could not remove incomplete update download "
                    + file.getFileName()
                    + ".");
        }
    }
}

void UpdateService::invalidatePendingUpdate(
    const juce::String& message)
{
    juce::File package;
    {
        const juce::ScopedLock lock(stateLock);
        if (pendingUpdate.has_value())
            package = pendingUpdate->file;
        pendingUpdate.reset();
    }
    if (package.existsAsFile()
        && !package.deleteFile())
    {
        logError(
            "update.cleanup",
            "Could not remove invalid update package "
                + package.getFileName()
                + ".");
    }
    const auto saveResult = saveSettings();
    if (saveResult.failed())
        logError("update.settings", saveResult.getErrorMessage());
    publishFailure(message);
}
}
