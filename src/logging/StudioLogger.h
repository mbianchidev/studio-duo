#pragma once

#include <juce_core/juce_core.h>

#include <deque>
#include <memory>

namespace studio
{
enum class StudioLogLevel
{
    error,
    info,
    debug
};

struct StudioLoggerOptions
{
    int retentionDays = 7;
    bool debugLogging = false;
};

[[nodiscard]] StudioLoggerOptions loadStudioLoggerOptions(
    const juce::File& settingsFile,
    juce::StringArray& errors);
[[nodiscard]] juce::StringArray maintainStudioLogFiles(
    const juce::File& logDirectory,
    const juce::File& activeLogFile,
    int retentionDays,
    juce::Time now);
[[nodiscard]] juce::String redactLogMessage(
    const juce::String& message);

class StudioLogger final : public juce::Logger,
                           private juce::Thread
{
public:
    StudioLogger(
        juce::File logDirectory,
        StudioLoggerOptions options);
    ~StudioLogger() override;

    [[nodiscard]] static std::unique_ptr<StudioLogger>
        createDefault();
    [[nodiscard]] static juce::File defaultLogDirectory();
    [[nodiscard]] static juce::File defaultSettingsFile();

    void logMessage(const juce::String& message) override;
    void log(
        StudioLogLevel level,
        const juce::String& category,
        const juce::String& message);
    void flush();

private:
    enum class EntryKind
    {
        message,
        flush
    };

    struct Entry
    {
        EntryKind kind = EntryKind::message;
        StudioLogLevel level = StudioLogLevel::info;
        juce::Time time;
        juce::String category;
        juce::String message;
        std::shared_ptr<juce::WaitableEvent> completion;
    };

    void run() override;
    void enqueue(Entry entry);
    void writeEntry(const Entry& entry);
    void writeInternalError(const juce::String& message);
    bool ensureOutputFor(juce::Time time);
    void closeOutput();
    void runMaintenance(juce::Time now);

    juce::File logDirectory;
    StudioLoggerOptions options;
    juce::String sessionId;
    juce::String activeDay;
    juce::File activeLogFile;
    std::unique_ptr<juce::FileOutputStream> output;
    juce::CriticalSection queueLock;
    std::deque<Entry> queue;
    juce::WaitableEvent wakeEvent;
    juce::Time lastMaintenance;
    int droppedEntries = 0;
    bool threadStarted = false;
};

void logError(
    const juce::String& category,
    const juce::String& message);
void logInfo(
    const juce::String& category,
    const juce::String& message);
void logDebug(
    const juce::String& category,
    const juce::String& message);
}
