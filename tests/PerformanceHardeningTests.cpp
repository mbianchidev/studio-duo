#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "platform/Architecture.h"
#include "project_io/ProjectFile.h"
#include "project_io/ProjectCollectionService.h"
#include "ui/TimelineComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <thread>

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
    std::uint64_t readOperations = 0;
    std::uint64_t writeOperations = 0;
};

juce::Result processStatistics(ProcessStatistics& statistics)
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS memory {};
    constexpr auto memorySize = static_cast<DWORD>(sizeof(memory));
    memory.cb = memorySize;
    FILETIME created {}, exited {}, kernel {}, user {};
    IO_COUNTERS io {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, memorySize)
        || !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
        || !GetProcessIoCounters(GetCurrentProcess(), &io))
        return juce::Result::fail("Could not read release-fixture process statistics: "
                                  + juce::String(GetLastError()));
    const auto ticks = [](const FILETIME& time)
    {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    statistics.peakMemoryBytes = memory.PeakWorkingSetSize;
    statistics.cpuMilliseconds = static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    statistics.readOperations = io.ReadOperationCount;
    statistics.writeOperations = io.WriteOperationCount;
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
    statistics.readOperations = static_cast<std::uint64_t>(usage.ru_inblock);
    statistics.writeOperations = static_cast<std::uint64_t>(usage.ru_oublock);
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
    for (;;)
    {
        juce::ignoreUnused(engine.pluginRuntimeStatuses());
        if (!engine.pluginRuntimeTransitionPending())
            return true;
        if (juce::Time::getMillisecondCounterHiRes() >= deadline)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
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

namespace
{
struct LongSessionProfile
{
    int tracks = 16;
    int songs = 3;
    int songSeconds = 180;
    int clipsPerSong = 4;
    int sampleRate = 48000;
    int bufferSize = 256;
    int memoryBudgetMiB = 16384;
    bool paced = false;
};

juce::Result parseLongSessionProfile(const juce::StringArray& arguments,
                                    LongSessionProfile& profile, juce::String& output)
{
    juce::StringArray seen;
    for (const auto& argument : arguments)
    {
        if (argument == "--qualify-long-session")
            continue;
        const auto key = argument.upToFirstOccurrenceOf("=", false, false);
        const auto text = argument.fromFirstOccurrenceOf("=", false, false);
        if (seen.contains(key))
            return juce::Result::fail("Duplicate qualification argument: " + key);
        seen.add(key);
        if (argument == "--paced")
        {
            profile.paced = true;
            continue;
        }
        if (key == "--qualification-output" && text.isNotEmpty())
        {
            output = text;
            continue;
        }
        auto* destination = key == "--tracks" ? &profile.tracks
            : key == "--songs" ? &profile.songs
            : key == "--song-seconds" ? &profile.songSeconds
            : key == "--clips-per-song" ? &profile.clipsPerSong
            : key == "--sample-rate" ? &profile.sampleRate
            : key == "--buffer-size" ? &profile.bufferSize
            : key == "--memory-budget-mib" ? &profile.memoryBudgetMiB : nullptr;
        if (destination == nullptr || text.isEmpty() || text.length() > 9
            || !text.containsOnly("0123456789"))
            return juce::Result::fail("Unknown or invalid qualification argument: " + key);
        *destination = text.getIntValue();
    }
    if (profile.tracks < 1 || profile.tracks > 128 || profile.songs < 2 || profile.songs > 8
        || profile.songSeconds < 1 || profile.songSeconds > 3600
        || profile.clipsPerSong < 1 || profile.clipsPerSong > 64
        || (profile.sampleRate != 48000 && profile.sampleRate != 96000)
        || (profile.bufferSize != 64 && profile.bufferSize != 128 && profile.bufferSize != 256
            && profile.bufferSize != 512 && profile.bufferSize != 1024)
        || profile.memoryBudgetMiB < 256 || profile.memoryBudgetMiB > 65536
        || output.isEmpty() || !juce::File::isAbsolutePath(output))
        return juce::Result::fail(
            "Use 1-128 tracks, 2-8 songs, 1-3600 seconds/song, 1-64 clips/song, "
            "48000/96000 Hz, 64/128/256/512/1024 samples, 256-65536 MiB, and a new absolute output directory.");
    return juce::Result::ok();
}

juce::Result writeLongMedia(const juce::File& file, const LongSessionProfile& profile,
                           int track, int song)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    auto writer = format.createWriterFor(stream,
        juce::AudioFormatWriterOptions {}.withSampleRate(profile.sampleRate)
            .withNumChannels(2).withBitsPerSample(24));
    if (writer == nullptr)
        return juce::Result::fail("Cannot create full-length synthetic media.");
    juce::AudioBuffer<float> block(2, 8192);
    const auto samples = static_cast<juce::int64>(profile.songSeconds) * profile.sampleRate;
    const auto frequency = 83.0 + track * 7.0 + song * 11.0;
    for (juce::int64 offset = 0; offset < samples; offset += block.getNumSamples())
    {
        const auto count = static_cast<int>(std::min<juce::int64>(block.getNumSamples(), samples - offset));
        for (int sample = 0; sample < count; ++sample)
        {
            const auto phase = static_cast<double>(offset + sample)
                * juce::MathConstants<double>::twoPi * frequency / profile.sampleRate;
            block.setSample(0, sample, static_cast<float>(std::sin(phase) * 0.05));
            block.setSample(1, sample, static_cast<float>(std::cos(phase * 1.003) * 0.04));
        }
        if (!writer->writeFromAudioSampleBuffer(block, 0, count))
            return juce::Result::fail("Cannot write full-length synthetic media.");
    }
    return writer->flush() ? juce::Result::ok()
                           : juce::Result::fail("Cannot flush full-length synthetic media.");
}

juce::Result makeLongProject(const LongSessionProfile& profile, const juce::File& directory,
                            studio::Project& project,
                            std::vector<studio::StudioAudioEngine::PluginRuntimeRequest>& requests)
{
    project.id = "synthetic-long-session";
    project.name = "Synthetic full-length multi-song qualification";
    project.metronomeEnabled = false;
    project.tempoChanges = { { 0.0, 137.0, true }, { profile.songSeconds * 0.5, 193.0, false } };
    const auto buses = std::max(1, std::min(8, profile.tracks / 4));
    const auto addProcessor = [&requests](studio::Track& track, const juce::String& identifier)
    {
        studio::PluginInsert insert;
        insert.id = track.id + "-processor";
        insert.pluginIdentifier = identifier;
        insert.name = identifier;
        insert.format = "Studio Duo";
        insert.bundledDevice = true;
        insert.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
        track.inserts.push_back(insert);
        studio::StudioAudioEngine::PluginRuntimeRequest request;
        request.trackId = track.id;
        request.insertId = insert.id;
        request.name = insert.name;
        request.deviceIdentifier = identifier;
        request.bridgeMode = insert.bridgeMode;
        requests.push_back(std::move(request));
    };
    for (int trackIndex = 0; trackIndex < profile.tracks; ++trackIndex)
    {
        studio::Track track;
        track.id = "long-track-" + juce::String(trackIndex);
        track.name = "Synthetic track " + juce::String(trackIndex + 1);
        track.volumeDecibels = -18.0f;
        track.outputTrackId = "long-bus-" + juce::String(trackIndex % buses);
        addProcessor(track, "studio.device.gain");
        studio::AutomationLane lane;
        lane.id = track.id + "-automation";
        lane.target.type = studio::AutomationTargetType::deviceParameter;
        lane.target.trackId = track.id;
        lane.target.insertId = track.inserts.front().id;
        lane.target.parameterId = "gain";
        lane.target.parameterIndex = 0;
        for (int point = 0; point < profile.songs * 64; ++point)
            lane.points.push_back({ lane.id + "-" + juce::String(point),
                static_cast<double>(point) * profile.songSeconds / 64.0,
                point % 2 == 0 ? 0.45 : 0.55 });
        project.automationLanes.push_back(std::move(lane));
        for (int song = 0; song < profile.songs; ++song)
        {
            const auto media = directory.getChildFile(track.id + "-song-" + juce::String(song) + ".wav");
            if (const auto result = writeLongMedia(media, profile, trackIndex, song); result.failed())
                return result;
            for (int edit = 0; edit < profile.clipsPerSong; ++edit)
            {
                studio::AudioClip clip;
                clip.id = track.id + "-song-" + juce::String(song) + "-edit-" + juce::String(edit);
                clip.name = "Synthetic full-length media edit";
                clip.sourceFile = media;
                clip.sourceLengthSeconds = profile.songSeconds;
                clip.sourceRangeEndSeconds = profile.songSeconds;
                clip.durationSeconds = static_cast<double>(profile.songSeconds) / profile.clipsPerSong;
                clip.sourceOffsetSeconds = edit * clip.durationSeconds;
                clip.startSeconds = song * profile.songSeconds + clip.sourceOffsetSeconds;
                track.clips.push_back(std::move(clip));
            }
        }
        studio::Track take;
        take.id = track.id + "-take";
        take.name = "Synthetic alternate take";
        take.parentTrackId = track.id;
        take.versionNumber = 1;
        take.clips.push_back(track.clips.front());
        take.clips.front().id = take.id + "-clip";
        track.versionsCollapsed = true;
        project.routingConnections.push_back(
            release_fixture::mainRoute(track.id + "-route", track.id, track.outputTrackId));
        project.tracks.push_back(std::move(track));
        project.tracks.push_back(std::move(take));
    }
    for (int index = 0; index < buses; ++index)
    {
        studio::Track bus;
        bus.id = "long-bus-" + juce::String(index);
        bus.name = "Synthetic bus";
        bus.type = studio::TrackType::bus;
        bus.outputTrackId = "long-master";
        project.routingConnections.push_back(
            release_fixture::mainRoute(bus.id + "-route", bus.id, bus.outputTrackId));
        project.tracks.push_back(std::move(bus));
    }
    studio::Track master;
    master.id = "long-master";
    master.name = "Synthetic master";
    master.type = studio::TrackType::master;
    addProcessor(master, "studio.device.limiter");
    project.tracks.push_back(std::move(master));
    for (int song = 0; song < profile.songs; ++song)
    {
        studio::SongSection section;
        section.id = "long-song-" + juce::String(song);
        section.name = "Synthetic song " + juce::String(song + 1);
        section.timeSeconds = song * profile.songSeconds;
        section.endTimeSeconds = (song + 1) * profile.songSeconds;
        project.sections.push_back(std::move(section));
    }
    return juce::Result::ok();
}

struct CallbackMeasurement
{
    double startMilliseconds = 0.0;
    double durationMilliseconds = 0.0;
    double latenessMilliseconds = 0.0;
    bool graphEditInProgress = false;
};

int qualifyLongSession(const LongSessionProfile& profile, const juce::File& output)
{
    if (output.exists())
    {
        std::cerr << "Qualification output must be a new directory; existing evidence is never overwritten.\n";
        return 2;
    }
    if (const auto result = output.createDirectory(); result.failed())
    {
        std::cerr << result.getErrorMessage() << '\n';
        return 1;
    }
    const auto workload = output.getChildFile("synthetic-workload");
    juce::DynamicObject::Ptr report = new juce::DynamicObject();
    report->setProperty("schemaVersion", 1);
    report->setProperty("scope", "synthetic-headless-characterisation");
    report->setProperty("candidateSha", STUDIO_DUO_BUILD_COMMIT);
    report->setProperty("buildRevision", STUDIO_DUO_BUILD_REVISION);
    report->setProperty("buildConfiguration", STUDIO_DUO_BUILD_CONFIG);
    report->setProperty("createdAt", juce::Time::getCurrentTime().toISO8601(true));
    report->setProperty("architecture", studio::currentArchitecture());
    report->setProperty("os", juce::SystemStats::getOperatingSystemName());
    report->setProperty("cpu", juce::SystemStats::getCpuModel());
    report->setProperty("ramMiB", juce::SystemStats::getMemorySizeInMegabytes());
    report->setProperty("physicalDevicesOpened", false);
    report->setProperty("paced", profile.paced);
    report->setProperty("cachePolicy", "Uncontrolled OS cache; first/repeated reads are not cold-cache certification.");
    report->setProperty("audioTracks", profile.tracks);
    report->setProperty("songs", profile.songs);
    report->setProperty("songSeconds", profile.songSeconds);
    report->setProperty("clipsPerSongPerTrack", profile.clipsPerSong);
    report->setProperty("alternateTakes", profile.tracks);
    report->setProperty("bundledInserts", profile.tracks + 1);
    report->setProperty("buses", std::max(1, std::min(8, profile.tracks / 4)));
    report->setProperty("audioClips", profile.tracks * (profile.songs * profile.clipsPerSong + 1));
    report->setProperty("thirdPartyInserts", 0);
    report->setProperty("automationPoints", profile.tracks * profile.songs * 64);
    report->setProperty("sampleRate", profile.sampleRate);
    report->setProperty("bufferSize", profile.bufferSize);
    report->setProperty("memoryBudgetMiB", profile.memoryBudgetMiB);
    report->setProperty("graphEditBudgetMilliseconds", 250.0);
    report->setProperty("callbackP99DeadlineRatioBudget", 0.7);
    juce::Array<juce::var> phases;
    juce::StringArray blockers;
    const auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };
    const auto finish = [&](juce::String status, juce::String reason)
    {
        if (workload.exists() && !workload.deleteRecursively())
        {
            blockers.add("Could not remove the owned synthetic workload directory.");
            status = "fail";
        }
        report->setProperty("status", status);
        report->setProperty("reason", reason.replace(output.getFullPathName(), "<qualification-output>"));
        report->setProperty("processPhases", phases);
        juce::Array<juce::var> recordedBlockers;
        for (const auto& blocker : blockers)
            recordedBlockers.add(blocker);
        report->setProperty("blockers", recordedBlockers);
        const auto json = juce::JSON::toString(juce::var(report.get()), true);
        if (!output.getChildFile("report.json").replaceWithText(json))
        {
            std::cerr << "Cannot write the qualification report.\n";
            return 1;
        }
        std::cout << json << '\n';
        return status == "pass" ? 0 : status == "blocked" ? 2 : 1;
    };
    const auto observe = [&](const char* phase)
    {
        ProcessStatistics statistics;
        if (const auto result = processStatistics(statistics); result.failed())
        {
            blockers.add(result.getErrorMessage());
            return false;
        }
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty("phase", phase);
        object->setProperty("peakWorkingSetBytes", static_cast<juce::int64>(statistics.peakMemoryBytes));
        object->setProperty("processCpuMilliseconds", statistics.cpuMilliseconds);
        object->setProperty("readIoOperations", static_cast<juce::int64>(statistics.readOperations));
        object->setProperty("writeIoOperations", static_cast<juce::int64>(statistics.writeOperations));
        phases.add(juce::var(object.release()));
        report->setProperty("status", "running");
        report->setProperty("processPhases", phases);
        if (!output.getChildFile("report.json").replaceWithText(juce::JSON::toString(juce::var(report.get()), true)))
        {
            blockers.add("Cannot checkpoint the in-progress qualification evidence.");
            return false;
        }
        const auto withinBudget = statistics.peakMemoryBytes
            <= static_cast<std::uint64_t>(profile.memoryBudgetMiB) * 1024 * 1024;
        if (!withinBudget)
            blockers.add("Peak working set exceeds the selected memory budget.");
        return withinBudget;
    };
    const auto totalSamples = static_cast<juce::int64>(profile.songs)
        * profile.songSeconds * profile.sampleRate;
    constexpr juce::int64 stereoFrameBytes = 2 * static_cast<juce::int64>(sizeof(float));
    const auto decodedBytes = static_cast<juce::int64>(profile.tracks)
        * profile.songs * profile.songSeconds * profile.sampleRate * stereoFrameBytes;
    const auto preparedBytes = decodedBytes + decodedBytes / profile.songs / profile.clipsPerSong;
    const auto estimatedPeakBytes = preparedBytes * 6
        + totalSamples * stereoFrameBytes * 2 + 512LL * 1024 * 1024;
    report->setProperty("decodedMediaBytes", decodedBytes);
    report->setProperty("preparedMediaBytesUpperBound", preparedBytes);
    report->setProperty("estimatedPreparedGenerations", 6);
    report->setProperty("estimatedPeakBytes", estimatedPeakBytes);
    if (totalSamples > std::numeric_limits<int>::max() - studio::PluginBridgeSharedState::maxSupportedLatencySamples
        || estimatedPeakBytes > static_cast<juce::int64>(profile.memoryBudgetMiB) * 1024 * 1024)
        return finish("blocked", "Whole-file decoded/prepared buffers exceed the selected preflight memory budget.");
    if (output.getBytesFreeOnVolume() < decodedBytes * 2 + 512LL * 1024 * 1024)
        return finish("blocked", "Insufficient free storage for unique long media and a validated portable copy.");
    if (!observe("baseline") || workload.createDirectory().failed())
        return finish("fail", "Cannot establish a bounded synthetic workload.");

    studio::Project project;
    std::vector<studio::StudioAudioEngine::PluginRuntimeRequest> requests;
    auto started = now();
    if (const auto result = makeLongProject(profile, workload, project, requests); result.failed())
        return finish("fail", result.getErrorMessage());
    report->setProperty("mediaWriteMilliseconds", now() - started);
    juce::int64 sourceBytes = 0;
    for (const auto& file : workload.findChildFiles(juce::File::findFiles, false, "*.wav"))
        sourceBytes += file.getSize();
    report->setProperty("sourceFileBytes", sourceBytes);
    const auto package = workload.getChildFile("long.studioduo");
    started = now();
    if (const auto result = studio::ProjectFile::save(project, package); result.failed())
        return finish("fail", result.getErrorMessage());
    const auto saveMs = now() - started;
    report->setProperty("saveMilliseconds", saveMs);
    juce::String error;
    for (int pass = 0; pass < 2; ++pass)
    {
        started = now();
        const auto loaded = studio::ProjectFile::load(package, error);
        const auto loadMs = now() - started;
        if (!loaded || release_fixture::snapshot(*loaded) != release_fixture::snapshot(project))
            return finish("fail", "Long-session native round trip failed: " + error);
        report->setProperty(pass == 0 ? "firstLoadMilliseconds" : "repeatedLoadMilliseconds", loadMs);
        if (loadMs > 10000.0)
            blockers.add("Native load exceeds 10 seconds.");
    }
    if (saveMs > 10000.0)
        blockers.add("Native save exceeds 10 seconds.");
    started = now();
    const auto portable = studio::ProjectCollectionService::savePortableCopy(
        project, package, workload.getChildFile("portable.studioduo"), error);
    report->setProperty("portableCopyMilliseconds", now() - started);
    const auto verified = portable ? studio::ProjectCollectionService::validatePortableCopy(
        workload.getChildFile("portable.studioduo"), error) : std::nullopt;
    if (!portable || !verified || verified->missingResources != 0 || verified->invalidResources != 0)
        return finish("fail", "Full-length portable media validation failed: " + error);
    if (!observe("media-save-portable"))
        return finish("fail", "Resource budget exceeded before graph construction.");

    auto engine = std::make_unique<studio::StudioAudioEngine>();
    if (const auto result = engine->configureAudioForTesting(profile.sampleRate, profile.bufferSize); result.failed())
        return finish("fail", result.getErrorMessage());
    started = now();
    const auto published = engine->updateProject(project, requests);
    if (published.failed() || !waitForGraph(*engine))
        return finish("fail", "Full-length graph publication failed: " + published.getErrorMessage());
    report->setProperty("initialGraphMilliseconds", now() - started);
    for (const auto& status : engine->pluginRuntimeStatuses())
        if (status.state != studio::StudioAudioEngine::PluginRuntimeStatus::State::ready)
            return finish("fail", "A bundled processor did not become ready: " + status.message);
    if (!observe("initial-graph"))
        return finish("fail", "Prepared-media memory exceeds the selected budget.");

    studio::TimelineComponent timeline;
    timeline.setProject(&project);
    timeline.setSize(timeline.preferredWidth(1024), timeline.preferredHeight(600));
    double maximumPaintMs = 0.0;
    for (const auto fraction : { 0.0, 0.5, 1.0 })
    {
        const auto viewX = static_cast<int>((timeline.getWidth() - 1024) * fraction);
        timeline.setViewportPosition(viewX);
        juce::Image image(juce::Image::ARGB, 1024, 600, true, juce::SoftwareImageType {});
        juce::Graphics graphics(image);
        graphics.addTransform(juce::AffineTransform::translation(-static_cast<float>(viewX), 0.0f));
        started = now();
        timeline.paint(graphics);
        maximumPaintMs = std::max(maximumPaintMs, now() - started);
    }
    report->setProperty("maximumViewportMilliseconds", maximumPaintMs);
    if (maximumPaintMs > 50.0)
        blockers.add("Software timeline viewport exceeds 50 milliseconds.");

    const auto outputLatency = engine->outputLatencySamplesForTesting();
    const auto playbackSamples = totalSamples + outputLatency;
    report->setProperty("declaredOutputLatencySamples", outputLatency);
    report->setProperty("contentSamples", totalSamples);
    juce::AudioBuffer<float> live(2, static_cast<int>(playbackSamples));
    std::vector<CallbackMeasurement> measurements;
    measurements.reserve(static_cast<std::size_t>((playbackSamples + profile.bufferSize - 1) / profile.bufferSize));
    std::atomic<bool> finished { false }, graphEditing { false }, cancel { false };
    std::atomic<juce::int64> renderedSamples { 0 };
    engine->seekSeconds(0.0);
    engine->play();
    ProcessStatistics beforePlayback;
    if (const auto result = processStatistics(beforePlayback); result.failed())
        return finish("fail", result.getErrorMessage());
    started = now();
    std::thread playback([&]
    {
        juce::AudioBuffer<float> block(2, profile.bufferSize);
        const auto start = std::chrono::steady_clock::now();
        for (juce::int64 offset = 0; offset < playbackSamples && !cancel.load(); offset += profile.bufferSize)
        {
            const auto expected = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(offset) / profile.sampleRate));
            if (profile.paced)
                std::this_thread::sleep_until(expected);
            const auto blockStart = now();
            const auto editing = graphEditing.load(std::memory_order_acquire);
            engine->renderActiveBlockForTesting(block);
            const auto duration = now() - blockStart;
            const auto lateness = profile.paced ? std::max(0.0,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                    - expected).count() - duration) : 0.0;
            measurements.push_back({ blockStart - started, duration, lateness, editing });
            const auto samples = static_cast<int>(std::min<juce::int64>(profile.bufferSize, playbackSamples - offset));
            for (int channel = 0; channel < 2; ++channel)
                live.copyFrom(channel, static_cast<int>(offset), block, channel, 0, samples);
            renderedSamples.store(offset + samples, std::memory_order_release);
        }
        finished.store(true, std::memory_order_release);
    });
    juce::Array<juce::var> graphEdits;
    auto edits = 0;
    while (!finished.load(std::memory_order_acquire))
    {
        if (edits < profile.songs
            && renderedSamples.load(std::memory_order_acquire) >= totalSamples * edits / profile.songs)
        {
            graphEditing.store(true, std::memory_order_release);
            project.tracks.front().name = "Synthetic graph edit " + juce::String(edits + 1);
            const auto editStarted = now();
            const auto result = engine->updateProject(project, requests);
            const auto ready = result.wasOk() && waitForGraph(*engine);
            const auto editMs = now() - editStarted;
            graphEditing.store(false, std::memory_order_release);
            auto item = std::make_unique<juce::DynamicObject>();
            item->setProperty("milliseconds", editMs);
            item->setProperty("completedDuringPlayback", !finished.load(std::memory_order_acquire));
            graphEdits.add(juce::var(item.release()));
            ++edits;
            if (!ready || editMs > 250.0)
                blockers.add("Graph republication exceeds 250 milliseconds or did not complete.");
            if (!ready || !observe("graph-edit"))
            {
                cancel.store(true);
                break;
            }
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    playback.join();
    const auto playbackMs = now() - started;
    engine->stop();
    report->setProperty("graphEdits", graphEdits);
    report->setProperty("graphEditProcedure", "Rename a track; rebuild identical audio while callbacks continue.");
    report->setProperty("playbackWallMilliseconds", playbackMs);
    report->setProperty("renderedSamples", renderedSamples.load());
    ProcessStatistics afterPlayback;
    if (const auto result = processStatistics(afterPlayback); result.failed())
        return finish("fail", result.getErrorMessage());
    report->setProperty("playbackProcessCpuMilliseconds",
        afterPlayback.cpuMilliseconds - beforePlayback.cpuMilliseconds);
    const auto incompletePlayback = cancel.load() || renderedSamples.load() != playbackSamples;
    if (measurements.empty())
        return finish("fail", "No callbacks were measured before playback stopped.");
    const auto deadlineMs = 1000.0 * profile.bufferSize / profile.sampleRate;
    auto misses = 0;
    auto overlap = 0;
    std::vector<double> callbackTimes;
    callbackTimes.reserve(measurements.size());
    auto csv = output.getChildFile("callbacks.csv").createOutputStream();
    if (csv == nullptr || !csv->writeText(
            "block,start_ms,callback_ms,start_lateness_ms,deadline_ms,graph_edit_in_progress\n", false, false, "\n"))
        return finish("fail", "Cannot create raw callback measurements.");
    for (std::size_t index = 0; index < measurements.size(); ++index)
    {
        const auto& measurement = measurements[index];
        callbackTimes.push_back(measurement.durationMilliseconds);
        misses += measurement.durationMilliseconds + measurement.latenessMilliseconds > deadlineMs ? 1 : 0;
        overlap += measurement.graphEditInProgress ? 1 : 0;
        const auto row = juce::String(static_cast<juce::int64>(index)) + ","
            + juce::String(measurement.startMilliseconds, 6) + ","
            + juce::String(measurement.durationMilliseconds, 6) + ","
            + juce::String(measurement.latenessMilliseconds, 6) + ","
            + juce::String(deadlineMs, 6) + "," + juce::String(measurement.graphEditInProgress ? 1 : 0) + "\n";
        if (!csv->writeText(row, false, false, "\n"))
            return finish("fail", "Cannot write raw callback measurements.");
    }
    csv->flush();
    if (csv->getStatus().failed())
        return finish("fail", "Cannot flush raw callback measurements.");
    csv.reset();
    std::sort(callbackTimes.begin(), callbackTimes.end());
    const auto p99 = callbackTimes[
        static_cast<std::size_t>(std::ceil(static_cast<double>(callbackTimes.size()) * 0.99)) - 1];
    report->setProperty("callbackDeadlineMilliseconds", deadlineMs);
    report->setProperty("maximumCallbackMilliseconds", callbackTimes.back());
    report->setProperty("p99CallbackMilliseconds", p99);
    report->setProperty("callbackDeadlineMisses", misses);
    report->setProperty("callbacksDuringGraphEdits", overlap);
    report->setProperty("pluginLateBlocks", static_cast<juce::int64>(engine->pluginLateBlockCount()));
    report->setProperty("pluginAutomationDrops", static_cast<juce::int64>(engine->pluginAutomationEventDropCount()));
    if (misses != 0 || p99 > deadlineMs * 0.7)
        blockers.add("Headless callback cost or paced harness scheduling exceeds its deadline budget.");
    if (overlap == 0 || edits == 0)
        blockers.add("No measured callback overlapped graph reconstruction.");
    if (engine->pluginLateBlockCount() != 0 || engine->pluginAutomationEventDropCount() != 0)
        blockers.add("Processor deadlines or automation event delivery failed.");
    report->setProperty("sourceDecodeCalls", static_cast<juce::int64>(engine->sourceReadCountForTesting()));
    if (incompletePlayback)
        return finish("fail", "Playback stopped at a resource bound; completed callback measurements are retained.");
    if (!observe("playback"))
        return finish("fail", "Playback peak memory exceeds its budget.");
    engine->shutdown();
    engine.reset();

    juce::AudioBuffer<float> offline;
    auto offlineEngine = std::make_unique<studio::StudioAudioEngine>();
    started = now();
    const auto rendered = offlineEngine->renderRangeToBuffer(project, offline, profile.sampleRate,
        { 0.0, static_cast<double>(profile.songs * profile.songSeconds), 0.0 }, requests);
    report->setProperty("offlineRenderMilliseconds", now() - started);
    if (rendered.failed() || offline.getNumSamples() != totalSamples || offline.getNumChannels() != 2)
        return finish("fail", "Full-session offline render failed: " + rendered.getErrorMessage());
    auto nullError = 0.0f;
    auto finite = true;
    auto firstNullErrorSample = -1;
    auto lastNullErrorSample = -1;
    auto maximumNullErrorSample = -1;
    juce::int64 differingSamples = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < offline.getNumSamples(); ++sample)
        {
            const auto difference = std::abs(
                live.getSample(channel, sample + outputLatency) - offline.getSample(channel, sample));
            finite = finite && std::isfinite(difference);
            if (difference > 1e-5f && (firstNullErrorSample < 0 || sample < firstNullErrorSample))
                firstNullErrorSample = sample;
            if (difference > 1e-5f)
            {
                lastNullErrorSample = std::max(lastNullErrorSample, sample);
                ++differingSamples;
            }
            if (difference > nullError)
                maximumNullErrorSample = sample;
            nullError = std::max(nullError, difference);
        }
    report->setProperty("maximumNullError", static_cast<double>(nullError));
    report->setProperty("firstNullErrorSample", firstNullErrorSample);
    report->setProperty("lastNullErrorSample", lastNullErrorSample);
    report->setProperty("maximumNullErrorSample", maximumNullErrorSample);
    report->setProperty("differingChannelSamples", differingSamples);
    report->setProperty("livePeak", live.getMagnitude(0, live.getNumSamples()));
    if (!finite || nullError > 1e-5f || live.getMagnitude(0, live.getNumSamples()) <= 1e-5f)
        blockers.add("The full-session deterministic live/offline null exceeds 1e-5 or contains invalid/silent audio.");
    if (!observe("offline-render"))
        return finish("fail", "Offline render peak memory exceeds its budget.");
    offlineEngine->shutdown();
    offlineEngine.reset();
    return finish(blockers.isEmpty() ? "pass" : "fail",
        "Computational characterisation only; no device, vendor, assistive-technology, or release sign-off.");
}
}

std::optional<int> runLongSessionQualification(const juce::StringArray& arguments)
{
    if (!arguments.contains("--qualify-long-session"))
        return std::nullopt;
    LongSessionProfile profile;
    juce::String output;
    if (const auto result = parseLongSessionProfile(arguments, profile, output); result.failed())
    {
        std::cerr << result.getErrorMessage() << '\n';
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI initialiser;
    return qualifyLongSession(profile, juce::File(output));
}

void longSessionQualificationTests()
{
    LongSessionProfile profile;
    juce::String output;
    expect(parseLongSessionProfile({ "--tracks=0", "--qualification-output=relative" }, profile, output).failed(),
           "Unsafe profile sizes and relative evidence paths are rejected.");
    profile = {};
    expect(parseLongSessionProfile({ "--tracks=2", "--tracks=3" }, profile, output).failed(),
           "Duplicate profile settings cannot silently override the requested workload.");
    profile = {};
    expect(parseLongSessionProfile({ "--unknown=1" }, profile, output).failed(),
           "Unknown qualification settings fail instead of changing the measurement implicitly.");
    release_fixture::Directory directory;
    profile = {};
    const auto boundedDestination = directory.file.getChildFile("memory-preflight");
    expect(qualifyLongSession(profile, boundedDestination) == 2,
           "The full-length default profile is rejected before allocation when graph generations exceed 16 GiB.");
    const auto preflight = juce::JSON::parse(boundedDestination.getChildFile("report.json"));
    expect(preflight["status"].toString() == "blocked"
               && static_cast<juce::int64>(preflight["estimatedPeakBytes"]) == 22517030912LL
               && !boundedDestination.getChildFile("synthetic-workload").exists(),
           "The preflight includes alternate-take preparation and six concurrent graph generations, not just source PCM.");
    profile = {};
    profile.tracks = 2;
    profile.songSeconds = 1;
    profile.bufferSize = 1024;
    profile.sampleRate = 96000;
    const auto destination = directory.file.getChildFile("characterisation");
    const auto outcome = qualifyLongSession(profile, destination);
    const auto report = juce::JSON::parse(destination.getChildFile("report.json"));
    const auto deadlineExceeded = static_cast<int>(report["callbackDeadlineMisses"]) > 0
        || static_cast<double>(report["p99CallbackMilliseconds"])
            > static_cast<double>(report["callbackDeadlineMilliseconds"]) * 0.7;
    const auto* blockers = report["blockers"].getArray();
    expect(outcome == (deadlineExceeded ? 1 : 0)
               && report["status"].toString() == (deadlineExceeded ? "fail" : "pass")
               && blockers != nullptr && blockers->size() == (deadlineExceeded ? 1 : 0)
               && (!deadlineExceeded || blockers->getReference(0).toString()
                   == "Headless callback cost or paced harness scheduling exceeds its deadline budget."),
           "The smoke verifies the CLI's strict outcome contract and retains shared-runner deadline misses as failed evidence.");
    expect(static_cast<double>(report["maximumCallbackMilliseconds"]) < 100.0
               && static_cast<double>(report["playbackWallMilliseconds"]) < 10000.0,
           "The shared-runner smoke stays inside generous regression bounds, not physical audio deadlines.");
    expect(report["physicalDevicesOpened"] == juce::var(false)
               && report["scope"].toString() == "synthetic-headless-characterisation"
               && static_cast<double>(report["sampleRate"]) == 96000.0
               && static_cast<juce::int64>(report["contentSamples"]) == 288000
               && static_cast<int>(report["declaredOutputLatencySamples"]) == 16
               && static_cast<juce::int64>(report["renderedSamples"]) == 288016
               && static_cast<double>(report["maximumNullError"]) <= 1e-5
               && destination.getChildFile("callbacks.csv").getSize() > 100
               && !destination.getChildFile("synthetic-workload").exists(),
           "Evidence records its exact synthetic scope/rate/counts and cleans only the generated workload.");
    const auto reportPath = juce::SystemStats::getEnvironmentVariable("STUDIO_DUO_LONG_SESSION_SMOKE_REPORT", {});
    if (reportPath.isNotEmpty())
    {
        const juce::File retained(reportPath);
        expect(retained.getParentDirectory().createDirectory().wasOk()
                   && retained.replaceWithText(juce::JSON::toString(report, true))
                   && destination.getChildFile("callbacks.csv").copyFileTo(retained.withFileExtension(".csv")),
               "CI can retain the explicitly synthetic smoke report and raw callback measurements.");
    }
    expect(qualifyLongSession(profile, destination) == 2,
           "Qualification cannot overwrite an existing evidence directory.");
    auto engine = std::make_unique<studio::StudioAudioEngine>();
    expect(engine->configureAudioForTesting(0.0, 256).failed()
               && engine->configureAudioForTesting(96000.0, 128).wasOk(),
           "Driver-free configuration validates the actual callback sample rate and block size.");
    engine->play();
    expect(engine->configureAudioForTesting(48000.0, 256).failed(),
           "A measurement cannot change the audio clock during playback.");
    engine->stop();
}
