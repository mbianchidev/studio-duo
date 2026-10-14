#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "platform/Architecture.h"
#include "project_io/ProjectFile.h"
#include "ui/TimelineComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace
{
struct ProcessStatistics
{
    std::uint64_t peakMemoryBytes = 0;
    double cpuMilliseconds = 0.0;
};

juce::Result processStatistics(ProcessStatistics& statistics)
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS memory {};
    constexpr auto memorySize = static_cast<DWORD>(sizeof(memory));
    memory.cb = memorySize;
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, memorySize)
        || !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return juce::Result::fail("Could not read release-fixture process statistics: "
                                  + juce::String(GetLastError()));
    const auto ticks = [](const FILETIME& time)
    {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    statistics.peakMemoryBytes = memory.PeakWorkingSetSize;
    statistics.cpuMilliseconds = static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
    rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return juce::Result::fail("Could not read release-fixture process statistics.");
    statistics.peakMemoryBytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#if !JUCE_MAC
    statistics.peakMemoryBytes *= 1024;
#endif
    statistics.cpuMilliseconds = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0
        + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
#endif
    return juce::Result::ok();
}

studio::Project largeProject(const juce::File& source)
{
    studio::Project project;
    project.id = "synthetic-large-project";
    project.name = "Synthetic 128-track release fixture";
    project.metronomeEnabled = false;
    project.tempo = 137.0;
    project.tempoChanges = { { 0.0, 137.0, true }, { 0.5, 193.0, false } };
    project.markers = { { "synthetic-long-session", "Thirty-minute timeline", 1800.0 } };
    project.sections = { { "synthetic-long-section", "Thirty-minute session range", 0.0 } };
    project.sections.front().endTimeSeconds = 1800.0;
    for (int index = 0; index < 128; ++index)
    {
        studio::Track track;
        track.id = "synthetic-track-" + juce::String(index);
        track.name = "Synthetic track " + juce::String(index + 1);
        track.volumeDecibels = -48.0f;
        track.outputTrackId = "synthetic-bus-" + juce::String(index / 16);
        for (int clipIndex = 0; clipIndex < 16; ++clipIndex)
        {
            studio::AudioClip clip;
            clip.id = track.id + "-clip-" + juce::String(clipIndex);
            clip.sourceFile = source;
            clip.startSeconds = static_cast<double>(clipIndex) / 16.0;
            clip.durationSeconds = 1.0 / 16.0;
            clip.sourceLengthSeconds = 0.125;
            clip.sourceRangeEndSeconds = 0.125;
            track.clips.push_back(std::move(clip));
        }
        studio::AutomationLane lane;
        lane.id = track.id + "-automation";
        lane.name = "Synthetic volume";
        lane.target.trackId = track.id;
        for (int point = 0; point < 64; ++point)
            lane.points.push_back({ lane.id + "-point-" + juce::String(point),
                                    static_cast<double>(point) / 64.0,
                                    point % 2 == 0 ? 0.1 : 0.2 });
        project.automationLanes.push_back(std::move(lane));
        project.routingConnections.push_back(
            release_fixture::mainRoute(track.id + "-route", track.id, track.outputTrackId));
        project.tracks.push_back(std::move(track));
    }
    for (int index = 0; index < 8; ++index)
    {
        studio::Track bus;
        bus.id = "synthetic-bus-" + juce::String(index);
        bus.name = "Synthetic bus";
        bus.type = studio::TrackType::bus;
        bus.outputTrackId = "synthetic-master";
        project.routingConnections.push_back(
            release_fixture::mainRoute(bus.id + "-route", bus.id, bus.outputTrackId));
        project.tracks.push_back(std::move(bus));
    }
    studio::Track master;
    master.id = "synthetic-master";
    master.type = studio::TrackType::master;
    project.tracks.push_back(std::move(master));
    return project;
}

bool waitForGraph(studio::StudioAudioEngine& engine)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (engine.pluginRuntimeTransitionPending())
    {
        if (juce::Time::getMillisecondCounterHiRes() >= deadline)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    return true;
}
}

void performanceHardeningTests()
{
    const auto initialFailures = failures;
    release_fixture::Directory directory;
    const auto source = directory.file.getChildFile("synthetic.wav");
    const auto mediaResult = release_fixture::writeMedia(source);
    expect(mediaResult.wasOk(), mediaResult.getErrorMessage().toRawUTF8());
    if (mediaResult.failed())
        return;
    auto project = largeProject(source);
    const auto package = directory.file.getChildFile("large.studioduo");
    const auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };
    auto started = now();
    const auto saved = studio::ProjectFile::save(project, package);
    const auto saveMs = now() - started;
    expect(saved.wasOk() && saveMs < 10000.0,
           ("The complete 128-track package saves within 10 seconds: "
            + saved.getErrorMessage()).toRawUTF8());
    started = now();
    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    const auto loadMs = now() - started;
    expect(loaded && release_fixture::snapshot(*loaded) == release_fixture::snapshot(project) && loadMs < 10000.0,
           ("The complete large-session package opens without data loss within 10 seconds: " + error).toRawUTF8());
    juce::int64 diskBytes = 0;
    for (const auto& file : package.findChildFiles(juce::File::findFiles, true))
        diskBytes += file.getSize();
    expect(diskBytes > 0 && diskBytes < 32 * 1024 * 1024,
           "The synthetic large-session save uses less than 32 MiB on disk.");

    auto engine = std::make_unique<studio::StudioAudioEngine>();
    started = now();
    const auto published = engine->updateProject(project);
    const auto graphReady = published.wasOk() && waitForGraph(*engine);
    const auto graphMs = now() - started;
    expect(graphReady && graphMs < 5000.0,
           ("The large routing/automation graph publishes within 5 seconds: "
            + published.getErrorMessage()).toRawUTF8());
    expect(engine->sourceReadCountForTesting() == 1,
           "A graph update decodes shared media once rather than once per edited clip.");
    if (!graphReady)
        return;
    ProcessStatistics before;
    if (const auto result = processStatistics(before); result.failed())
    {
        expect(false, result.getErrorMessage().toRawUTF8());
        return;
    }
    engine->seekSeconds(0.0);
    engine->play();
    constexpr int blockSize = 256;
    constexpr int blocks = 128;
    juce::AudioBuffer<float> live(2, blockSize * blocks);
    double maximumCallbackMs = 0.0;
    started = now();
    for (int block = 0; block < blocks; ++block)
    {
        const auto blockStart = now();
        const auto rendered = engine->renderActiveBlockForTesting(blockSize);
        maximumCallbackMs = std::max(maximumCallbackMs, now() - blockStart);
        for (int channel = 0; channel < 2; ++channel)
            live.copyFrom(channel, block * blockSize, rendered, channel, 0, blockSize);
    }
    const auto callbackBatchMs = now() - started;
    engine->stop();
    ProcessStatistics after;
    if (const auto result = processStatistics(after); result.failed())
    {
        expect(false, result.getErrorMessage().toRawUTF8());
        return;
    }
    const auto callbackCpuMs = after.cpuMilliseconds - before.cpuMilliseconds;
    expect(callbackBatchMs < 10000.0 && maximumCallbackMs < 100.0 && callbackCpuMs < 5000.0,
           "Large-session callback work stays within the documented hosted-runner regression budgets.");

    juce::AudioBuffer<float> offline;
    started = now();
    const auto rendered = engine->renderRangeToBuffer(
        project, offline, 48000.0, { 0.0, static_cast<double>(live.getNumSamples()) / 48000.0, 0.0 });
    const auto renderMs = now() - started;
    auto nullError = 0.0f;
    const auto matchingShape = offline.getNumSamples() == live.getNumSamples() && offline.getNumChannels() == 2;
    if (rendered.wasOk() && matchingShape)
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < live.getNumSamples(); ++sample)
                nullError = std::max(nullError, std::abs(live.getSample(channel, sample) - offline.getSample(channel, sample)));
    expect(rendered.wasOk() && matchingShape && renderMs < 5000.0
               && live.getMagnitude(0, live.getNumSamples()) > 0.00001f
               && live.getMagnitude(0, live.getNumSamples()) < 0.5f
               && std::isfinite(nullError) && nullError < 0.00001f,
           "Large-session live and offline renders are audible, sample-aligned, and null below 1e-5.");

    studio::TimelineComponent timeline;
    timeline.setProject(&project);
    timeline.setBounds(0, 0, timeline.preferredWidth(1024), timeline.preferredHeight(600));
    expect(timeline.getWidth() > 173000,
           "The performance viewport belongs to a real thirty-minute arrangement extent.");
    double maximumPaintMs = 0.0;
    for (const auto viewX : { 0, 86000, 172000 })
    {
        constexpr int viewY = 80 + 96 * 88;
        timeline.setViewportPosition(viewX);
        juce::Image image(juce::Image::ARGB, 1024, 600, true, juce::SoftwareImageType {});
        juce::Graphics graphics(image);
        graphics.addTransform(juce::AffineTransform::translation(
            -static_cast<float>(viewX), -static_cast<float>(viewY)));
        const auto paintBounds = graphics.getClipBounds();
        started = now();
        timeline.paint(graphics);
        maximumPaintMs = std::max(maximumPaintMs, now() - started);
        const auto statistics = timeline.paintStatisticsForTesting();
        expect(statistics.tracksPainted <= 8 && statistics.clipsPainted <= 128
                   && statistics.gridLinesConsidered <= 128,
               "Painting a late timeline viewport visits only its visible rows, clips, and beat grid.");
        expect(image.getPixelAt(16, 40).getARGB() == 0xff15181b,
               ("The late viewport paints its actual track-header surface (view "
                + juce::String(viewX) + ", clip " + paintBounds.toString()
                + ", pixel " + juce::String::toHexString(image.getPixelAt(16, 40).getARGB()) + ").").toRawUTF8());
    }
    expect(maximumPaintMs < 500.0, "Each large-session viewport paints within 500 ms.");
    const auto readsBeforeRefresh = engine->sourceReadCountForTesting();
    started = now();
    project.tracks.front().volumeDecibels = -42.0f;
    const auto refreshed = engine->updateProject(project);
    const auto refreshReady = refreshed.wasOk() && waitForGraph(*engine);
    const auto refreshMs = now() - started;
    expect(refreshReady && refreshMs < 5000.0
               && engine->sourceReadCountForTesting() == readsBeforeRefresh + 1,
           "A mixer edit republishes the graph with one fresh shared-source read.");
    engine->seekSeconds(0.0);
    engine->play();
    const auto beforeMediaChange = engine->renderActiveBlockForTesting(blockSize);
    engine->stop();
    const auto originalSize = source.getSize();
    const auto originalTime = source.getLastModificationTime();
    expect(source.deleteFile() && release_fixture::writeMedia(source, 0.0625f).wasOk()
               && source.setLastModificationTime(originalTime) && source.getSize() == originalSize,
           "A same-size, same-timestamp media replacement can be written.");
    expect(engine->updateProject(project).wasOk() && waitForGraph(*engine),
           "A graph refresh rereads changed media rather than relying on stale file metadata.");
    engine->seekSeconds(0.0);
    engine->play();
    const auto afterMediaChange = engine->renderActiveBlockForTesting(blockSize);
    engine->stop();
    float refreshError = 0.0f;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
            refreshError = std::max(refreshError,
                                    std::abs(afterMediaChange.getSample(channel, sample)
                                             - beforeMediaChange.getSample(channel, sample) * 0.5f));
    expect(beforeMediaChange.getMagnitude(0, blockSize) > 0.00001f && refreshError < 0.000001f,
           "A changed media source becomes audible immediately after graph publication.");
    if (const auto result = processStatistics(after); result.failed())
    {
        expect(false, result.getErrorMessage().toRawUTF8());
        return;
    }
    expect(after.peakMemoryBytes > 0 && after.peakMemoryBytes < 1024ULL * 1024ULL * 1024ULL,
           "The complete synthetic regression fixture stays below a 1 GiB peak working set.");

    juce::DynamicObject::Ptr report = new juce::DynamicObject();
    report->setProperty("schemaVersion", 1);
    report->setProperty("applicationVersion", STUDIO_DUO_VERSION);
    report->setProperty("projectFormatVersion", studio::Project::currentFormatVersion);
    report->setProperty("architecture", studio::currentArchitecture());
#if JUCE_MAC
    report->setProperty("platform", "macos");
#elif JUCE_WINDOWS
    report->setProperty("platform", "windows");
#else
    report->setProperty("platform", "unsupported");
#endif
    report->setProperty("status", failures == initialFailures ? "pass" : "fail");
    report->setProperty("audioTracks", 128);
    report->setProperty("buses", 8);
    report->setProperty("audioClips", 2048);
    report->setProperty("automationPoints", 8192);
    report->setProperty("saveMilliseconds", saveMs);
    report->setProperty("loadMilliseconds", loadMs);
    report->setProperty("graphMilliseconds", graphMs);
    report->setProperty("graphRefreshMilliseconds", refreshMs);
    report->setProperty("callbackBatchMilliseconds", callbackBatchMs);
    report->setProperty("callbackCpuMilliseconds", callbackCpuMs);
    report->setProperty("maximumCallbackMilliseconds", maximumCallbackMs);
    report->setProperty("renderMilliseconds", renderMs);
    report->setProperty("maximumPaintMilliseconds", maximumPaintMs);
    report->setProperty("paintBackend", "software");
    report->setProperty("peakMemoryBytes", static_cast<juce::int64>(after.peakMemoryBytes));
    report->setProperty("packageBytes", diskBytes);
    report->setProperty("maximumNullError", static_cast<double>(nullError));
    const auto reportJson = juce::JSON::toString(juce::var(report.get()), true);
    const auto reportPath = juce::SystemStats::getEnvironmentVariable("STUDIO_DUO_HARDENING_REPORT", {});
    if (reportPath.isNotEmpty())
    {
        const juce::File file(reportPath);
        expect(file.getParentDirectory().createDirectory().wasOk() && file.replaceWithText(reportJson),
               "The synthetic performance evidence report can be written.");
    }
    std::cout << reportJson << '\n';
}
