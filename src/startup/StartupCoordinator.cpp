#include "StartupCoordinator.h"

#include <algorithm>
#include <cmath>

namespace studio
{
juce::String startupTaskId(StartupTaskId id)
{
    switch (id)
    {
        case StartupTaskId::welcome: return "startup.welcome";
        case StartupTaskId::preferences: return "startup.preferences";
        case StartupTaskId::recovery: return "startup.recovery";
        case StartupTaskId::pluginCatalog: return "startup.plugin-catalog";
        case StartupTaskId::audio: return "startup.audio";
        case StartupTaskId::midi: return "startup.midi";
        case StartupTaskId::workspace: return "startup.workspace";
        case StartupTaskId::project: return "startup.project";
        case StartupTaskId::pluginRescan: return "startup.plugin-rescan";
        case StartupTaskId::diagnostics: return "startup.diagnostics";
    }
    jassertfalse;
    return "startup.invalid";
}

juce::String startupStateName(StartupState state)
{
    switch (state)
    {
        case StartupState::pending: return "pending";
        case StartupState::running: return "running";
        case StartupState::succeeded: return "succeeded";
        case StartupState::warning: return "warning";
        case StartupState::failed: return "failed";
        case StartupState::blocked: return "blocked";
        case StartupState::cancelled: return "cancelled";
        case StartupState::timedOut: return "timed-out";
        case StartupState::skipped: return "skipped";
    }
    jassertfalse;
    return "invalid";
}

juce::String startupIssueCode(StartupIssue issue)
{
    switch (issue)
    {
        case StartupIssue::none: return "none";
        case StartupIssue::preferencesInvalid: return "preferences-invalid";
        case StartupIssue::catalogInvalid: return "catalog-invalid";
        case StartupIssue::recoveryRequired: return "unclean-session";
        case StartupIssue::audioUnavailable: return "audio-unavailable";
        case StartupIssue::midiUnavailable: return "midi-unavailable";
        case StartupIssue::projectUnavailable: return "project-unavailable";
        case StartupIssue::pluginWorkerCrash: return "plugin-worker-crash";
        case StartupIssue::pluginWorkerTimeout: return "plugin-worker-timeout";
        case StartupIssue::pluginWorkerProtocol: return "plugin-worker-protocol";
        case StartupIssue::pluginDiscoveryFailure: return "plugin-discovery-failure";
        case StartupIssue::pluginValidationFailure: return "plugin-validation-failure";
        case StartupIssue::storageUnavailable: return "storage-unavailable";
        case StartupIssue::safeAudio: return "audio-disabled-by-user";
        case StartupIssue::safePlugins: return "third-party-runtimes-disabled";
        case StartupIssue::scanDisabled: return "background-scan-disabled";
        case StartupIssue::cancelled: return "cancelled";
        case StartupIssue::timeout: return "deadline-exceeded";
    }
    jassertfalse;
    return "invalid";
}

bool startupStateIsTerminal(StartupState state) noexcept
{
    return state != StartupState::pending && state != StartupState::running;
}

bool startupStateAllowsDependents(StartupState state) noexcept
{
    return state == StartupState::succeeded || state == StartupState::warning
        || state == StartupState::skipped;
}

StartupCoordinator::StartupCoordinator(std::vector<StartupTask> tasks)
{
    for (auto& task : tasks)
    {
        StartupTaskResult result;
        result.task = std::move(task);
        taskResults.push_back(std::move(result));
    }
}

juce::Result StartupCoordinator::begin(double nowMs, juce::Time wallTime)
{
    if (started || taskResults.empty() || !std::isfinite(nowMs))
        return juce::Result::fail("Startup needs a non-empty graph and can begin only once.");
    std::vector<StartupTaskId> sorted;
    for (const auto& item : taskResults)
    {
        if (!std::isfinite(item.task.weight) || item.task.weight <= 0.0
            || !std::isfinite(item.task.timeoutMs) || item.task.timeoutMs <= 0.0
            || item.task.maximumAttempts < 1)
            return juce::Result::fail("Startup task weights, deadlines and attempt limits must be positive.");
        if (std::count_if(taskResults.begin(), taskResults.end(), [&item](const auto& other)
                         { return other.task.id == item.task.id; }) != 1)
            return juce::Result::fail("Startup task IDs must be unique.");
        for (const auto dependency : item.task.dependencies)
            if (result(dependency) == nullptr)
                return juce::Result::fail("A startup prerequisite is missing.");
    }
    while (sorted.size() < taskResults.size())
    {
        const auto previousSize = sorted.size();
        for (const auto& item : taskResults)
            if (std::find(sorted.begin(), sorted.end(), item.task.id) == sorted.end()
                && std::all_of(item.task.dependencies.begin(), item.task.dependencies.end(),
                               [&sorted](auto id)
                               { return std::find(sorted.begin(), sorted.end(), id) != sorted.end(); }))
                sorted.push_back(item.task.id);
        if (sorted.size() == previousSize)
            return juce::Result::fail("Startup prerequisites contain a cycle.");
    }
    started = true;
    startMs = nowMs;
    wallStart = wallTime;
    ++stateRevision;
    return juce::Result::ok();
}

std::vector<StartupToken> StartupCoordinator::takeRunnableTasks(double nowMs)
{
    std::vector<StartupToken> runnable;
    if (!started || cancelled)
        return runnable;
    expire(nowMs);
    for (auto& item : taskResults)
    {
        if (item.state != StartupState::pending)
            continue;
        const auto failedDependency = std::any_of(
            item.task.dependencies.begin(), item.task.dependencies.end(), [this](auto id)
            {
                const auto state = result(id)->state;
                return startupStateIsTerminal(state) && !startupStateAllowsDependents(state);
            });
        if (failedDependency)
        {
            finish(item, { StartupState::blocked, StartupIssue::none,
                           "Waiting for a prerequisite to be retried or explicitly bypassed." }, nowMs);
            continue;
        }
        if (!std::all_of(item.task.dependencies.begin(), item.task.dependencies.end(),
                         [this](auto id) { return startupStateAllowsDependents(result(id)->state); }))
            continue;
        item.state = StartupState::running;
        item.startedMs = nowMs;
        item.startedAt = timestamp(nowMs);
        ++item.attempt;
        record(item, nowMs);
        runnable.push_back({ item.task.id, item.attempt });
    }
    return runnable;
}

bool StartupCoordinator::complete(StartupToken token, StartupOutcome outcome, double nowMs)
{
    if (!accepts(token) || !std::isfinite(nowMs))
        return false;
    auto& item = *find(token.id);
    if (nowMs - item.startedMs >= item.task.timeoutMs)
    {
        finish(item, { StartupState::timedOut, StartupIssue::timeout,
                       "This step exceeded its deadline. Retry or use the recovery actions." }, nowMs);
        return false;
    }
    if (outcome.state != StartupState::succeeded && outcome.state != StartupState::warning
        && outcome.state != StartupState::failed && outcome.state != StartupState::cancelled
        && outcome.state != StartupState::timedOut)
        return false;
    finish(item, std::move(outcome), nowMs);
    return true;
}

bool StartupCoordinator::updateProgress(StartupToken token, double value, juce::String detail)
{
    if (!accepts(token) || !std::isfinite(value) || value < 0.0 || value > 1.0)
        return false;
    auto& item = *find(token.id);
    item.progress = juce::jmax(item.progress, value);
    if (detail.isNotEmpty())
        item.detail = std::move(detail);
    ++stateRevision;
    return true;
}

juce::Result StartupCoordinator::retry(StartupTaskId id)
{
    auto* item = find(id);
    if (item == nullptr || cancelled || !startupStateIsTerminal(item->state)
        || startupStateAllowsDependents(item->state)
        || item->attempt >= item->task.maximumAttempts)
        return juce::Result::fail("This startup step cannot be retried, or its retry limit was reached.");
    item->state = StartupState::pending;
    item->issue = StartupIssue::none;
    item->detail.clear();
    item->progress = 0.0;
    item->durationMs = 0.0;
    item->startedAt = {};
    item->endedAt = {};
    for (auto& dependent : taskResults)
        if (dependent.state == StartupState::blocked)
            dependent.state = StartupState::pending;
    ++stateRevision;
    return juce::Result::ok();
}

juce::Result StartupCoordinator::skip(
    StartupTaskId id, StartupIssue issue, juce::String detail, double nowMs)
{
    auto* item = find(id);
    if (!started || cancelled || item == nullptr
        || (item->task.required && !item->task.allowSkip))
        return juce::Result::fail("This required startup step cannot be bypassed.");
    finish(*item, { StartupState::skipped, issue, std::move(detail) }, nowMs);
    for (auto& dependent : taskResults)
        if (dependent.state == StartupState::blocked)
            dependent.state = StartupState::pending;
    return juce::Result::ok();
}

juce::Result StartupCoordinator::resolve(StartupTaskId id, StartupOutcome outcome, double nowMs)
{
    auto* item = find(id);
    if (item == nullptr || !started || cancelled || !startupStateIsTerminal(item->state)
        || (outcome.state != StartupState::succeeded && outcome.state != StartupState::warning))
        return juce::Result::fail("Only an explicitly recovered startup result can be resolved.");
    finish(*item, std::move(outcome), nowMs);
    for (auto& dependent : taskResults)
        if (dependent.state == StartupState::blocked)
            dependent.state = StartupState::pending;
    return juce::Result::ok();
}

void StartupCoordinator::cancel(double nowMs)
{
    if (!started)
        return;
    cancelled = true;
    for (auto& item : taskResults)
        if (!startupStateIsTerminal(item.state) || item.state == StartupState::blocked)
            finish(item, { StartupState::cancelled, StartupIssue::cancelled,
                           "Startup was cancelled." }, nowMs);
    ++stateRevision;
}

void StartupCoordinator::expire(double nowMs)
{
    if (!std::isfinite(nowMs))
        return;
    for (auto& item : taskResults)
        if (item.state == StartupState::running && nowMs - item.startedMs >= item.task.timeoutMs)
            finish(item, { StartupState::timedOut, StartupIssue::timeout,
                           "This step exceeded its deadline. Retry or use the recovery actions." }, nowMs);
}

bool StartupCoordinator::accepts(StartupToken token) const noexcept
{
    const auto* item = result(token.id);
    return !cancelled && item != nullptr && item->state == StartupState::running
        && item->attempt == token.attempt;
}

StartupReadiness StartupCoordinator::readiness() const noexcept
{
    if (cancelled)
        return StartupReadiness::cancelled;
    if (!started)
        return StartupReadiness::loading;
    auto loading = false;
    auto degraded = false;
    for (const auto& item : taskResults)
    {
        if (item.task.required && startupStateIsTerminal(item.state)
            && !startupStateAllowsDependents(item.state))
            return StartupReadiness::blocked;
        if (item.task.required && !startupStateIsTerminal(item.state))
            loading = true;
        if (item.state == StartupState::warning
            || (item.state == StartupState::skipped && (item.task.required
                || (item.issue != StartupIssue::none && item.issue != StartupIssue::scanDisabled)))
            || (!item.task.required && startupStateIsTerminal(item.state)
                && !startupStateAllowsDependents(item.state)))
            degraded = true;
    }
    if (loading)
        return StartupReadiness::loading;
    return degraded ? StartupReadiness::degraded : StartupReadiness::ready;
}

double StartupCoordinator::progress() const noexcept
{
    auto weight = 0.0;
    auto completeWeight = 0.0;
    for (const auto& item : taskResults)
        if (item.task.required)
        {
            weight += item.task.weight;
            completeWeight += item.task.weight
                * (startupStateIsTerminal(item.state) ? 1.0 : item.progress);
        }
    return weight > 0.0 ? completeWeight / weight : 0.0;
}

const std::vector<StartupTaskResult>& StartupCoordinator::results() const noexcept { return taskResults; }
const std::vector<StartupHistoryEntry>& StartupCoordinator::history() const noexcept { return stepHistory; }
std::uint64_t StartupCoordinator::revision() const noexcept { return stateRevision; }

const StartupTaskResult* StartupCoordinator::result(StartupTaskId id) const noexcept
{
    const auto found = std::find_if(taskResults.begin(), taskResults.end(),
                                    [id](const auto& item) { return item.task.id == id; });
    return found != taskResults.end() ? &*found : nullptr;
}

StartupTaskResult* StartupCoordinator::find(StartupTaskId id) noexcept
{
    const auto found = std::find_if(taskResults.begin(), taskResults.end(),
                                    [id](const auto& item) { return item.task.id == id; });
    return found != taskResults.end() ? &*found : nullptr;
}

juce::Time StartupCoordinator::timestamp(double nowMs) const
{
    return juce::Time(wallStart.toMilliseconds()
                      + static_cast<juce::int64>(juce::jmax(0.0, nowMs - startMs)));
}

void StartupCoordinator::finish(StartupTaskResult& item, StartupOutcome outcome, double nowMs)
{
    item.durationMs = item.attempt > 0
        ? juce::jmax(0.0, nowMs - item.startedMs) : 0.0;
    item.endedAt = timestamp(nowMs);
    item.progress = 1.0;
    item.state = outcome.state;
    item.issue = outcome.issue;
    item.detail = std::move(outcome.detail);
    record(item, nowMs);
}

void StartupCoordinator::record(StartupTaskResult& item, double nowMs)
{
    constexpr std::size_t maximumHistory = 128;
    if (stepHistory.size() >= maximumHistory)
        stepHistory.erase(stepHistory.begin());
    stepHistory.push_back({ item.task.id, item.state, item.issue, item.attempt,
                            timestamp(nowMs), item.durationMs, item.detail });
    ++stateRevision;
    juce::Logger::writeToLog("[" + startupTaskId(item.task.id) + "] "
        + startupStateName(item.state) + " " + startupIssueCode(item.issue)
        + " (" + juce::String(item.durationMs, 1) + " ms)");
}
}
