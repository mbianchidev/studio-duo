#include "TestHarness.h"
#include "TestSuites.h"

#include "logging/StudioLogger.h"

namespace
{
juce::File temporaryLogDirectory()
{
    const auto directory =
        juce::File::getSpecialLocation(
            juce::File::tempDirectory)
            .getChildFile(
                "studio-duo-logging-tests-"
                + juce::Uuid().toString());
    directory.createDirectory();
    return directory;
}
}

void loggingTests()
{
    const auto now = juce::Time::getCurrentTime();
    const auto maintenanceDirectory =
        temporaryLogDirectory();
    const auto recent = maintenanceDirectory
        .getChildFile("studio-duo-recent.log");
    const auto old = maintenanceDirectory
        .getChildFile("studio-duo-old.log");
    const auto expired = maintenanceDirectory
        .getChildFile("studio-duo-expired.log");
    const auto expiredArchive = maintenanceDirectory
        .getChildFile("studio-duo-expired.log.gz");
    const auto staleTemporary = maintenanceDirectory
        .getChildFile(
            "studio-duo-old.log.gz.tmp-interrupted");
    recent.replaceWithText("recent");
    old.replaceWithText("old");
    expired.replaceWithText("expired");
    expiredArchive.replaceWithText("expired archive");
    staleTemporary.replaceWithText("partial archive");
    recent.setLastModificationTime(
        now - juce::RelativeTime::hours(12));
    old.setLastModificationTime(
        now - juce::RelativeTime::days(2));
    expired.setLastModificationTime(
        now - juce::RelativeTime::days(8));
    expiredArchive.setLastModificationTime(
        now - juce::RelativeTime::days(8));
    staleTemporary.setLastModificationTime(
        now - juce::RelativeTime::days(2));

    const auto maintenanceErrors =
        studio::maintainStudioLogFiles(
            maintenanceDirectory,
            {},
            7,
            now);
    expect(
        maintenanceErrors.isEmpty(),
        maintenanceErrors.joinIntoString("\n")
            .toRawUTF8());
    expect(
        recent.existsAsFile(),
        "Logs from the last 24 hours remain uncompressed.");
    const auto oldArchive = old.getSiblingFile(
        old.getFileName() + ".gz");
    expect(
        !old.existsAsFile()
            && oldArchive.existsAsFile(),
        "Logs older than 24 hours are gzip compressed.");
    if (auto compressedInput =
            oldArchive.createInputStream())
    {
        juce::GZIPDecompressorInputStream decompressed(
            compressedInput.release(),
            true,
            juce::GZIPDecompressorInputStream::gzipFormat);
        expect(
            decompressed.readEntireStreamAsString()
                == "old",
            "Compressed logs preserve their content.");
    }
    else
    {
        expect(
            false,
            "Compressed logs can be reopened.");
    }
    expect(
        !expired.existsAsFile()
            && !expiredArchive.existsAsFile(),
        "Logs older than the retention period are deleted.");
    expect(
        !staleTemporary.existsAsFile(),
        "Interrupted compression files are cleaned up.");
    maintenanceDirectory.deleteRecursively();

    const auto optionsDirectory =
        temporaryLogDirectory();
    const auto settings =
        optionsDirectory.getChildFile("logging.json");
    settings.replaceWithText(
        R"json({"schemaVersion":1,"retentionDays":14,"debugLogging":true})json");
    juce::StringArray optionErrors;
    const auto options =
        studio::loadStudioLoggerOptions(
            settings,
            optionErrors);
    expect(
        optionErrors.isEmpty()
            && options.retentionDays == 14
            && options.debugLogging,
        "Logging options load from logging.json.");
    optionsDirectory.deleteRecursively();

    const auto outputDirectory =
        temporaryLogDirectory();
    studio::StudioLoggerOptions loggerOptions;
    {
        studio::StudioLogger logger(
            outputDirectory,
            loggerOptions);
        logger.log(
            studio::StudioLogLevel::debug,
            "test.debug",
            "hidden debug entry");
        logger.log(
            studio::StudioLogLevel::info,
            "test.info",
            "visible info entry");
        logger.log(
            studio::StudioLogLevel::error,
            "test.error",
            "visible error entry");
        logger.flush();
    }
    const auto files = outputDirectory.findChildFiles(
        juce::File::findFiles,
        false,
        "studio-duo-*.log");
    expect(
        files.size() == 1,
        "The logger creates one process-specific daily file.");
    if (!files.isEmpty())
    {
        const auto content =
            files.getFirst().loadFileAsString();
        expect(
            content.contains("visible info entry")
                && content.contains("visible error entry"),
            "Info and error entries are written.");
        expect(
            !content.contains("hidden debug entry"),
            "Debug entries are disabled by default.");
    }
    outputDirectory.deleteRecursively();

    const auto debugDirectory =
        temporaryLogDirectory();
    loggerOptions.debugLogging = true;
    {
        studio::StudioLogger logger(
            debugDirectory,
            loggerOptions);
        logger.log(
            studio::StudioLogLevel::debug,
            "test.debug",
            "visible debug entry");
        logger.flush();
    }
    const auto debugFiles = debugDirectory.findChildFiles(
        juce::File::findFiles,
        false,
        "studio-duo-*.log");
    expect(
        debugFiles.size() == 1
            && debugFiles.getFirst()
                   .loadFileAsString()
                   .contains("visible debug entry"),
        "Debug entries are written when enabled.");
    debugDirectory.deleteRecursively();

    const auto redacted =
        studio::redactLogMessage(
            juce::File::getSpecialLocation(
                juce::File::userHomeDirectory)
                    .getFullPathName()
                + "/private/project.studioduo");
    expect(
        redacted.startsWith("~/"),
        "User-home paths are redacted in logs.");
}
