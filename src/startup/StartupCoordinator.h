#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace studio
{
enum class StartupTaskId
{
    welcome,
    preferences,
    recovery,
    pluginCatalog,
    audio,
    midi,
    workspace,
    project,
    pluginRescan,
    diagnostics
};

enum class StartupState
{
    pending,
    running,
    succeeded,
    warning,
    failed,
    blocked,
    cancelled,
    timedOut,
    skipped
};

enum class StartupIssue
{
    none,
    preferencesInvalid,
    catalogInvalid,
    recoveryRequired,
    audioUnavailable,
    midiUnavailable,
    projectUnavailable,
    pluginWorkerCrash,
    pluginWorkerTimeout,
    pluginWorkerProtocol,
    pluginDiscoveryFailure,
    pluginValidationFailure,
    storageUnavailable,
    safeAudio,
    safePlugins,
    scanDisabled,
    cancelled,
    timeout
};

enum class StartupReadiness { loading, ready, degraded, blocked, cancelled };

struct StartupTask
{
    StartupTaskId id;
    juce::String name;
    std::vector<StartupTaskId> dependencies;
    bool required = true;
    bool allowSkip = false;
    double weight = 1.0;
    double timeoutMs = 10000.0;
    int maximumAttempts = 1;
};

struct StartupToken
{
    StartupTaskId id;
    int attempt = 0;
    bool operator==(const StartupToken&) const = default;
};

struct StartupOutcome
{
    StartupState state = StartupState::succeeded;
    StartupIssue issue = StartupIssue::none;
    juce::String detail;
};

struct StartupTaskResult
{
    StartupTask task;
    StartupState state = StartupState::pending;
    StartupIssue issue = StartupIssue::none;
    juce::String detail;
    int attempt = 0;
    juce::Time startedAt;
    juce::Time endedAt;
    double startedMs = 0.0;
    double durationMs = 0.0;
    double progress = 0.0;
};

struct StartupHistoryEntry
{
    StartupTaskId id;
    StartupState state;
    StartupIssue issue;
    int attempt;
    juce::Time timestamp;
    double durationMs;
    juce::String detail;
};

[[nodiscard]] juce::String startupTaskId(StartupTaskId id);
[[nodiscard]] juce::String startupStateName(StartupState state);
[[nodiscard]] juce::String startupIssueCode(StartupIssue issue);
[[nodiscard]] bool startupStateIsTerminal(StartupState state) noexcept;
[[nodiscard]] bool startupStateAllowsDependents(StartupState state) noexcept;

class StartupCoordinator
{
public:
    explicit StartupCoordinator(std::vector<StartupTask> tasks);

    juce::Result begin(double nowMs, juce::Time wallTime = juce::Time::getCurrentTime());
    [[nodiscard]] std::vector<StartupToken> takeRunnableTasks(double nowMs);
    bool complete(StartupToken token, StartupOutcome outcome, double nowMs);
    bool updateProgress(StartupToken token, double value, juce::String detail = {});
    juce::Result retry(StartupTaskId id);
    juce::Result skip(StartupTaskId id, StartupIssue issue, juce::String detail, double nowMs);
    juce::Result resolve(StartupTaskId id, StartupOutcome outcome, double nowMs);
    void cancel(double nowMs);
    void expire(double nowMs);

    [[nodiscard]] bool accepts(StartupToken token) const noexcept;
    [[nodiscard]] StartupReadiness readiness() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] const std::vector<StartupTaskResult>& results() const noexcept;
    [[nodiscard]] const std::vector<StartupHistoryEntry>& history() const noexcept;
    [[nodiscard]] const StartupTaskResult* result(StartupTaskId id) const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

private:
    StartupTaskResult* find(StartupTaskId id) noexcept;
    void record(StartupTaskResult& result, double nowMs);
    void finish(StartupTaskResult& result, StartupOutcome outcome, double nowMs);
    [[nodiscard]] juce::Time timestamp(double nowMs) const;

    std::vector<StartupTaskResult> taskResults;
    std::vector<StartupHistoryEntry> stepHistory;
    juce::Time wallStart;
    double startMs = 0.0;
    bool started = false;
    bool cancelled = false;
    std::uint64_t stateRevision = 0;
};
}
