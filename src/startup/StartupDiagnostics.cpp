#include "StartupDiagnostics.h"
#include "platform/Architecture.h"

#include <algorithm>
#include <memory>

namespace studio
{
namespace
{
constexpr auto maximumReportBytes = 128 * 1024;

juce::String readinessName(StartupReadiness value)
{
    switch (value)
    {
        case StartupReadiness::loading: return "loading";
        case StartupReadiness::ready: return "ready";
        case StartupReadiness::degraded: return "degraded";
        case StartupReadiness::blocked: return "blocked";
        case StartupReadiness::cancelled: return "cancelled";
    }
    jassertfalse;
    return "invalid";
}

juce::var buildDetails()
{
    auto build = std::make_unique<juce::DynamicObject>();
    build->setProperty("application", "Studio Duo");
    build->setProperty("version", STUDIO_DUO_VERSION);
#if defined(STUDIO_DUO_BUILD_REVISION)
    build->setProperty("revision", STUDIO_DUO_BUILD_REVISION);
#endif
#if defined(STUDIO_DUO_BUILD_CONFIG)
    build->setProperty("configuration", STUDIO_DUO_BUILD_CONFIG);
#endif
#if JUCE_MAC
    build->setProperty("platform", "macOS");
#elif JUCE_WINDOWS
    build->setProperty("platform", "Windows");
#else
    build->setProperty("platform", "unsupported");
#endif
    build->setProperty("osVersion", juce::SystemStats::getOperatingSystemName());
    build->setProperty("architecture", currentArchitecture());
    return juce::var(build.release());
}

juce::var resultValue(
    StartupTaskId id, StartupState state, StartupIssue issue,
    int attempt, juce::Time at, double durationMs)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("category", startupTaskId(id));
    object->setProperty("state", startupStateName(state));
    object->setProperty("issue", startupIssueCode(issue));
    object->setProperty("attempt", attempt);
    object->setProperty("timestamp", at.toMilliseconds() != 0 ? at.toISO8601(true) : juce::String());
    object->setProperty("durationMs", durationMs);
    return juce::var(object.release());
}
}

juce::var startupDiagnosticReport(
    const StartupCoordinator& startup, const StartupDeviceSummary& devices,
    const StartupPluginSummary& plugins, bool safePlugins, bool defaultAudio,
    double firstPaintMs)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schemaVersion", 1);
    root->setProperty("build", buildDetails());
    root->setProperty("readiness", readinessName(startup.readiness()));
    root->setProperty("requiredProgress", startup.progress());
    root->setProperty("firstPaintMs", firstPaintMs);
    root->setProperty("safePlugins", safePlugins);
    root->setProperty("defaultAudio", defaultAudio);
    root->setProperty("privacy", "No paths, names, device identifiers, plug-in state or audio content.");
    juce::Array<juce::var> results;
    for (const auto& item : startup.results())
    {
        auto value = resultValue(item.task.id, item.state, item.issue, item.attempt,
                                  item.endedAt, item.durationMs);
        auto* object = value.getDynamicObject();
        object->setProperty("required", item.task.required);
        object->setProperty("startedAt", item.startedAt.toMilliseconds() != 0
            ? item.startedAt.toISO8601(true) : juce::String());
        object->setProperty("progress", item.progress);
        results.add(std::move(value));
    }
    root->setProperty("results", juce::var(results));
    juce::Array<juce::var> history;
    for (const auto& item : startup.history())
        history.add(resultValue(item.id, item.state, item.issue, item.attempt,
                                 item.timestamp, item.durationMs));
    root->setProperty("history", juce::var(history));
    auto deviceValue = std::make_unique<juce::DynamicObject>();
    deviceValue->setProperty("audioEnabled", devices.audioEnabled);
    deviceValue->setProperty("midiAvailable", devices.midiAvailable);
    deviceValue->setProperty("sampleRate", devices.sampleRate);
    deviceValue->setProperty("bufferSize", devices.bufferSize);
    deviceValue->setProperty("audioInputs", devices.audioInputs);
    deviceValue->setProperty("audioOutputs", devices.audioOutputs);
    deviceValue->setProperty("enabledMidiInputs", devices.enabledMidiInputs);
    deviceValue->setProperty("enabledMidiOutputs", devices.enabledMidiOutputs);
    deviceValue->setProperty("unavailableMidiDevices", devices.unavailableMidiDevices);
    deviceValue->setProperty("retrospectiveCapture", devices.retrospectiveCapture);
    root->setProperty("devices", juce::var(deviceValue.release()));
    auto pluginValue = std::make_unique<juce::DynamicObject>();
    pluginValue->setProperty("ready", plugins.ready);
    pluginValue->setProperty("blocked", plugins.blocked);
    pluginValue->setProperty("bundled", plugins.bundled);
    pluginValue->setProperty("failedValidations", plugins.failedValidations);
    pluginValue->setProperty("scanCrashes", plugins.scanCrashes);
    pluginValue->setProperty("runtimeCrashes", plugins.runtimeCrashes);
    pluginValue->setProperty("timeouts", plugins.timeouts);
    constexpr const char* failureNames[] {
        "none", "scanCrash", "runtimeCrash", "timeout", "protocolMismatch",
        "unsupportedLayout", "corruptState", "missing"
    };
    auto failureValue = std::make_unique<juce::DynamicObject>();
    for (std::size_t index = 1; index < plugins.failures.size(); ++index)
        failureValue->setProperty(failureNames[index], plugins.failures[index]);
    pluginValue->setProperty("failures", juce::var(failureValue.release()));
    root->setProperty("plugins", juce::var(pluginValue.release()));
    return juce::var(root.release());
}

juce::Result writeStartupDiagnostic(const juce::File& destination, const juce::var& report)
{
    const auto text = juce::JSON::toString(report, true);
    if (text.getNumBytesAsUTF8() > maximumReportBytes)
        return juce::Result::fail("The startup report exceeds its 128 KiB limit.");
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create the startup diagnostics directory.");
    const auto temporary = destination.getSiblingFile(destination.getFileName() + ".tmp-" + juce::Uuid().toString());
    if (!temporary.replaceWithText(text, false, false, "\n"))
        return juce::Result::fail("Could not write the startup diagnostic report.");
    const auto published = destination.existsAsFile()
        ? temporary.replaceFileIn(destination) : temporary.moveFileTo(destination);
    if (!published)
    {
        if (!temporary.deleteFile())
            juce::Logger::writeToLog("[startup.diagnostics] Could not remove an unpublished temporary report.");
        return juce::Result::fail("Could not publish the startup diagnostic report.");
    }
    return juce::Result::ok();
}

StartupDiagnosticStore::StartupDiagnosticStore(juce::File folder)
    : directory(std::move(folder)),
      currentReport(directory.getChildFile("startup-" + juce::Uuid().toString() + ".json"))
{
}

juce::Result StartupDiagnosticStore::persist(const juce::var& report)
{
    if (const auto written = writeStartupDiagnostic(currentReport, report); written.failed())
        return written;
    auto files = directory.findChildFiles(juce::File::findFiles, false, "startup-*.json");
    std::sort(files.begin(), files.end(), [this](const auto& left, const auto& right)
    {
        if (left == currentReport || right == currentReport)
            return left == currentReport && right != currentReport;
        if (left.getLastModificationTime() == right.getLastModificationTime())
            return left.getFileName() < right.getFileName();
        return left.getLastModificationTime() > right.getLastModificationTime();
    });
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::days(14.0);
    auto retained = 0;
    for (const auto& file : files)
    {
        ++retained;
        if (file != currentReport && (retained > 10 || file.getLastModificationTime() < cutoff))
            if (!file.deleteFile())
                return juce::Result::fail("Could not remove an expired startup report.");
    }
    return juce::Result::ok();
}

juce::File StartupDiagnosticStore::reportFile() const { return currentReport; }
}
