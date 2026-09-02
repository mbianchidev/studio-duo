#include "PluginCompatibilityValidator.h"

#include "AraDocumentHost.h"
#include "ClapPluginInstance.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace studio
{
namespace
{
const juce::DynamicObject* objectFor(const juce::var& value,
                                     juce::String& error,
                                     const juce::String& type)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        error = type + " must be a JSON object.";
    return object;
}

void addCheck(PluginValidationReport& report,
              juce::String name,
              juce::String status,
              juce::String message)
{
    report.checks.push_back({
        std::move(name),
        std::move(status),
        std::move(message)
    });
}

bool finiteAudio(const juce::AudioBuffer<float>& audio)
{
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            if (!std::isfinite(audio.getSample(channel, sample)))
                return false;
    return true;
}

juce::File createAraValidationSource(juce::String& error)
{
    const auto file = juce::File::getSpecialLocation(
                          juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoAraValidation",
                              ".wav",
                              false);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream =
        file.createOutputStream();
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(2)
            .withBitsPerSample(24));
    if (writer == nullptr)
    {
        error = "Could not create the ARA validation source.";
        return {};
    }
    juce::AudioBuffer<float> source(2, 4800);
    source.clear();
    source.setSample(0, 0, 0.5f);
    source.setSample(1, 0, 0.5f);
    if (!writer->writeFromAudioSampleBuffer(
            source,
            0,
            source.getNumSamples()))
    {
        error = "Could not write the ARA validation source.";
        return {};
    }
    return file;
}

PluginValidationCheck platformValidation(
    const juce::PluginDescription& description)
{
#if JUCE_MAC
    if (description.pluginFormatName == "AudioUnit")
    {
        juce::ChildProcess process;
        juce::StringArray arguments {
            "/usr/bin/auval",
            "-a"
        };
        if (!process.start(arguments))
            return { "platform-validator", "skip", "auval is unavailable" };
        if (!process.waitForProcessToFinish(15000))
        {
            process.kill();
            return { "platform-validator", "fail", "auval timed out" };
        }
        const auto output = process.readAllProcessOutput();
        return output.containsIgnoreCase(description.name)
            ? PluginValidationCheck {
                  "platform-validator",
                  "pass",
                  "Audio Unit is listed by auval"
              }
            : PluginValidationCheck {
                  "platform-validator",
                  "fail",
                  "Audio Unit was not listed by auval"
              };
    }
#endif
    if (description.pluginFormatName == "VST3")
    {
        juce::StringArray candidates;
        if (const auto* configured = std::getenv("VST3_VALIDATOR"))
            candidates.add(juce::String::fromUTF8(configured));
#if JUCE_MAC
        candidates.add("/opt/homebrew/bin/validator");
        candidates.add("/usr/local/bin/validator");
#endif
        auto validator = juce::String();
        for (const auto& candidate : candidates)
        {
            if (juce::File(candidate).existsAsFile())
            {
                validator = candidate;
                break;
            }
        }
        if (validator.isEmpty())
            return {
                "platform-validator",
                "skip",
                "Steinberg VST3 validator is not installed"
            };
        juce::ChildProcess process;
        const auto pluginFile =
            description.fileOrIdentifier.upToFirstOccurrenceOf(
                "|",
                false,
                false);
        juce::StringArray arguments {
            validator,
            pluginFile
        };
        if (!process.start(arguments))
            return {
                "platform-validator",
                "fail",
                "Could not launch Steinberg VST3 validator"
            };
        if (!process.waitForProcessToFinish(60000))
        {
            process.kill();
            return {
                "platform-validator",
                "fail",
                "Steinberg VST3 validator timed out"
            };
        }
        return process.getExitCode() == 0
            ? PluginValidationCheck {
                  "platform-validator",
                  "pass",
                  "Steinberg VST3 validator passed"
              }
            : PluginValidationCheck {
                  "platform-validator",
                  "fail",
                  process.readAllProcessOutput().substring(0, 1000)
              };
    }
    return {
        "platform-validator",
        "skip",
        "No supplemental validator applies to this format"
    };
}
}

juce::var PluginValidationCheck::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("name", name);
    object->setProperty("status", status);
    object->setProperty("message", message);
    return juce::var(object.release());
}

std::optional<PluginValidationCheck> PluginValidationCheck::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = objectFor(value, error, "Validation check");
    if (object == nullptr)
        return std::nullopt;
    PluginValidationCheck check;
    check.name = object->getProperty("name").toString();
    check.status = object->getProperty("status").toString();
    check.message = object->getProperty("message").toString();
    if (check.name.isEmpty() || check.status.isEmpty())
    {
        error = "Validation checks require a name and status.";
        return std::nullopt;
    }
    return check;
}

juce::var PluginValidationReport::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("schemaVersion", 1);
    object->setProperty("pluginIdentifier", pluginIdentifier);
    object->setProperty("name", name);
    object->setProperty("format", format);
    object->setProperty("version", version);
    object->setProperty("status", status);
    object->setProperty("createdAt", createdAt);
    object->setProperty("araCapable", araCapable);
    juce::Array<juce::var> checkValues;
    for (const auto& check : checks)
        checkValues.add(check.toVar());
    object->setProperty("checks", juce::var(checkValues));
    return juce::var(object.release());
}

std::optional<PluginValidationReport> PluginValidationReport::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = objectFor(value, error, "Validation report");
    if (object == nullptr
        || static_cast<int>(object->getProperty("schemaVersion")) != 1
        || !object->getProperty("checks").isArray())
    {
        if (error.isEmpty())
            error = "Validation report schema is unsupported.";
        return std::nullopt;
    }
    PluginValidationReport report;
    report.pluginIdentifier =
        object->getProperty("pluginIdentifier").toString();
    report.name = object->getProperty("name").toString();
    report.format = object->getProperty("format").toString();
    report.version = object->getProperty("version").toString();
    report.status = object->getProperty("status").toString();
    report.createdAt = object->getProperty("createdAt").toString();
    report.araCapable = static_cast<bool>(
        object->getProperty("araCapable"));
    for (const auto& checkValue : *object->getProperty("checks").getArray())
    {
        auto check = PluginValidationCheck::fromVar(checkValue, error);
        if (!check.has_value())
            return std::nullopt;
        report.checks.push_back(std::move(*check));
    }
    if (report.pluginIdentifier.isEmpty()
        || report.name.isEmpty()
        || report.format.isEmpty()
        || report.status.isEmpty())
    {
        error = "Validation report identity is incomplete.";
        return std::nullopt;
    }
    return report;
}

PluginValidationReport PluginCompatibilityValidator::validate(
    const juce::PluginDescription& description)
{
    PluginValidationReport report;
    report.pluginIdentifier = description.createIdentifierString();
    report.name = description.name;
    report.format = description.pluginFormatName;
    report.version = description.version;
    report.createdAt = juce::Time::getCurrentTime().toISO8601(true);
    report.araCapable = description.hasARAExtension;
    addCheck(report,
             "descriptor",
             description.name.isNotEmpty()
                     && description.pluginFormatName.isNotEmpty()
                 ? "pass"
                 : "fail",
             "Public plugin descriptor");

    const auto createInstance = [&description](
                                    juce::String& error)
        -> std::unique_ptr<juce::AudioPluginInstance>
    {
        if (description.pluginFormatName == "CLAP")
        {
            return ClapPluginInstance::create(
                description,
                48000.0,
                256,
                error);
        }
        juce::AudioPluginFormatManager formats;
        PluginFormats::addSupportedFormats(formats);
        return formats.createPluginInstance(
            description,
            48000.0,
            256,
            error);
    };

    juce::String error;
    auto instance = createInstance(error);
    if (instance == nullptr)
    {
        addCheck(report,
                 "instantiate",
                 "fail",
                 error.isNotEmpty() ? error : "Instantiation failed");
        report.status = "fail";
        return report;
    }
    addCheck(report, "instantiate", "pass", "Instance created");

    std::unique_ptr<AraDocumentHost> araHost;
    std::shared_ptr<const AraDocumentDescriptor> araDescriptor;
    auto araValidationSource = juce::File();
    if (description.hasARAExtension)
    {
        araValidationSource = createAraValidationSource(error);
        if (araValidationSource.existsAsFile())
        {
            auto araProject = Project::createDefault();
            AudioClip clip;
            clip.name = "ARA validation";
            clip.sourceFile = araValidationSource;
            clip.durationSeconds = 0.1;
            clip.sourceLengthSeconds = 0.1;
            clip.sourceRangeEndSeconds = 0.1;
            araProject.tracks.front().clips.push_back(clip);
            araDescriptor = AraDocumentHost::describeProject(
                araProject,
                araProject.tracks.front().id);
            araHost = std::make_unique<AraDocumentHost>();
            const auto result = araHost->bind(
                *instance,
                araDescriptor);
            addCheck(report,
                     "ara2",
                     result.wasOk() ? "pass" : "fail",
                     result.wasOk()
                         ? "ARA document bound in compatibility mode"
                         : result.getErrorMessage());
            addCheck(
                report,
                "ara-audio-source",
                result.wasOk()
                        && araHost->audioSourceCount() == 1
                        && araHost->playbackRegionCount() == 1
                    ? "pass"
                    : "fail",
                result.wasOk()
                    ? "Project audio source and playback region registered"
                    : "ARA source registration unavailable");

            juce::MemoryBlock archive;
            const auto archiveResult = result.wasOk()
                ? araHost->archive(archive)
                : result;
            auto restoreResult = juce::Result::fail(
                "ARA archive was unavailable.");
            if (archiveResult.wasOk() && !archive.isEmpty())
            {
                juce::String restoreError;
                auto restoredInstance = createInstance(restoreError);
                if (restoredInstance != nullptr)
                {
                    AraDocumentHost restoredHost;
                    restoreResult = restoredHost.bind(
                        *restoredInstance,
                        araDescriptor,
                        archive);
                    restoredInstance->releaseResources();
                }
                else
                {
                    restoreResult = juce::Result::fail(
                        restoreError);
                }
            }
            addCheck(
                report,
                "ara-archive",
                archiveResult.wasOk()
                        && !archive.isEmpty()
                        && restoreResult.wasOk()
                    ? "pass"
                    : "fail",
                archiveResult.failed()
                    ? archiveResult.getErrorMessage()
                    : restoreResult.failed()
                        ? restoreResult.getErrorMessage()
                        : "ARA document archive restored");
        }
        else
        {
            addCheck(report, "ara2", "fail", error);
            addCheck(
                report,
                "ara-audio-source",
                "fail",
                "ARA validation source unavailable");
            addCheck(
                report,
                "ara-archive",
                "fail",
                "ARA validation source unavailable");
        }
    }

    instance->prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> audio(
        std::max({
            2,
            instance->getTotalNumInputChannels(),
            instance->getTotalNumOutputChannels()
        }),
        256);
    audio.clear();
    for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
        audio.setSample(channel, 0, 0.5f);
    juce::MidiBuffer midi;
    instance->processBlock(audio, midi);
    addCheck(report,
             "process",
             finiteAudio(audio) ? "pass" : "fail",
             finiteAudio(audio)
                 ? "Finite impulse response"
                 : "Processor emitted non-finite audio");

    addCheck(report,
             "parameters",
             "pass",
             juce::String(instance->getParameters().size())
                 + " parameters enumerated");

    juce::MemoryBlock state;
    instance->getStateInformation(state);
    if (state.isEmpty())
    {
        addCheck(report,
                 "state",
                 "skip",
                 "Processor returned empty opaque state");
    }
    else
    {
        instance->setStateInformation(
            state.getData(),
            static_cast<int>(state.getSize()));
        juce::MemoryBlock restored;
        instance->getStateInformation(restored);
        addCheck(report,
                 "state",
                 restored == state ? "pass" : "fail",
                 restored == state
                     ? "Opaque state round-trip"
                     : "Opaque state changed after restore");
    }

    addCheck(report,
             "latency-tail",
             instance->getLatencySamples() >= 0
                     && instance->getTailLengthSeconds() >= 0.0
                 ? "pass"
                 : "fail",
             juce::String(instance->getLatencySamples())
                 + " samples, "
                 + juce::String(instance->getTailLengthSeconds(), 3)
                 + " seconds");

    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && instance->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(
            instance->createEditorAndMakeActive());
        addCheck(report,
                 "editor",
                 editor != nullptr ? "pass" : "fail",
                 editor != nullptr
                     ? "Editor created"
                     : "Editor creation failed");
    }
    else
    {
        addCheck(report,
                 "editor",
                 "skip",
                 "No message loop or editor available");
    }

    if (!description.hasARAExtension)
    {
        addCheck(report, "ara2", "skip", "Plugin does not advertise ARA 2");
    }

    report.checks.push_back(platformValidation(description));

    instance->releaseResources();
    araHost.reset();
    if (araValidationSource.existsAsFile())
        araValidationSource.deleteFile();
    report.status = std::any_of(
        report.checks.cbegin(),
        report.checks.cend(),
        [](const auto& check)
        {
            return check.status == "fail";
        })
        ? "fail"
        : "pass";
    return report;
}
}
