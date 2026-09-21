#include "StudioLogger.h"

#include <algorithm>
#include <utility>

namespace studio
{
namespace
{
constexpr int maximumQueuedEntries = 16384;

const char* levelName(StudioLogLevel level)
{
    switch (level)
    {
        case StudioLogLevel::error:
            return "ERROR";
        case StudioLogLevel::info:
            return "INFO";
        case StudioLogLevel::debug:
            return "DEBUG";
    }
    return "INFO";
}

juce::String sanitizedSessionId()
{
    return juce::Uuid().toString()
        .removeCharacters("-")
        .substring(0, 12);
}

juce::Result compressLogFile(const juce::File& source)
{
    auto input = source.createInputStream();
    if (input == nullptr)
        return juce::Result::fail(
            "Could not open "
            + source.getFileName()
            + " for compression.");

    const auto destination = source.getSiblingFile(
        source.getFileName() + ".gz");
    const auto temporary = destination.getSiblingFile(
        destination.getFileName()
            + ".tmp-"
            + juce::Uuid().toString());
    auto output = temporary.createOutputStream();
    if (output == nullptr)
        return juce::Result::fail(
            "Could not create a compressed log file for "
            + source.getFileName()
            + ".");

    const auto sourceSize = source.getSize();
    auto complete = false;
    {
        juce::GZIPCompressorOutputStream compressed(
            *output,
            6,
            juce::GZIPCompressorOutputStream::windowBitsGZIP);
        complete =
            compressed.writeFromInputStream(*input, -1)
            == sourceSize;
        compressed.flush();
    }
    output->flush();
    if (!complete)
    {
        output.reset();
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not fully compress "
            + source.getFileName()
            + ".");
    }
    if (output->getStatus().failed())
    {
        const auto error = output->getStatus().getErrorMessage();
        output.reset();
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not finish compressed log "
            + source.getFileName()
            + ": "
            + error);
    }
    output.reset();

    const auto published = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);
    if (!published)
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not publish compressed log "
            + destination.getFileName()
            + ".");
    }

    const auto sourceTime = source.getLastModificationTime();
    if (!destination.setLastModificationTime(sourceTime))
    {
        destination.deleteFile();
        return juce::Result::fail(
            "Could not preserve the timestamp of "
            + destination.getFileName()
            + ".");
    }
    if (!source.deleteFile())
    {
        return juce::Result::fail(
            "Compressed "
            + source.getFileName()
            + " but could not remove the original.");
    }
    return juce::Result::ok();
}

void logAtLevel(
    StudioLogLevel level,
    const juce::String& category,
    const juce::String& message)
{
    if (auto* logger = dynamic_cast<StudioLogger*>(
            juce::Logger::getCurrentLogger()))
    {
        logger->log(level, category, message);
        return;
    }

    juce::Logger::writeToLog(
        "["
        + juce::String(levelName(level))
        + "] "
        + category
        + ": "
        + redactLogMessage(message));
}
}

StudioLoggerOptions loadStudioLoggerOptions(
    const juce::File& settingsFile,
    juce::StringArray& errors)
{
    StudioLoggerOptions options;
    if (!settingsFile.existsAsFile())
        return options;

    const auto root = juce::JSON::parse(
        settingsFile.loadFileAsString());
    const auto* object = root.getDynamicObject();
    if (object == nullptr
        || static_cast<int>(
               object->getProperty("schemaVersion"))
            != 1)
    {
        errors.add(
            "logging.json is corrupt or uses an unsupported schema; defaults are active.");
        return options;
    }

    const auto retention =
        object->getProperty("retentionDays");
    if (retention.isInt()
        || retention.isInt64()
        || retention.isDouble())
    {
        const auto days = static_cast<int>(retention);
        if (days >= 1 && days <= 365)
            options.retentionDays = days;
        else
            errors.add(
                "logging.json retentionDays must be between 1 and 365; using 7.");
    }
    else if (!retention.isVoid())
    {
        errors.add(
            "logging.json retentionDays must be a number; using 7.");
    }

    const auto debug =
        object->getProperty("debugLogging");
    if (debug.isBool())
        options.debugLogging = static_cast<bool>(debug);
    else if (!debug.isVoid())
        errors.add(
            "logging.json debugLogging must be true or false; debug logging remains disabled.");
    return options;
}

juce::StringArray maintainStudioLogFiles(
    const juce::File& logDirectory,
    const juce::File& activeLogFile,
    int retentionDays,
    juce::Time now)
{
    juce::StringArray errors;
    if (!logDirectory.createDirectory())
    {
        errors.add(
            "Could not create the log directory.");
        return errors;
    }

    const auto deleteBefore = now
        - juce::RelativeTime::days(
            juce::jlimit(1, 365, retentionDays));
    const auto compressBefore =
        now - juce::RelativeTime::hours(24);

    for (const auto& file : logDirectory.findChildFiles(
             juce::File::findFiles,
             false,
             "studio-duo-*.log.gz.tmp-*"))
    {
        if (file.getLastModificationTime() < compressBefore
            && !file.deleteFile())
        {
            errors.add(
                "Could not delete stale log temporary file "
                + file.getFileName()
                + ".");
        }
    }

    for (const auto& file : logDirectory.findChildFiles(
             juce::File::findFiles,
             false,
             "studio-duo-*.log.gz"))
    {
        if (file.getLastModificationTime() < deleteBefore
            && !file.deleteFile())
        {
            errors.add(
                "Could not delete expired log "
                + file.getFileName()
                + ".");
        }
    }

    for (const auto& file : logDirectory.findChildFiles(
             juce::File::findFiles,
             false,
             "studio-duo-crash-*.dmp"))
    {
        if (file.getLastModificationTime() < deleteBefore
            && !file.deleteFile())
        {
            errors.add(
                "Could not delete expired native dump "
                + file.getFileName()
                + ".");
        }
    }

    for (const auto& file : logDirectory.findChildFiles(
             juce::File::findFiles,
             false,
             "studio-duo-*.log"))
    {
        if (file == activeLogFile)
            continue;

        const auto modified = file.getLastModificationTime();
        if (modified < deleteBefore)
        {
            if (!file.deleteFile())
            {
                errors.add(
                    "Could not delete expired log "
                    + file.getFileName()
                    + ".");
            }
            continue;
        }
        if (modified < compressBefore)
        {
            if (const auto result = compressLogFile(file);
                result.failed())
            {
                errors.add(result.getErrorMessage());
            }
        }
    }
    return errors;
}

juce::String redactLogMessage(const juce::String& message)
{
    auto redacted = message;
    const auto workingDirectory =
        juce::File::getCurrentWorkingDirectory()
            .getFullPathName();
    const auto homeDirectory =
        juce::File::getSpecialLocation(
            juce::File::userHomeDirectory)
            .getFullPathName();

    const auto redactPath = [&redacted](
                                const juce::String& path,
                                const juce::String& replacement)
    {
        if (path.length() <= 1)
            return;
        redacted = redacted.replace(
            path,
            replacement,
            true);
        redacted = redacted.replace(
            path.replaceCharacter('\\', '/'),
            replacement,
            true);
    };
    redactPath(workingDirectory, "<workspace>");
    redactPath(homeDirectory, "~");
    return redacted
        .replace("\r\n", "\\n")
        .replace("\r", "\\n")
        .replace("\n", "\\n");
}

StudioLogger::StudioLogger(
    juce::File directory,
    StudioLoggerOptions loggerOptions)
    : juce::Thread("Studio Duo file logger"),
      logDirectory(std::move(directory)),
      options(loggerOptions),
      sessionId(sanitizedSessionId())
{
    threadStarted = startThread();
    if (!threadStarted)
    {
        juce::Logger::outputDebugString(
            "Studio Duo could not start the file logger thread.");
    }
}

StudioLogger::~StudioLogger()
{
    flush();
    signalThreadShouldExit();
    wakeEvent.signal();
    if (threadStarted)
        stopThread(5000);
    closeOutput();
}

std::unique_ptr<StudioLogger> StudioLogger::createDefault()
{
    juce::StringArray errors;
    const auto options = loadStudioLoggerOptions(
        defaultSettingsFile(),
        errors);
    auto logger = std::make_unique<StudioLogger>(
        defaultLogDirectory(),
        options);
    logger->log(
        StudioLogLevel::debug,
        "logging.lifecycle",
        "File logging started; retention "
            + juce::String(options.retentionDays)
            + " days; debug "
            + (options.debugLogging ? "enabled." : "disabled."));
    for (const auto& error : errors)
    {
        logger->log(
            StudioLogLevel::error,
            "logging.config",
            error);
    }
    return logger;
}

juce::File StudioLogger::defaultLogDirectory()
{
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Studio Duo")
        .getChildFile("Logs");
}

juce::File StudioLogger::defaultSettingsFile()
{
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Studio Duo")
        .getChildFile("logging.json");
}

void StudioLogger::logMessage(const juce::String& message)
{
    log(StudioLogLevel::info, {}, message);
}

void StudioLogger::log(
    StudioLogLevel level,
    const juce::String& category,
    const juce::String& message)
{
    if (level == StudioLogLevel::debug
        && !options.debugLogging)
    {
        return;
    }

    Entry entry;
    entry.level = level;
    entry.time = juce::Time::getCurrentTime();
    entry.category = category;
    entry.message = message;
    enqueue(std::move(entry));
}

void StudioLogger::flush()
{
    if (!threadStarted
        || !isThreadRunning()
        || juce::Thread::getCurrentThreadId()
            == getThreadId())
    {
        return;
    }

    auto completed =
        std::make_shared<juce::WaitableEvent>();
    Entry entry;
    entry.kind = EntryKind::flush;
    entry.completion = completed;
    enqueue(std::move(entry));
    if (!completed->wait(5000))
        writeInternalError("Timed out waiting for queued log entries to reach disk.");
}

void StudioLogger::run()
{
    runMaintenance(juce::Time::getCurrentTime());
    while (!threadShouldExit())
    {
        wakeEvent.wait(1000);

        std::deque<Entry> pending;
        auto dropped = 0;
        {
            const juce::ScopedLock lock(queueLock);
            pending.swap(queue);
            dropped = std::exchange(droppedEntries, 0);
        }

        if (dropped > 0)
        {
            Entry overflow;
            overflow.level = StudioLogLevel::error;
            overflow.time = juce::Time::getCurrentTime();
            overflow.category = "logging.queue";
            overflow.message =
                juce::String(dropped)
                + " log entries were dropped because the queue was full.";
            writeEntry(overflow);
        }

        auto flushed = false;
        for (const auto& entry : pending)
        {
            if (entry.kind == EntryKind::flush)
            {
                if (output != nullptr)
                    output->flush();
                if (entry.completion != nullptr)
                    entry.completion->signal();
                flushed = true;
                continue;
            }
            writeEntry(entry);
        }
        if (!pending.empty() && output != nullptr)
            output->flush();

        if (flushed)
            continue;

        const auto now = juce::Time::getCurrentTime();
        if (output != nullptr
            && now.formatted("%Y-%m-%d") != activeDay)
        {
            closeOutput();
        }
        if (lastMaintenance.getMilliseconds() == 0
            || now - lastMaintenance
                >= juce::RelativeTime::minutes(5))
        {
            runMaintenance(now);
        }
    }

    std::deque<Entry> pending;
    {
        const juce::ScopedLock lock(queueLock);
        pending.swap(queue);
    }
    for (const auto& entry : pending)
    {
        if (entry.kind == EntryKind::message)
            writeEntry(entry);
        else if (entry.completion != nullptr)
            entry.completion->signal();
    }
    if (output != nullptr)
        output->flush();
}

void StudioLogger::enqueue(Entry entry)
{
    if (!threadStarted)
    {
        juce::Logger::outputDebugString(
            entry.category
            + ": "
            + redactLogMessage(entry.message));
        if (entry.completion != nullptr)
            entry.completion->signal();
        return;
    }

    {
        const juce::ScopedLock lock(queueLock);
        if (entry.kind == EntryKind::message
            && queue.size() >= maximumQueuedEntries)
        {
            const auto droppable = std::find_if(
                queue.begin(),
                queue.end(),
                [](const Entry& queued)
                {
                    return queued.kind == EntryKind::message
                        && queued.level
                            != StudioLogLevel::error;
                });
            if (droppable != queue.end())
            {
                queue.erase(droppable);
                ++droppedEntries;
            }
            else if (entry.level != StudioLogLevel::error)
            {
                ++droppedEntries;
                return;
            }
            else
            {
                queue.pop_front();
                ++droppedEntries;
            }
        }
        queue.push_back(std::move(entry));
    }
    wakeEvent.signal();
}

void StudioLogger::writeEntry(const Entry& entry)
{
    if (!ensureOutputFor(entry.time))
        return;

    auto line = entry.time.toISO8601(true)
        + " ["
        + levelName(entry.level)
        + "] ";
    if (entry.category.isNotEmpty())
        line += entry.category + ": ";
    line += redactLogMessage(entry.message) + "\n";
    const auto bytes = line.getNumBytesAsUTF8();
    if (!output->write(line.toRawUTF8(), bytes))
    {
        writeInternalError(
            "Could not append to "
            + activeLogFile.getFileName()
            + ".");
        closeOutput();
        return;
    }
    if (entry.level == StudioLogLevel::error)
        output->flush();
}

void StudioLogger::writeInternalError(
    const juce::String& message)
{
    juce::Logger::outputDebugString(
        "Studio Duo file logger: " + message);
}

bool StudioLogger::ensureOutputFor(juce::Time time)
{
    const auto day = time.formatted("%Y-%m-%d");
    if (output != nullptr && day == activeDay)
        return true;

    closeOutput();
    activeDay = day;
    activeLogFile = logDirectory.getChildFile(
        "studio-duo-"
        + activeDay
        + "-"
        + sessionId
        + ".log");
    if (!logDirectory.createDirectory())
    {
        writeInternalError(
            "Could not create the log directory.");
        return false;
    }
    output = activeLogFile.createOutputStream();
    if (output == nullptr)
    {
        writeInternalError(
            "Could not open "
            + activeLogFile.getFileName()
            + ".");
        return false;
    }
    return true;
}

void StudioLogger::closeOutput()
{
    if (output != nullptr)
        output->flush();
    output.reset();
    activeDay.clear();
    activeLogFile = juce::File();
}

void StudioLogger::runMaintenance(juce::Time now)
{
    for (const auto& error : maintainStudioLogFiles(
             logDirectory,
             activeLogFile,
             options.retentionDays,
             now))
    {
        Entry entry;
        entry.level = StudioLogLevel::error;
        entry.time = now;
        entry.category = "logging.maintenance";
        entry.message = error;
        writeEntry(entry);
    }
    lastMaintenance = now;
}

void logError(
    const juce::String& category,
    const juce::String& message)
{
    logAtLevel(
        StudioLogLevel::error,
        category,
        message);
}

void logInfo(
    const juce::String& category,
    const juce::String& message)
{
    logAtLevel(
        StudioLogLevel::info,
        category,
        message);
}

void logDebug(
    const juce::String& category,
    const juce::String& message)
{
    logAtLevel(
        StudioLogLevel::debug,
        category,
        message);
}

void flushStudioLog()
{
    if (auto* logger = dynamic_cast<StudioLogger*>(
            juce::Logger::getCurrentLogger()))
        logger->flush();
}
}
