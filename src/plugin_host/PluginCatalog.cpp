#include "PluginCatalog.h"

#include "PluginScanWorker.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace studio
{
namespace
{
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
      cancelRequested(std::make_shared<std::atomic<bool>>(false))
{
    catalogDirectory.createDirectory();
    juce::addDefaultFormatsToManager(formatManager);
    knownPlugins.setCustomScanner(std::make_unique<OutOfProcessPluginScanner>(cancelRequested));
    load();
    juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal(knownPlugins,
                                                                      deadMansPedalFile);

    const auto count = knownPlugins.getNumTypes();
    statusMessage = count == 0 ? "No plugins scanned yet."
                               : juce::String(count) + " plugins loaded from catalog.";
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
    startThread();
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
            description.fileOrIdentifier,
            description.createIdentifierString(),
            description.numInputChannels,
            description.numOutputChannels,
            description.isInstrument
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

void PluginCatalog::run()
{
    const auto force = forceNextScan.exchange(false, std::memory_order_acq_rel);
    const auto formatCount = formatManager.getNumFormats();
    juce::StringArray failedFiles;

    for (int formatIndex = 0; formatIndex < formatCount && !threadShouldExit(); ++formatIndex)
    {
        auto* format = formatManager.getFormat(formatIndex);
        if (format == nullptr)
            continue;

        auto identifiers = format->searchPathsForPlugins(format->getDefaultLocationsToSearch(),
                                                         true,
                                                         false);
        if (identifiers.isEmpty())
        {
            updateState("No " + format->getName() + " plugins found in default locations.",
                        static_cast<float>(formatIndex + 1) / static_cast<float>(formatCount));
            continue;
        }

        juce::PluginDirectoryScanner scanner(knownPlugins,
                                             *format,
                                             format->getDefaultLocationsToSearch(),
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
                            ? "Scanning " + format->getName() + "..."
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
        scanning.store(false, std::memory_order_release);
        catalogRevision.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    const auto saveResult = save();
    const auto pluginCount = knownPlugins.getNumTypes();
    const auto blockedCount = knownPlugins.getBlacklistedFiles().size();
    if (saveResult.failed())
    {
        updateState(saveResult.getErrorMessage(), 1.0f);
    }
    else if (!failedFiles.isEmpty())
    {
        updateState(juce::String(pluginCount)
                        + " plugins ready; "
                        + juce::String(failedFiles.size())
                        + " files did not expose a supported plugin.",
                    1.0f);
    }
    else
    {
        updateState(juce::String(pluginCount)
                        + " plugins ready; "
                        + juce::String(blockedCount)
                        + " blocked after worker failures.",
                    1.0f);
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
