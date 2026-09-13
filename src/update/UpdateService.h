#pragma once

#include "UpdateManifest.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace studio
{
enum class UpdatePhase
{
    idle,
    checking,
    upToDate,
    available,
    downloading,
    ready,
    failed,
    unsupported
};

struct UpdateSnapshot
{
    UpdatePhase phase = UpdatePhase::idle;
    juce::String currentVersion;
    juce::String availableVersion;
    juce::String releaseNotesUrl;
    juce::String message;
    double progress = 0.0;
    bool automaticDownloads = true;
};

class UpdateService final : private juce::Thread,
                            private juce::AsyncUpdater
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void updateStateChanged(
            const UpdateSnapshot& snapshot) = 0;
    };

    UpdateService(
        juce::String currentVersion,
        juce::String manifestUrl,
        juce::File storageDirectory = defaultStorageDirectory());
    ~UpdateService() override;

    [[nodiscard]] static juce::File defaultStorageDirectory();
    [[nodiscard]] UpdateSnapshot snapshot() const;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);
    void checkForUpdates();
    void downloadUpdate();
    [[nodiscard]] juce::Result setAutomaticDownloads(bool enabled);
    [[nodiscard]] juce::Result launchReadyUpdate();

private:
    enum class Operation
    {
        none,
        check,
        download
    };

    struct PendingUpdate
    {
        juce::String version;
        juce::String releaseNotesUrl;
        juce::String sha256;
        std::int64_t sizeBytes = 0;
        juce::File file;
    };

    void run() override;
    void handleAsyncUpdate() override;
    void performCheck();
    void performDownload(const UpdateRelease& release);
    void publishFailure(const juce::String& message);
    void publishState(
        UpdatePhase phase,
        const juce::String& message,
        double progress);
    [[nodiscard]] juce::Result saveSettings();
    void loadSettings();
    void cleanupIncompleteDownloads();
    void invalidatePendingUpdate(const juce::String& message);

    const juce::String currentVersion;
    const juce::String manifestUrl;
    const juce::File storageDirectory;
    const juce::File settingsFile;
    const juce::File downloadsDirectory;
    const UpdatePlatform platform = currentUpdatePlatform();

    mutable juce::CriticalSection stateLock;
    juce::CriticalSection settingsLock;
    juce::CriticalSection networkLock;
    UpdateSnapshot state;
    std::optional<UpdateRelease> availableRelease;
    std::optional<PendingUpdate> pendingUpdate;
    juce::ListenerList<Listener> listeners;
    std::atomic<Operation> requestedOperation { Operation::none };
    std::atomic<bool> automaticDownloads { true };
    juce::WebInputStream* activeWebStream = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdateService)
};
}
