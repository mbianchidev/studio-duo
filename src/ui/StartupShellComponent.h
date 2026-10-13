#pragma once

#include "StartupProgressComponent.h"
#include "StudioTheme.h"
#include "startup/StartupDiagnostics.h"
#include "startup/StartupRecovery.h"
#include "plugin_host/PluginCatalog.h"
#include "update/StudioPreferences.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace studio
{
class MainComponent;

struct StartupOptions
{
    bool safeAudio = false;
    bool safePlugins = false;
    bool defaultAudio = false;
    bool selfTest = false;
    juce::String selfTestScenario;
    juce::File project;
};

class StartupShellComponent final : public juce::Component, private juce::Timer
{
public:
    explicit StartupShellComponent(StartupOptions options);
    ~StartupShellComponent() override;
    bool prepareForShutdown();
    void resized() override;
    std::function<void(bool, const juce::var&)> onSelfTestFinished;

private:
    struct Completion
    {
        explicit Completion(StartupToken value) : token(value) {}
        StartupToken token;
        StartupOutcome outcome;
        std::unique_ptr<StudioPreferences> preferences;
        std::unique_ptr<PluginCatalog> catalog;
        std::unique_ptr<juce::XmlElement> audioSetup;
        StartupRecoveryCheck recoveryCheck;
        bool audioPreflightPassed = false;
        bool invalidAudioSettings = false;
        bool ioDiscovery = false;
    };
    struct Mailbox
    {
        std::mutex mutex;
        std::vector<Completion> completed;
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> waitingForCatalogHeartbeats { false };
        std::atomic<int> catalogHeartbeats { 0 };
    };

    static std::vector<StartupTask> tasks();
    static void post(const std::shared_ptr<Mailbox>& mailbox, Completion completion);
    void begin();
    void timerCallback() override;
    void startTask(StartupToken token);
    void collectCompletions();
    void activateAudio();
    void retryAudio();
    void openIo();
    void continueWithoutAudio();
    void acceptRecovery(bool safe);
    void enterSafeStart();
    void refreshSurface();
    void updatePluginSummary();
    void updateScan();
    void persistReport(StartupToken token);
    void exportReport();
    [[nodiscard]] juce::var report() const;
    [[nodiscard]] bool requiredServicesAvailable() const;
    void checkSelfTest();

    StartupOptions options;
    StartupCoordinator startup { tasks() };
    StudioTheme theme;
    StartupProgressComponent surface;
    std::unique_ptr<MainComponent> workspace;
    juce::File dataDirectory;
    juce::File testDirectory;
    std::shared_ptr<StartupRecovery> recovery;
    std::shared_ptr<StartupDiagnosticStore> diagnostics;
    juce::ThreadPool workers { 3 };
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::unique_ptr<StudioPreferences> restoredPreferences;
    std::unique_ptr<PluginCatalog> restoredCatalog;
    std::unique_ptr<juce::XmlElement> savedAudioSetup;
    std::shared_ptr<std::atomic<bool>> audioProbeCancellation;
    std::optional<StartupToken> pendingAudioActivation;
    std::optional<StartupToken> scanToken;
    StartupDeviceSummary deviceSummary;
    StartupPluginSummary pluginSummary;
    std::unique_ptr<juce::FileChooser> exportChooser;
    double constructedMs = juce::Time::getMillisecondCounterHiRes();
    double firstPaintMs = -1.0;
    double lastPersistMs = 0.0;
    std::uint64_t displayedRevision = 0;
    std::uint64_t persistedRevision = 0;
    bool audioProbeInFlight = false;
    bool ioDiscoveryInFlight = false;
    bool nativeAudioActivationInFlight = false;
    bool withoutAudioAfterActivation = false;
    bool safeStartAfterActivation = false;
    bool shutdownAfterActivation = false;
    bool invalidAudioSettings = false;
    bool scanOnStartup = true;
    int devicePollTicks = 0;
    bool workspaceEntered = false;
    StartupReadiness workspaceReadiness = StartupReadiness::loading;
    bool historyOpen = false;
    bool persistInFlight = false;
    bool diagnosticsDisabled = false;
    bool stopped = false;
    bool selfTestFinished = false;
    int responsiveWorkerTicks = 0;
};
}
