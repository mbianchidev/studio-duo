#include "TestHarness.h"
#include "TestSuites.h"

#include "plugin_host/PluginFormats.h"
#include "plugin_host/ClapPluginFormat.h"
#include "model/ProjectCommands.h"
#include "audio/StudioAudioEngine.h"

void pluginFormatTests()
{
    juce::AudioPluginFormatManager manager;
    studio::PluginFormats::addSupportedFormats(manager);

    juce::StringArray names;
    for (auto* format : manager.getFormats())
        names.add(format->getName());
    expect(names.contains("VST3"),
           "Supported plugin formats include VST3.");
#if JUCE_MAC
    expect(names.contains("AudioUnit"),
           "Supported plugin formats include Audio Units on macOS.");
#endif
    expect(names.contains("CLAP"),
           "Supported plugin formats include CLAP.");

    studio::ClapPluginFormat clapFormat;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    clapFormat.findAllTypesForFile(descriptions,
                                   STUDIO_DUO_CLAP_FIXTURE_PATH);
    expect(descriptions.size() == 1
               && descriptions[0]->pluginFormatName == "CLAP",
           "CLAP scanning reads public descriptors from a bundle.");
    if (descriptions.isEmpty())
        return;

    juce::String error;
    auto instance = clapFormat.createInstanceFromDescription(
        *descriptions[0],
        48000.0,
        64,
        error);
    expect(instance != nullptr, error.toRawUTF8());
    if (instance == nullptr)
        return;

    instance->prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 64);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            audio.setSample(channel, sample, 1.0f);
    juce::MidiBuffer midi;
    instance->processBlock(audio, midi);
    expect(std::abs(audio.getSample(0, 32) - 0.5f) < 0.0001f,
           "CLAP instances process audio.");

    expect(instance->getParameters().size() == 1,
           "CLAP parameters are exposed to the host.");
    juce::MemoryBlock state;
    instance->getStateInformation(state);
    instance->getParameters()[0]->setValueNotifyingHost(0.25f);
    instance->setStateInformation(state.getData(),
                                  static_cast<int>(state.getSize()));
    expect(std::abs(instance->getParameters()[0]->getValue() - 0.5f)
               < 0.0001f,
           "CLAP opaque state restores parameter values.");
    instance->releaseResources();

    auto project = studio::Project::createDefault();
    studio::PluginInsert insert;
    insert.pluginIdentifier = "ara-mode-test";
    insert.name = "ARA Mode Test";
    insert.format = "VST3";
    insert.araCapable = true;
    project.tracks.front().inserts.push_back(insert);
    studio::CommandStack history;
    error.clear();
    expect(history.perform(
               std::make_unique<studio::SetPluginBridgeModeCommand>(
                   project.tracks.front().id,
                   insert.id,
                   studio::PluginBridgeMode::araCompatibility),
               project,
               error),
           error.toRawUTF8());
    expect(project.tracks.front().inserts.front().bridgeMode
               == studio::PluginBridgeMode::araCompatibility,
           "ARA compatibility mode is a typed project edit.");
    expect(history.undo(project)
               && project.tracks.front().inserts.front().bridgeMode
                      == studio::PluginBridgeMode::sandboxed,
           "Plugin isolation mode changes are undoable.");

    studio::StudioAudioEngine engine;
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = project.tracks.front().id;
    request.insertId = insert.id;
    request.name = insert.name;
    request.description = *descriptions[0];
    request.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    expect(engine.updateProject(project, { request }).wasOk(),
           "Trusted plugin runtime request is accepted.");
    for (int attempt = 0;
         attempt < 100 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    const auto statuses = engine.pluginRuntimeStatuses();
    expect(!statuses.empty()
               && statuses.front().state
                      == studio::StudioAudioEngine::PluginRuntimeStatus::State::ready
               && statuses.front().message.containsIgnoreCase("in-process"),
           "Trusted CLAP processing activates in process.");
}
