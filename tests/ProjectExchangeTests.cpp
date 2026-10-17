#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "project_io/ProjectFile.h"

#include <bit>
#include <cstdint>

namespace
{
juce::Result writeExchange(const juce::File& package)
{
    if (package.exists())
        return juce::Result::fail("The exchange-fixture destination already exists.");
    const auto media = package.getChildFile("media/synthetic-phase.wav");
    if (const auto result = release_fixture::writeMedia(media); result.failed())
        return result;
    return studio::ProjectFile::save(release_fixture::exchangeProject(media), package);
}

juce::Result verifyExchange(const juce::File& package)
{
    const auto manifestFile = package.getChildFile("manifest.json");
    const auto manifestBytes = manifestFile.loadFileAsString();
    const auto manifest = juce::JSON::parse(manifestBytes);
    const auto sessionFile = package.getChildFile(manifest["activeSession"].toString());
    const auto sessionBytes = sessionFile.loadFileAsString();
    if (!sessionBytes.contains("${PROJECT_DIR}/media/synthetic-phase.wav")
        || sessionBytes.contains(package.getFullPathName()))
        return juce::Result::fail("The exchange fixture did not serialize package-relative media.");
    juce::String error;
    const auto project = studio::ProjectFile::load(package, error);
    if (!project)
        return juce::Result::fail("The exchanged native package did not open: " + error);
    const auto media = package.getChildFile("media/synthetic-phase.wav");
    if (!media.existsAsFile())
        return juce::Result::fail("The exchanged synthetic media is missing.");
    const auto expected = release_fixture::exchangeProject(media);
    if (release_fixture::snapshot(*project) != release_fixture::snapshot(expected))
        return juce::Result::fail(
            "Exchanged edits, routing, automation, MIDI, processor placeholders, scenes, or mastering data changed.");
    juce::WavAudioFormat format;
    auto input = media.createInputStream();
    if (input == nullptr)
        return juce::Result::fail("The exchanged synthetic media could not be opened.");
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(input.release(), true));
    if (reader == nullptr || reader->sampleRate != 48000.0 || reader->lengthInSamples != 6000
        || reader->numChannels != 2)
        return juce::Result::fail("The exchanged synthetic media format changed.");
    juce::AudioBuffer<float> audio(2, 6000);
    if (!reader->read(&audio, 0, audio.getNumSamples(), 0, true, true))
        return juce::Result::fail("The exchanged synthetic media could not be decoded.");
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto value = sample % 4 < 2 ? 0.125f : -0.125f;
        if (std::bit_cast<std::uint32_t>(audio.getSample(0, sample)) != std::bit_cast<std::uint32_t>(value)
            || std::bit_cast<std::uint32_t>(audio.getSample(1, sample)) != std::bit_cast<std::uint32_t>(-value))
            return juce::Result::fail("The exchanged synthetic PCM samples changed.");
    }
    const auto opened = studio::ProjectFile::loadForOpen(package, error);
    if (!opened || opened->recovered || opened->warning.isNotEmpty()
        || release_fixture::snapshot(opened->project) != release_fixture::snapshot(expected))
        return juce::Result::fail("An unchanged exchanged project unexpectedly required recovery: " + error);
    if (manifestFile.loadFileAsString() != manifestBytes || sessionFile.loadFileAsString() != sessionBytes)
        return juce::Result::fail("Opening the exchange fixture modified the source package.");
    return juce::Result::ok();
}
}

std::optional<int> runProjectExchangeFixture(const juce::StringArray& arguments)
{
    juce::String writePath;
    juce::String verifyPath;
    bool requested = false;
    for (const auto& argument : arguments)
    {
        if (argument.startsWith("--write-exchange-fixture="))
        {
            requested = true;
            writePath = argument.fromFirstOccurrenceOf("=", false, false);
        }
        if (argument.startsWith("--verify-exchange-fixture="))
        {
            requested = true;
            verifyPath = argument.fromFirstOccurrenceOf("=", false, false);
        }
    }
    if (!requested)
        return std::nullopt;
    const auto path = writePath.isNotEmpty() ? writePath : verifyPath;
    if (writePath.isNotEmpty() == verifyPath.isNotEmpty() || !juce::File::isAbsolutePath(path))
    {
        std::cerr << "Choose exactly one exchange operation with an absolute fixture package path.\n";
        return 2;
    }
    const juce::File package(path);
    if (writePath.isNotEmpty())
        if (const auto result = writeExchange(package); result.failed())
        {
            std::cerr << result.getErrorMessage() << '\n';
            return 3;
        }
    if (const auto result = verifyExchange(package); result.failed())
    {
        std::cerr << result.getErrorMessage() << '\n';
        return 4;
    }
    std::cout << "The complete synthetic project exchange fixture is valid.\n";
    return 0;
}

void projectExchangeTests()
{
    release_fixture::Directory directory;
    const auto source = directory.file.getChildFile("source.studioduo");
    const auto relocated = directory.file.getChildFile("relocated.studioduo");
    const auto written = writeExchange(source);
    expect(written.wasOk(), written.getErrorMessage().toRawUTF8());
    if (written.failed())
        return;
    const auto original = verifyExchange(source);
    expect(original.wasOk(), original.getErrorMessage().toRawUTF8());
    expect(source.copyDirectoryTo(relocated), "The complete synthetic exchange package can be relocated.");
    const auto transferred = verifyExchange(relocated);
    expect(transferred.wasOk(), transferred.getErrorMessage().toRawUTF8());
    expect(writeExchange(source).failed(), "Fixture creation never overwrites an existing project.");
    expect(relocated.getChildFile("media/synthetic-phase.wav").deleteFile(),
           "A missing-media exchange drill can remove its synthetic source.");
    const auto missing = verifyExchange(relocated);
    expect(missing.failed() && missing.getErrorMessage().contains("missing"),
           "Missing exchange media is an explicit failure, not a successful empty render.");
}
