#include "TestHarness.h"
#include "TestSuites.h"

#include "devices/DeviceRegistry.h"
#include "audio/StudioAudioEngine.h"
#include "model/ProjectModel.h"

namespace
{
void setParameter(juce::AudioProcessor& processor,
                  const juce::String& id,
                  float normalized)
{
    for (auto* parameter : processor.getParameters())
    {
        const auto* identified =
            dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter);
        if (identified != nullptr && identified->paramID == id)
        {
            parameter->setValueNotifyingHost(normalized);
            return;
        }
    }
}

juce::AudioBuffer<float> constantBuffer(float value, int samples = 256)
{
    juce::AudioBuffer<float> buffer(2, samples);
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < samples; ++sample)
            buffer.setSample(channel, sample, value);
    return buffer;
}
}

void deviceTests()
{
    const auto descriptors = studio::DeviceRegistry::descriptors();
    expect(descriptors.size() == 10,
           "The bundled registry contains all ten Phase 3 utility devices.");

    juce::MidiBuffer midi;
    auto gain = studio::DeviceRegistry::create("studio.device.gain");
    expect(gain != nullptr, "Gain device can be created.");
    gain->prepareToPlay(48000.0, 256);
    setParameter(*gain, "gain", 0.75f);
    auto gainAudio = constantBuffer(0.25f);
    gain->processBlock(gainAudio, midi);
    expect(gainAudio.getSample(0, 32) > 0.45f,
           "Gain device applies positive gain.");

    auto polarity = studio::DeviceRegistry::create("studio.device.polarity");
    polarity->prepareToPlay(48000.0, 256);
    setParameter(*polarity, "invert", 1.0f);
    auto polarityAudio = constantBuffer(0.25f);
    polarity->processBlock(polarityAudio, midi);
    expect(polarityAudio.getSample(0, 32) < -0.24f,
           "Polarity device inverts signal.");

    auto delay = studio::DeviceRegistry::create("studio.device.delay");
    delay->prepareToPlay(48000.0, 256);
    setParameter(*delay, "samples", 10.0f / 96000.0f);
    juce::AudioBuffer<float> delayAudio(2, 64);
    delayAudio.clear();
    delayAudio.setSample(0, 0, 1.0f);
    delayAudio.setSample(1, 0, 1.0f);
    delay->processBlock(delayAudio, midi);
    expect(std::abs(delayAudio.getSample(0, 10) - 1.0f) < 0.0001f,
           "Delay device moves an impulse by the requested samples.");

    auto gate = studio::DeviceRegistry::create("studio.device.gate");
    gate->prepareToPlay(48000.0, 256);
    auto gateAudio = constantBuffer(0.00001f);
    gate->processBlock(gateAudio, midi);
    expect(std::abs(gateAudio.getSample(0, 200)) < 0.000001f,
           "Noise gate closes on sub-threshold signal.");

    auto compressor = studio::DeviceRegistry::create("studio.device.compressor");
    compressor->prepareToPlay(48000.0, 256);
    auto transparentAudio = constantBuffer(0.1f);
    compressor->processBlock(transparentAudio, midi);
    expect(transparentAudio.getSample(0, 0) > 0.095f,
           "Compressor reset starts at unity gain.");
    compressor->reset();
    auto compressorAudio = constantBuffer(0.9f);
    compressor->processBlock(compressorAudio, midi);
    expect(compressorAudio.getSample(0, 200) < 0.8f,
           "Compressor reduces high-level signal.");
    auto compressorLayout = compressor->getBusesLayout();
    compressorLayout.inputBuses.set(1, juce::AudioChannelSet::stereo());
    expect(compressor->setBusesLayout(compressorLayout),
           "Compressor enables a stereo sidechain bus.");
    compressor->reset();
    juce::AudioBuffer<float> sidechainAudio(4, 256);
    for (int sample = 0; sample < sidechainAudio.getNumSamples(); ++sample)
    {
        sidechainAudio.setSample(0, sample, 0.1f);
        sidechainAudio.setSample(1, sample, 0.1f);
        sidechainAudio.setSample(2, sample, 1.0f);
        sidechainAudio.setSample(3, sample, 1.0f);
    }
    compressor->processBlock(sidechainAudio, midi);
    expect(sidechainAudio.getSample(0, 200) < 0.09f,
           "Compressor responds to its external sidechain.");

    auto limiter = studio::DeviceRegistry::create("studio.device.limiter");
    limiter->prepareToPlay(48000.0, 256);
    auto limiterAudio = constantBuffer(1.5f);
    limiter->processBlock(limiterAudio, midi);
    expect(std::abs(limiterAudio.getSample(0, 200)) <= 0.99f,
           "True-peak limiter respects its ceiling.");
    auto limiterWhole = studio::DeviceRegistry::create("studio.device.limiter");
    auto limiterSplit = studio::DeviceRegistry::create("studio.device.limiter");
    limiterWhole->prepareToPlay(48000.0, 512);
    limiterSplit->prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> whole(2, 512);
    juce::AudioBuffer<float> firstHalf(2, 256);
    juce::AudioBuffer<float> secondHalf(2, 256);
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < 512; ++sample)
            whole.setSample(channel, sample, sample < 128 ? 1.5f : 0.2f);
        firstHalf.copyFrom(channel, 0, whole, channel, 0, 256);
        secondHalf.copyFrom(channel, 0, whole, channel, 256, 256);
    }
    limiterWhole->processBlock(whole, midi);
    limiterSplit->processBlock(firstHalf, midi);
    limiterSplit->processBlock(secondHalf, midi);
    expect(std::abs(whole.getSample(0, 256)
                    - secondHalf.getSample(0, 0))
               < 0.0001f,
           "Limiter release state is independent of host block boundaries.");

    auto equalizer = studio::DeviceRegistry::create("studio.device.eq");
    equalizer->prepareToPlay(48000.0, 256);
    auto eqAudio = constantBuffer(0.2f);
    setParameter(*equalizer, "midGain", 1.0f);
    equalizer->processBlock(eqAudio, midi);
    expect(std::isfinite(eqAudio.getSample(0, 200))
               && std::abs(eqAudio.getSample(0, 200) - 0.2f) > 0.0001f,
           "Parametric EQ performs finite filtering.");

    auto reverb = studio::DeviceRegistry::create("studio.device.reverb");
    reverb->prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> reverbAudio(2, 256);
    reverbAudio.clear();
    reverbAudio.setSample(0, 0, 1.0f);
    reverbAudio.setSample(1, 0, 1.0f);
    reverb->processBlock(reverbAudio, midi);
    auto tailEnergy = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        reverbAudio.clear();
        reverb->processBlock(reverbAudio, midi);
        for (int sample = 0; sample < reverbAudio.getNumSamples(); ++sample)
            tailEnergy += std::abs(reverbAudio.getSample(0, sample));
    }
    expect(tailEnergy > 0.01f,
           "Algorithmic reverb produces a tail.");

    auto generator = studio::DeviceRegistry::create("studio.device.generator");
    generator->prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> generatedA(2, 256);
    generatedA.clear();
    generator->processBlock(generatedA, midi);
    generator->reset();
    juce::AudioBuffer<float> generatedB(2, 256);
    generatedB.clear();
    generator->processBlock(generatedB, midi);
    expect(std::abs(generatedA.getSample(0, 100)
                    - generatedB.getSample(0, 100))
               < 0.000001f,
           "Signal generator reset is deterministic.");

    auto tuner = studio::DeviceRegistry::create("studio.device.tuner");
    tuner->prepareToPlay(48000.0, 2048);
    juce::AudioBuffer<float> tunerAudio(2, 2048);
    for (int sample = 0; sample < tunerAudio.getNumSamples(); ++sample)
    {
        const auto signal = static_cast<float>(
            std::sin(juce::MathConstants<double>::twoPi
                     * 440.0
                     * static_cast<double>(sample)
                     / 48000.0));
        tunerAudio.setSample(0, sample, signal);
        tunerAudio.setSample(1, sample, signal);
    }
    tuner->processBlock(tunerAudio, midi);
    expect(std::abs(studio::DeviceRegistry::meterValue(*tuner, "frequency")
                    - 440.0)
               < 5.0,
           "Tuner detects a 440 Hz test tone.");

    juce::MemoryBlock state;
    compressor->getStateInformation(state);
    expect(!state.isEmpty(),
           "Bundled device state is persistent.");

    auto project = studio::Project::createDefault();
    studio::StudioAudioEngine engine;
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = project.tracks.front().id;
    request.insertId = juce::Uuid().toString();
    request.name = "Gain";
    request.deviceIdentifier = "studio.device.gain";
    request.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    expect(engine.updateProject(project, { request }).wasOk(),
           "Bundled device runtime request is accepted.");
    for (int attempt = 0;
         attempt < 100 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    const auto statuses = engine.pluginRuntimeStatuses();
    expect(!statuses.empty()
               && statuses.front().state
                      == studio::StudioAudioEngine::PluginRuntimeStatus::State::ready
               && statuses.front().message.containsIgnoreCase("device"),
           "Bundled devices activate in the standard insert runtime.");

    studio::PluginInsert generatorInsert;
    generatorInsert.pluginIdentifier = "studio.device.generator";
    generatorInsert.name = "Signal Generator";
    generatorInsert.format = "Studio Duo";
    generatorInsert.bundledDevice = true;
    generatorInsert.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    auto renderProject = studio::Project::createDefault();
    renderProject.tracks.front().inserts.push_back(generatorInsert);
    studio::StudioAudioEngine::PluginRuntimeRequest generatorRequest;
    generatorRequest.trackId = renderProject.tracks.front().id;
    generatorRequest.insertId = generatorInsert.id;
    generatorRequest.name = generatorInsert.name;
    generatorRequest.deviceIdentifier = generatorInsert.pluginIdentifier;
    generatorRequest.bridgeMode =
        studio::PluginBridgeMode::trustedInProcess;
    juce::AudioBuffer<float> rendered;
    expect(engine.renderToBuffer(
               renderProject,
               rendered,
               48000.0,
               { generatorRequest })
               .wasOk(),
           "Bundled devices render through the playback processor graph.");
    expect(rendered.getMagnitude(0, 0, 512) > 0.01f,
           "Plugin-inclusive rendering includes bundled DSP output.");
}
