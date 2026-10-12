#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/AudioDeviceProbe.h"

#include <cstdlib>
#include <thread>

std::optional<int> runAudioDeviceProbeFixture(
    const juce::StringArray& arguments)
{
    return studio::runAudioDeviceProbeWorker(
        arguments,
        [](const juce::XmlElement& setup)
        {
            if (setup.getStringAttribute("deviceType") == studio::midiDeviceDiscoveryProbeType)
                return juce::Result::ok();
            const auto device = setup.getStringAttribute(
                "audioOutputDeviceName");
            if (device == "Fixture abrupt exit")
                std::_Exit(42);
            if (device == "Fixture missing response")
                std::_Exit(0);
            if (device == "Fixture timeout")
                juce::Thread::sleep(10000);
            if (device == "Fixture unavailable")
                return juce::Result::fail("Fixture device is disconnected.");
            if (setup.getDoubleAttribute("audioDeviceRate") != 48000.0
                || setup.getStringAttribute("audioDeviceInChans") != "1111")
            {
                return juce::Result::fail(
                    "The probe did not preserve the requested device setup.");
            }
            return juce::Result::ok();
        });
}

void audioDeviceProbeTests()
{
    auto invoked = false;
    const auto ordinaryLaunch = studio::runAudioDeviceProbeWorker(
        { "--safe-audio" },
        [&invoked](const juce::XmlElement&)
        {
            invoked = true;
            return juce::Result::ok();
        });
    expect(
        !ordinaryLaunch.has_value() && !invoked,
        "An ordinary launch never runs the audio probe worker.");

    juce::XmlElement setup("DEVICESETUP");
    setup.setAttribute("deviceType", "ASIO");
    setup.setAttribute("audioInputDeviceName", "Fixture ready");
    setup.setAttribute("audioOutputDeviceName", "Fixture ready");
    setup.setAttribute("audioDeviceRate", 48000.0);
    setup.setAttribute("audioDeviceInChans", "1111");

    studio::AudioDeviceProbeOptions options;
    options.executable = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    juce::XmlElement discovery("DEVICESETUP");
    discovery.setAttribute("deviceType", studio::midiDeviceDiscoveryProbeType);
    expect(
        studio::probeAudioDeviceSetup(discovery, options).wasOk(),
        "MIDI discovery can be checked without selecting or opening an audio device.");
    expect(
        studio::audioDeviceSetupName(discovery).isEmpty(),
        "A discovery-only request has no audio device name.");
    juce::XmlElement legacySetup("DEVICESETUP");
    legacySetup.setAttribute("audioDeviceName", "Fixture legacy");
    expect(
        studio::audioDeviceSetupName(legacySetup) == "Fixture legacy"
            && studio::audioDeviceSetupName(setup) == "Fixture ready",
        "Startup diagnostics identify both legacy and separate-I/O saved setups.");
    const auto ready = studio::probeAudioDeviceSetup(setup, options);
    expect(
        ready.wasOk(),
        ("A clean worker exit validates the requested audio setup: "
         + ready.getErrorMessage()).toRawUTF8());

    setup.setAttribute("audioOutputDeviceName", "Fixture unavailable");
    const auto unavailable = studio::probeAudioDeviceSetupDetailed(setup, options);
    expect(
        unavailable.result.failed()
            && unavailable.failure == studio::AudioDeviceProbeFailure::deviceUnavailable
            && unavailable.result.getErrorMessage().contains("disconnected"),
        "A driver error is returned to the host without opening it there.");

    setup.setAttribute("audioOutputDeviceName", "Fixture abrupt exit");
    const auto crashed = studio::probeAudioDeviceSetupDetailed(setup, options);
    expect(
        crashed.result.failed()
            && crashed.failure == studio::AudioDeviceProbeFailure::crash
            && crashed.result.getErrorMessage().contains("0x2a"),
        "An abrupt worker exit is contained and reports its native exit code.");

    setup.setAttribute("audioOutputDeviceName", "Fixture missing response");
    const auto missingResponse =
        studio::probeAudioDeviceSetupDetailed(setup, options);
    expect(
        missingResponse.result.failed() && missingResponse.failure == studio::AudioDeviceProbeFailure::protocol,
        "Exit code zero without a completed probe response is not success.");

    setup.setAttribute("audioOutputDeviceName", "Fixture timeout");
    options.timeoutMs = 250;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto timedOut = studio::probeAudioDeviceSetupDetailed(setup, options);
    expect(
        timedOut.result.failed()
            && timedOut.failure == studio::AudioDeviceProbeFailure::timeout
            && timedOut.result.getErrorMessage().contains("timed out")
            && juce::Time::getMillisecondCounterHiRes() - started < 5000.0,
        "A hung driver is terminated within the bounded startup deadline.");

    options.timeoutMs = 10000;
    options.cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        const std::jthread cancellation([flag = options.cancelled]
        {
            juce::Thread::sleep(100);
            flag->store(true, std::memory_order_release);
        });
        const auto cancelStarted = juce::Time::getMillisecondCounterHiRes();
        const auto cancelled = studio::probeAudioDeviceSetupDetailed(setup, options);
        expect(cancelled.result.failed() && cancelled.failure == studio::AudioDeviceProbeFailure::cancelled
                   && juce::Time::getMillisecondCounterHiRes() - cancelStarted < 3000.0,
               "User cancellation terminates the isolated worker and is not reported as a timeout or crash.");
    }
    options.cancelled.reset();

    options.timeoutMs = 0;
    expect(
        studio::probeAudioDeviceSetup(setup, options).failed(),
        "An invalid timeout cannot start an unbounded audio probe.");

    options.timeoutMs = 10000;
    options.executable = options.executable.getSiblingFile(
        "missing-studio-duo-probe-executable");
    expect(
        studio::probeAudioDeviceSetup(setup, options).failed(),
        "A missing worker executable never falls back to unsafe in-process probing.");

#if defined(STUDIO_DUO_BRIDGE_WORKER_PATH)
    studio::AudioDeviceProbeOptions applicationOptions;
    applicationOptions.executable = juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH);
    juce::XmlElement unsupportedSetup("DEVICESETUP");
    unsupportedSetup.setAttribute("deviceType", "Fixture unsupported backend");
    const auto applicationResult =
        studio::probeAudioDeviceSetup(unsupportedSetup, applicationOptions);
    expect(
        applicationResult.failed()
            && applicationResult.getErrorMessage().contains(
#if JUCE_WINDOWS
                "unsupported device type"
#else
                "supported only on Windows"
#endif
            ),
        "The production executable dispatches probe requests without constructing its window or loading drivers.");
#endif
}
