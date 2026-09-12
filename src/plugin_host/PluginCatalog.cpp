#include "PluginCatalog.h"

#include "logging/StudioLogger.h"
#include "PluginFormats.h"
#include "PluginScanWorker.h"
#include "devices/DeviceRegistry.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace studio
{
namespace
{
juce::String currentArchitecture()
{
#if JUCE_ARM
    return JUCE_64BIT ? "arm64" : "arm";
#elif JUCE_64BIT
    return "x86_64";
#else
    return "x86";
#endif
}

PluginCompatibilityRecord compatibilityIdentity(
    const PluginInsert& insert)
{
    PluginCompatibilityRecord record;
    record.pluginIdentifier = insert.pluginIdentifier;
    record.name = insert.name;
    record.format = insert.format;
    record.vendor = insert.manufacturer;
    record.version = insert.version;
    record.architecture = insert.architecture;
    record.preferredMode = insert.bridgeMode;
    record.araCapable = insert.araCapable;
    return record;
}

PluginCompatibilityRecord compatibilityIdentity(
    const PluginCatalogEntry& entry)
{
    PluginCompatibilityRecord record;
    record.pluginIdentifier = entry.identifier;
    record.name = entry.name;
    record.format = entry.format;
    record.vendor = entry.manufacturer;
    record.version = entry.version;
    record.architecture = entry.architecture;
    record.araCapable = entry.araCapable;
    return record;
}

class ScanCoordinator final : private juce::ChildProcessCoordinator
{
public:
    enum class ResponseState
    {
        waiting,
        result,
        connectionLost
    };

    struct Response
    {
        ResponseState state = ResponseState::waiting;
        std::unique_ptr<juce::XmlElement> xml;
    };

    ScanCoordinator()
    {
        launched = launchWorkerProcess(juce::File::getSpecialLocation(juce::File::currentExecutableFile),
                                       pluginScanProcessId,
                                       5000,
                                       0);
    }

    [[nodiscard]] bool isLaunched() const noexcept
    {
        return launched;
    }

    bool send(const juce::MemoryBlock& request)
    {
        return sendMessageToWorker(request);
    }

    Response waitForResponse(std::chrono::milliseconds duration)
    {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, duration, [this] { return responseReady || disconnected; }))
            return {};

        Response response;
        response.state = disconnected ? ResponseState::connectionLost : ResponseState::result;
        response.xml = std::move(responseXml);
        responseReady = false;
        disconnected = false;
        return response;
    }

private:
    void handleMessageFromWorker(const juce::MemoryBlock& message) override
    {
        const std::lock_guard lock(mutex);
        responseXml = juce::parseXML(message.toString());
        responseReady = true;
        condition.notify_one();
    }

    void handleConnectionLost() override
    {
        const std::lock_guard lock(mutex);
        disconnected = true;
        condition.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> responseXml;
    bool launched = false;
    bool responseReady = false;
    bool disconnected = false;
};

class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    explicit OutOfProcessPluginScanner(std::shared_ptr<std::atomic<bool>> cancelFlag)
        : cancelled(std::move(cancelFlag))
    {
    }

    bool findPluginTypesFor(juce::AudioPluginFormat& format,
                            juce::OwnedArray<juce::PluginDescription>& result,
                            const juce::String& fileOrIdentifier) override
    {
        if (cancelled->load(std::memory_order_acquire) || shouldExit())
            return true;

        if (coordinator == nullptr)
            coordinator = std::make_unique<ScanCoordinator>();

        if (!coordinator->isLaunched())
        {
            coordinator.reset();
            return false;
        }

        juce::MemoryBlock request;
        juce::MemoryOutputStream stream(request, true);
        stream.writeString(format.getName());
        stream.writeString(fileOrIdentifier);

        if (!coordinator->send(request))
        {
            coordinator.reset();
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (cancelled->load(std::memory_order_acquire) || shouldExit())
                return true;

            auto response = coordinator->waitForResponse(std::chrono::milliseconds(50));
            if (response.state == ScanCoordinator::ResponseState::waiting)
                continue;

            if (response.state == ScanCoordinator::ResponseState::connectionLost)
            {
                coordinator.reset();
                return false;
            }

            if (response.xml != nullptr)
            {
                for (const auto* item : response.xml->getChildIterator())
                {
                    auto description = std::make_unique<juce::PluginDescription>();
                    if (description->loadFromXml(*item))
                        result.add(std::move(description));
                }
            }
            return true;
        }

        coordinator.reset();
        return false;
    }

    void scanFinished() override
    {
    }

private:
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::unique_ptr<ScanCoordinator> coordinator;
};

juce::Result writeTextAtomically(const juce::File& destination, const juce::String& text)
{
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create the plugin catalog directory.");

    const auto temporary = destination.getSiblingFile(destination.getFileName()
                                                       + ".tmp-"
                                                       + juce::Uuid().toString());
    if (!temporary.replaceWithText(text, false, false, "\n"))
        return juce::Result::fail("Could not write the temporary plugin catalog.");

    const auto replaced = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);
    if (!replaced)
    {
        temporary.deleteFile();
        return juce::Result::fail("Could not atomically replace the plugin catalog.");
    }

    return juce::Result::ok();
}
}

PluginCatalog::PluginCatalog()
    : juce::Thread("Studio Duo plugin scanner"),
      catalogDirectory(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Studio Duo")
                           .getChildFile("Plugin Catalog")),
      catalogFile(catalogDirectory.getChildFile("plugins.xml")),
      deadMansPedalFile(catalogDirectory.getChildFile("scan-deadman.txt")),
      pluginSearchPaths(catalogDirectory.getChildFile("search-paths.json")),
      compatibilityDatabase(
          catalogDirectory.getChildFile("compatibility.json")),
      cancelRequested(std::make_shared<std::atomic<bool>>(false))
{
    catalogDirectory.createDirectory();
    PluginFormats::addSupportedFormats(formatManager);
    for (auto* format : formatManager.getFormats())
    {
        if (format->getName() == "VST3")
        {
            defaultVst3Folders = format->getDefaultLocationsToSearch();
            break;
        }
    }
    knownPlugins.setCustomScanner(std::make_unique<OutOfProcessPluginScanner>(cancelRequested));
    juce::String searchPathError;
    pluginSearchPaths.load(searchPathError);
    load();
    juce::String compatibilityError;
    compatibilityDatabase.load(compatibilityError);
    juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal(knownPlugins,
                                                                      deadMansPedalFile);

    const auto count = knownPlugins.getNumTypes();
    statusMessage = count == 0 ? "No plugins scanned yet."
                               : juce::String(count) + " plugins loaded from catalog.";
    if (searchPathError.isNotEmpty())
    {
        statusMessage << " " << searchPathError;
        logError(
            "plugin.catalog",
            searchPathError);
    }
    if (compatibilityError.isNotEmpty())
    {
        statusMessage << " " << compatibilityError;
        logError(
            "plugin.catalog",
            compatibilityError);
    }
}

PluginCatalog::~PluginCatalog()
{
    cancelScan();
    stopThread(35000);
}

void PluginCatalog::startScan(bool forceRescan)
{
    if (isThreadRunning())
        return;

    cancelRequested->store(false, std::memory_order_release);
    forceNextScan.store(forceRescan, std::memory_order_release);
    scanning.store(true, std::memory_order_release);
    updateState("Preparing sandboxed plugin scan...", 0.0f);
    logInfo(
        "plugin.scan",
        forceRescan
            ? "Started a forced plugin rescan."
            : "Started a plugin scan.");
    if (!startThread())
    {
        const auto message =
            juce::String("Could not start the plugin scanner thread.");
        scanning.store(false, std::memory_order_release);
        updateState(message, 0.0f);
        logError("plugin.scan", message);
    }
}

void PluginCatalog::cancelScan()
{
    cancelRequested->store(true, std::memory_order_release);
    signalThreadShouldExit();
}

bool PluginCatalog::isScanning() const noexcept
{
    return scanning.load(std::memory_order_acquire);
}

float PluginCatalog::progress() const noexcept
{
    return scanProgress.load(std::memory_order_relaxed);
}

juce::String PluginCatalog::status() const
{
    const juce::ScopedLock lock(stateLock);
    return statusMessage;
}

std::vector<PluginCatalogEntry> PluginCatalog::entries() const
{
    std::vector<PluginCatalogEntry> result;
    const auto descriptions = knownPlugins.getTypes();
    result.reserve(static_cast<std::size_t>(descriptions.size()));

    for (const auto& description : descriptions)
    {
        result.push_back({
            description.descriptiveName.isNotEmpty() ? description.descriptiveName : description.name,
            description.manufacturerName,
            description.category,
            description.pluginFormatName,
            description.version,
            currentArchitecture(),
            description.fileOrIdentifier,
            description.createIdentifierString(),
            description.numInputChannels,
            description.numOutputChannels,
            description.isInstrument,
            description.hasARAExtension,
            false
        });
    }
    for (const auto& device : DeviceRegistry::descriptors())
    {
        result.push_back({
            device.name,
            "Studio Duo",
            "Utility",
            "Studio Duo",
            STUDIO_DUO_VERSION,
            currentArchitecture(),
            device.identifier,
            device.identifier,
            2,
            2,
            false,
            false,
            true
        });
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right)
    {
        const auto manufacturerOrder = left.manufacturer.compareIgnoreCase(right.manufacturer);
        return manufacturerOrder == 0 ? left.name.compareIgnoreCase(right.name) < 0
                                      : manufacturerOrder < 0;
    });
    return result;
}

juce::StringArray PluginCatalog::blacklistedFiles() const
{
    return knownPlugins.getBlacklistedFiles();
}

std::uint64_t PluginCatalog::revision() const noexcept
{
    return catalogRevision.load(std::memory_order_acquire);
}

juce::StringArray PluginCatalog::availableFormats() const
{
    juce::StringArray result;
    for (auto* format : formatManager.getFormats())
        result.add(format->getName());
    return result;
}

juce::File PluginCatalog::dataDirectory() const
{
    return catalogDirectory;
}

juce::StringArray PluginCatalog::defaultVst3SearchFolders() const
{
    juce::StringArray paths;
    for (int index = 0; index < defaultVst3Folders.getNumPaths(); ++index)
        paths.add(defaultVst3Folders[index].getFullPathName());
    return PluginSearchPaths::normalizeAndDeduplicate(
        paths,
        PluginSearchPaths::nativePathStyle());
}

juce::StringArray PluginCatalog::customVst3SearchFolders() const
{
    const juce::ScopedLock lock(searchPathLock);
    return pluginSearchPaths.customFolders();
}

juce::Result PluginCatalog::addCustomVst3SearchFolder(
    const juce::File& folder)
{
    if (isScanning())
    {
        const auto message =
            juce::String("Cancel the plugin scan before changing VST3 folders.");
        updateState(message, progress());
        return juce::Result::fail(message);
    }

    for (const auto& defaultFolder : defaultVst3SearchFolders())
    {
        if (juce::File(defaultFolder) == folder)
        {
            updateState(
                "That folder is already included in the default VST3 locations.",
                progress());
            return juce::Result::ok();
        }
    }

    juce::Result result = juce::Result::ok();
    auto wasAlreadyConfigured = false;
    {
        const juce::ScopedLock lock(searchPathLock);
        const auto previousCount = pluginSearchPaths.customFolders().size();
        result = pluginSearchPaths.addCustomFolder(folder);
        wasAlreadyConfigured =
            result.wasOk()
            && pluginSearchPaths.customFolders().size() == previousCount;
    }

    if (result.failed())
        updateState(result.getErrorMessage(), progress());
    else if (wasAlreadyConfigured)
        updateState(
            "VST3 folder already configured: " + folder.getFullPathName(),
            progress());
    else
        updateState(
            "Added VST3 folder: " + folder.getFullPathName()
                + ". Choose SCAN to discover plugins.",
            progress());
    return result;
}

juce::Result PluginCatalog::removeCustomVst3SearchFolder(
    const juce::File& folder)
{
    if (isScanning())
    {
        const auto message =
            juce::String("Cancel the plugin scan before changing VST3 folders.");
        updateState(message, progress());
        return juce::Result::fail(message);
    }

    juce::Result result = juce::Result::ok();
    {
        const juce::ScopedLock lock(searchPathLock);
        result = pluginSearchPaths.removeCustomFolder(folder);
    }
    if (result.failed())
        updateState(result.getErrorMessage(), progress());
    else
        updateState(
            "Removed VST3 folder: " + folder.getFullPathName()
                + ". Future scans will skip it.",
            progress());
    return result;
}

void PluginCatalog::recordRuntimeReady(const PluginInsert& insert)
{
    const juce::ScopedLock compatibilityGuard(compatibilityLock);
    const auto signature = "ready:"
        + pluginBridgeModeToString(insert.bridgeMode);
    const auto existing = std::find_if(
        recordedRuntimeStates.begin(),
        recordedRuntimeStates.end(),
        [&insert](const auto& value)
        {
            return value.first == insert.id;
        });
    if (existing != recordedRuntimeStates.end()
        && existing->second == signature)
        return;
    if (existing != recordedRuntimeStates.end())
        existing->second = signature;
    else
        recordedRuntimeStates.emplace_back(insert.id, signature);

    compatibilityDatabase.noteReady(
        compatibilityIdentity(insert),
        insert.bridgeMode);
    juce::String error;
    if (!compatibilityDatabase.save(error))
        updateState(error, scanProgress.load(std::memory_order_relaxed));
}

void PluginCatalog::recordRuntimeFailure(
    const PluginInsert& insert,
    PluginFailureKind failure,
    const juce::String& message)
{
    const juce::ScopedLock compatibilityGuard(compatibilityLock);
    const auto signature = pluginFailureKindToString(failure)
        + ":"
        + message;
    const auto existing = std::find_if(
        recordedRuntimeStates.begin(),
        recordedRuntimeStates.end(),
        [&insert](const auto& value)
        {
            return value.first == insert.id;
        });
    if (existing != recordedRuntimeStates.end()
        && existing->second == signature)
        return;
    if (existing != recordedRuntimeStates.end())
        existing->second = signature;
    else
        recordedRuntimeStates.emplace_back(insert.id, signature);

    compatibilityDatabase.noteFailure(
        compatibilityIdentity(insert),
        failure,
        message);
    juce::String error;
    if (!compatibilityDatabase.save(error))
        updateState(error, scanProgress.load(std::memory_order_relaxed));
}

void PluginCatalog::recordValidation(
    const PluginCatalogEntry& entry,
    const juce::String& status)
{
    const juce::ScopedLock compatibilityGuard(compatibilityLock);
    compatibilityDatabase.noteValidation(
        compatibilityIdentity(entry),
        status);
    juce::String error;
    if (!compatibilityDatabase.save(error))
        updateState(error, scanProgress.load(std::memory_order_relaxed));
}

std::vector<PluginCompatibilityRecord>
PluginCatalog::compatibilityRecords() const
{
    const juce::ScopedLock compatibilityGuard(compatibilityLock);
    return compatibilityDatabase.records();
}

std::optional<juce::PluginDescription> PluginCatalog::descriptionForIdentifier(
    const juce::String& identifier) const
{
    auto description = knownPlugins.getTypeForIdentifierString(identifier);
    if (description == nullptr)
        return std::nullopt;
    return *description;
}

void PluginCatalog::run()
{
    const auto force = forceNextScan.exchange(false, std::memory_order_acq_rel);
    const auto formatCount = formatManager.getNumFormats();
    juce::StringArray failedFiles;
    juce::StringArray searchPathWarnings;

    for (int formatIndex = 0; formatIndex < formatCount && !threadShouldExit(); ++formatIndex)
    {
        auto* format = formatManager.getFormat(formatIndex);
        if (format == nullptr)
            continue;

        const auto formatName = format->getName();
        auto folders = format->getDefaultLocationsToSearch();
        if (formatName == "VST3")
        {
            PluginSearchPlan plan;
            {
                const juce::ScopedLock lock(searchPathLock);
                plan = pluginSearchPaths.createScanPlan(defaultVst3Folders);
            }
            folders = plan.folders;
            searchPathWarnings.addArray(plan.warnings);
        }

        auto identifiers = format->searchPathsForPlugins(folders,
                                                         true,
                                                         false);
        if (identifiers.isEmpty())
        {
            auto message =
                formatName == "VST3"
                    ? juce::String("No VST3 plugins found in configured locations.")
                    : "No " + formatName + " plugins found in default locations.";
            if (formatName == "VST3" && !searchPathWarnings.isEmpty())
            {
                message << " Skipped custom folders: "
                        << searchPathWarnings.joinIntoString("; ") << ".";
            }
            updateState(message,
                        static_cast<float>(formatIndex + 1) / static_cast<float>(formatCount));
            continue;
        }

        juce::PluginDirectoryScanner scanner(knownPlugins,
                                             *format,
                                             folders,
                                             true,
                                             deadMansPedalFile,
                                             false);
        scanner.setFilesOrIdentifiersToScan(identifiers);

        for (;;)
        {
            if (threadShouldExit() || cancelRequested->load(std::memory_order_acquire))
                break;

            juce::String currentPlugin;
            const auto hasMore = scanner.scanNextFile(!force, currentPlugin);
            const auto totalProgress = (static_cast<float>(formatIndex) + scanner.getProgress())
                / static_cast<float>(formatCount);
            updateState(currentPlugin.isEmpty()
                            ? "Scanning " + formatName + "..."
                            : "Scanning " + currentPlugin + " in a worker process...",
                        totalProgress);
            if (!hasMore)
                break;
        }

        failedFiles.addArray(scanner.getFailedFiles());
    }

    if (threadShouldExit() || cancelRequested->load(std::memory_order_acquire))
    {
        updateState("Plugin scan cancelled.", scanProgress.load(std::memory_order_relaxed));
        logInfo(
            "plugin.scan",
            "Plugin scan cancelled.");
        scanning.store(false, std::memory_order_release);
        catalogRevision.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    const auto saveResult = save();
    const auto pluginCount = knownPlugins.getNumTypes();
    const auto blockedCount = knownPlugins.getBlacklistedFiles().size();
    juce::String completionMessage;
    if (saveResult.failed())
    {
        completionMessage = saveResult.getErrorMessage();
    }
    else if (!failedFiles.isEmpty())
    {
        completionMessage = juce::String(pluginCount)
            + " plugins ready; "
            + juce::String(failedFiles.size())
            + " files did not expose a supported plugin.";
    }
    else
    {
        completionMessage = juce::String(pluginCount)
            + " plugins ready; "
            + juce::String(blockedCount)
            + " blocked after worker failures.";
    }
    if (!searchPathWarnings.isEmpty())
    {
        completionMessage << " Skipped "
                          << juce::String(searchPathWarnings.size())
                          << " custom VST3 folder(s): "
                          << searchPathWarnings.joinIntoString("; ")
                          << ".";
    }
    updateState(completionMessage, 1.0f);
    if (saveResult.failed())
    {
        logError(
            "plugin.scan",
            saveResult.getErrorMessage());
    }
    else if (!failedFiles.isEmpty())
    {
        logError(
            "plugin.scan",
            juce::String(failedFiles.size())
                + " plugin file(s) failed or timed out; "
                + juce::String(pluginCount)
                + " plugins are ready.");
    }
    else
    {
        logInfo(
            "plugin.scan",
            juce::String(pluginCount)
                + " plugins ready; "
                + juce::String(blockedCount)
                + " blocked.");
    }
    {
        const juce::ScopedLock compatibilityGuard(compatibilityLock);
        for (const auto& failedFile : failedFiles)
        {
            PluginCompatibilityRecord record;
            record.pluginIdentifier = "scan:" + failedFile;
            record.name = juce::File(failedFile).getFileName();
            record.architecture = currentArchitecture();
            compatibilityDatabase.noteFailure(
                record,
                PluginFailureKind::scanCrash,
                "Plugin scan worker failed or timed out.");
        }
        if (!failedFiles.isEmpty())
        {
            juce::String compatibilityError;
            if (!compatibilityDatabase.save(compatibilityError))
            {
                updateState(compatibilityError, 1.0f);
                logError(
                    "plugin.catalog",
                    compatibilityError);
            }
        }
    }

    scanning.store(false, std::memory_order_release);
    catalogRevision.fetch_add(1, std::memory_order_acq_rel);
}

void PluginCatalog::load()
{
    if (!catalogFile.existsAsFile())
        return;

    auto xml = juce::parseXML(catalogFile);
    if (xml == nullptr)
    {
        updateState("Plugin catalog is corrupt; a new scan will rebuild it.", 0.0f);
        logError(
            "plugin.catalog",
            "Plugin catalog is corrupt; a new scan will rebuild it.");
        return;
    }

    knownPlugins.recreateFromXml(*xml);
    catalogRevision.fetch_add(1, std::memory_order_acq_rel);
}

juce::Result PluginCatalog::save()
{
    const auto xml = knownPlugins.createXml();
    if (xml == nullptr)
        return juce::Result::fail("Could not serialize the plugin catalog.");

    return writeTextAtomically(catalogFile, xml->toString());
}

void PluginCatalog::updateState(juce::String message, float newProgress)
{
    {
        const juce::ScopedLock lock(stateLock);
        statusMessage = std::move(message);
    }
    scanProgress.store(juce::jlimit(0.0f, 1.0f, newProgress), std::memory_order_relaxed);
    catalogRevision.fetch_add(1, std::memory_order_acq_rel);
}
}
