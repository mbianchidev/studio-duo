#include "TestHarness.h"
#include "TestSuites.h"

#include "plugin_host/PluginCompatibilityValidator.h"
#include "plugin_host/ScreamForgeValidation.h"
#include "plugin_host/ClapPluginFormat.h"
#include "plugin_host/PluginFormats.h"
#include "audio/StudioAudioEngine.h"

void pluginCompatibilityTests()
{
    studio::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions,
                               STUDIO_DUO_CLAP_FIXTURE_PATH);
    expect(descriptions.size() == 1,
           "Compatibility fixture can be discovered.");
    if (descriptions.isEmpty())
        return;

    const auto report = studio::PluginCompatibilityValidator::validate(
        *descriptions[0]);
    expect(report.status == "pass"
               && std::all_of(
                   report.checks.cbegin(),
                   report.checks.cend(),
                   [](const auto& check)
                   {
                       return check.status == "pass"
                           || check.status == "skip";
                   }),
           "Public-standard compatibility validation passes the CLAP fixture.");
    juce::String error;
    const auto restored =
        studio::PluginValidationReport::fromVar(report.toVar(), error);
    expect(restored.has_value()
               && restored->pluginIdentifier == report.pluginIdentifier,
           "Compatibility reports are stable and serializable.");

    juce::AudioPluginFormatManager araFormats;
    studio::PluginFormats::addSupportedFormats(araFormats);
    juce::OwnedArray<juce::PluginDescription> araDescriptions;
    for (auto* pluginFormat : araFormats.getFormats())
    {
        if (pluginFormat->getName() == "VST3")
        {
            pluginFormat->findAllTypesForFile(
                araDescriptions,
                STUDIO_DUO_ARA_FIXTURE_PATH);
            break;
        }
    }
    expect(araDescriptions.size() == 1
               && araDescriptions[0]->hasARAExtension,
           "The public ARA fixture advertises ARA 2.");
    if (!araDescriptions.isEmpty())
    {
        const auto araReport =
            studio::PluginCompatibilityValidator::validate(
                *araDescriptions[0]);
        const auto passed = [&araReport](const juce::String& name)
        {
            const auto check = std::find_if(
                araReport.checks.cbegin(),
                araReport.checks.cend(),
                [&name](const auto& candidate)
                {
                    return candidate.name == name;
                });
            return check != araReport.checks.cend()
                && check->status == "pass";
        };
        expect(araReport.status == "pass"
                   && passed("ara2")
                   && passed("ara-audio-source")
                   && passed("ara-archive"),
               "ARA validation covers document binding, audio access, and archive restore.");

        const auto sourceFile = juce::File::getSpecialLocation(
                                    juce::File::tempDirectory)
                                    .getNonexistentChildFile(
                                        "StudioDuoAraLiveState",
                                        ".wav",
                                        false);
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream =
                sourceFile.createOutputStream();
            auto writer = wav.createWriterFor(
                stream,
                juce::AudioFormatWriterOptions {}
                    .withSampleRate(48000.0)
                    .withNumChannels(2)
                    .withBitsPerSample(24));
            juce::AudioBuffer<float> source(2, 480);
            source.clear();
            expect(writer != nullptr
                       && writer->writeFromAudioSampleBuffer(
                           source,
                           0,
                           source.getNumSamples()),
                   "ARA live-state source can be written.");
        }
        auto araProject = studio::Project::createDefault();
        studio::AudioClip clip;
        clip.sourceFile = sourceFile;
        clip.durationSeconds = 0.01;
        clip.sourceLengthSeconds = 0.01;
        clip.sourceRangeEndSeconds = 0.01;
        araProject.tracks.front().clips.push_back(clip);
        studio::PluginInsert araInsert;
        araInsert.id = juce::Uuid().toString();
        araInsert.name = "ARA Fixture";
        araInsert.pluginIdentifier =
            araDescriptions[0]->createIdentifierString();
        araInsert.format = "VST3";
        araInsert.bridgeMode =
            studio::PluginBridgeMode::araCompatibility;
        araInsert.araCapable = true;
        araProject.tracks.front().inserts.push_back(araInsert);

        studio::StudioAudioEngine araEngine;
        studio::StudioAudioEngine::PluginRuntimeRequest araRequest;
        araRequest.trackId = araProject.tracks.front().id;
        araRequest.insertId = araInsert.id;
        araRequest.name = araInsert.name;
        araRequest.description = *araDescriptions[0];
        araRequest.bridgeMode =
            studio::PluginBridgeMode::araCompatibility;
        araRequest.araDocument =
            studio::AraDocumentHost::describeProject(
                araProject,
                araRequest.trackId);
        expect(araEngine.updateProject(
                   araProject,
                   { araRequest })
                   .wasOk(),
               "ARA fixture runtime accepts project audio.");
        const auto waitForAraRuntime = [&araEngine]
        {
            for (int attempt = 0;
                 attempt < 500
                 && araEngine.pluginRuntimeTransitionPending();
                 ++attempt)
            {
                if (auto* messages =
                        juce::MessageManager::getInstanceWithoutCreating())
                {
                    messages->runDispatchLoopUntil(10);
                }
                else
                {
                    juce::Thread::sleep(10);
                }
            }
        };
        waitForAraRuntime();
        juce::String parameterError;
        expect(araEngine.setPluginParameter(
                   araInsert.id,
                   0,
                   0.25f,
                   parameterError),
               parameterError.toRawUTF8());

        araProject.tracks.front().clips.front().startSeconds = 0.25;
        araRequest.araDocument =
            studio::AraDocumentHost::describeProject(
                araProject,
                araRequest.trackId);
        expect(araEngine.updateProject(
                   araProject,
                   { araRequest })
                   .wasOk(),
               "ARA graph changes schedule a state-preserving rebuild.");
        waitForAraRuntime();
        const auto rebuiltStatuses = araEngine.pluginRuntimeStatuses();
        const auto preservedParameter = !rebuiltStatuses.empty()
            && !rebuiltStatuses.front().parameters.empty()
            ? rebuiltStatuses.front().parameters.front().value
            : -1.0f;
        const auto liveCaptures = araEngine.capturePluginStates(5000);
        juce::MemoryBlock processorState;
        juce::MemoryBlock araState;
        const auto unpacked = !liveCaptures.empty()
            && studio::AraDocumentHost::unpackState(
                liveCaptures.front().state,
                processorState,
                araState);
        expect(!liveCaptures.empty()
                   && liveCaptures.front().result.wasOk()
                   && std::abs(preservedParameter - 0.25f) < 0.0001f
                   && unpacked
                   && !processorState.isEmpty()
                   && !araState.isEmpty(),
               "ARA graph rebuilds preserve unsaved processor and document state.");
        if (!liveCaptures.empty()
            && liveCaptures.front().result.wasOk())
        {
            araRequest.state = liveCaptures.front().state;
        }
        juce::AudioBuffer<float> araRender;
        expect(araEngine.renderToBuffer(
                   araProject,
                   araRender,
                   48000.0,
                   { araRequest })
                   .wasOk(),
               "ARA processors render while message-thread creation remains responsive.");
        araEngine.shutdown();
        sourceFile.deleteFile();
    }

    juce::PluginDescription screamForge;
    screamForge.name = "Scream Forge";
    screamForge.manufacturerName = "Studio Duo";
    screamForge.pluginFormatName = "VST3";
    expect(studio::ScreamForgeValidation::matches(screamForge),
           "Scream Forge selection uses public plugin metadata.");
    const auto unavailable =
        studio::ScreamForgeValidation::validateInstalled({});
    expect(unavailable.status == "not-installed",
           "Absent Scream Forge builds report not-installed explicitly.");
}
