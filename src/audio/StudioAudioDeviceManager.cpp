#include "StudioAudioDeviceManager.h"

#include "logging/StudioLogger.h"

namespace studio
{
namespace
{
juce::File defaultAudioSettingsFile()
{
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Studio Duo")
        .getChildFile("audio-device.xml");
}

bool isGenericAsioWrapper(const juce::String& name)
{
    return name.containsIgnoreCase("asio4all")
        || name.containsIgnoreCase("generic low latency asio")
        || name.containsIgnoreCase("steinberg built-in asio")
        || name.containsIgnoreCase("fl studio asio")
        || name.containsIgnoreCase("flexasio");
}

juce::Result writeTextAtomically(
    const juce::File& destination,
    const juce::String& text)
{
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail(
            "Could not create the audio settings directory.");

    const auto temporary = destination.getSiblingFile(
        destination.getFileName()
            + ".tmp-"
            + juce::Uuid().toString());
    if (!temporary.replaceWithText(text, false, false, "\n"))
        return juce::Result::fail(
            "Could not write the temporary audio settings file.");

    const auto replaced = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);
    if (!replaced)
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not publish the audio settings file.");
    }
    return juce::Result::ok();
}

std::unique_ptr<juce::XmlElement> currentSetupXml(
    const StudioAudioDeviceManager& manager)
{
    if (auto state = manager.createStateXml())
        return state;

    const auto setup = manager.getAudioDeviceSetup();
    auto xml = std::make_unique<juce::XmlElement>("DEVICESETUP");
    xml->setAttribute(
        "deviceType",
        manager.getCurrentAudioDeviceType());
    xml->setAttribute(
        "audioOutputDeviceName",
        setup.outputDeviceName);
    xml->setAttribute(
        "audioInputDeviceName",
        setup.inputDeviceName);

    if (auto* device = manager.getCurrentAudioDevice())
    {
        xml->setAttribute(
            "audioDeviceRate",
            device->getCurrentSampleRate());
        xml->setAttribute(
            "audioDeviceBufferSize",
            device->getCurrentBufferSizeSamples());
    }
    if (!setup.useDefaultInputChannels)
        xml->setAttribute(
            "audioDeviceInChans",
            setup.inputChannels.toString(2));
    if (!setup.useDefaultOutputChannels)
        xml->setAttribute(
            "audioDeviceOutChans",
            setup.outputChannels.toString(2));
    return xml;
}
}

juce::String preferredAsioDeviceName(
    const juce::StringArray& deviceNames)
{
    for (const auto& name : deviceNames)
    {
        if (name.isNotEmpty() && !isGenericAsioWrapper(name))
            return name;
    }

    return deviceNames.isEmpty() ? juce::String() : deviceNames[0];
}

juce::AudioDeviceManager::AudioDeviceSetup
preferredAsioDeviceSetup(const juce::String& deviceName)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = deviceName;
    setup.outputDeviceName = deviceName;
    setup.inputChannels.setRange(
        0,
        maximumHardwareAudioChannels,
        true);
    setup.outputChannels.setRange(0, 2, true);
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    return setup;
}

int callbackChannelIndex(
    const juce::BigInteger& activeChannels,
    int physicalChannel) noexcept
{
    if (physicalChannel < 0 || !activeChannels[physicalChannel])
        return -1;

    auto callbackChannel = 0;
    for (auto channel = 0; channel < physicalChannel; ++channel)
        if (activeChannels[channel])
            ++callbackChannel;
    return callbackChannel;
}

StudioAudioDeviceManager::StudioAudioDeviceManager()
    : settingsFile(defaultAudioSettingsFile())
{
#if JUCE_WINDOWS
    if (auto asio = std::unique_ptr<juce::AudioIODeviceType>(
            juce::AudioIODeviceType::createAudioIODeviceType_ASIO()))
    {
        addAudioDeviceType(std::move(asio));
    }
    else
    {
        addWindowsFallbackDeviceTypes();
    }
#endif
}

juce::Result StudioAudioDeviceManager::initialiseStudioAudio()
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto logResult = [started](
                               StudioLogLevel level,
                               const juce::String& message)
    {
        const auto timedMessage =
            message
            + " ("
            + juce::String(
                  juce::Time::getMillisecondCounterHiRes()
                      - started,
                  1)
            + " ms)";
        if (level == StudioLogLevel::error)
            logError("audio.startup", timedMessage);
        else
            logInfo("audio.startup", timedMessage);
    };

    if (settingsFile.existsAsFile())
    {
        auto saved = juce::parseXML(settingsFile);
        if (saved == nullptr || !saved->hasTagName("DEVICESETUP"))
        {
            logError(
                "audio.settings",
                "Ignored corrupt audio-device.xml.");
        }
        else
        {
#if JUCE_WINDOWS
            if (!saved->getStringAttribute("deviceType")
                     .equalsIgnoreCase("ASIO"))
            {
                addWindowsFallbackDeviceTypes();
            }
#endif
            getAvailableDeviceTypes();
            markDeviceTypesScanned();
            const auto error = juce::AudioDeviceManager::initialise(
                1,
                2,
                saved.get(),
                false);
            if (error.isEmpty() && getCurrentAudioDevice() != nullptr)
            {
                logResult(
                    StudioLogLevel::info,
                    "restored "
                    + getCurrentAudioDeviceType()
                    + " device "
                    + getCurrentAudioDevice()->getName());
                return juce::Result::ok();
            }

            const auto detail = error.isNotEmpty()
                ? error
                : juce::String("the saved device did not open");
            logResult(
                StudioLogLevel::error,
                "Saved device failed: " + detail);
            return juce::Result::fail(
                "Saved audio device setup failed: "
                + detail
                + ". Open I/O to select or reset the device.");
        }
    }

#if JUCE_WINDOWS
    const auto& deviceTypes = getAvailableDeviceTypes();
    markDeviceTypesScanned();
    auto* asioType = [&]() -> juce::AudioIODeviceType*
    {
        for (auto* type : deviceTypes)
            if (type->getTypeName().equalsIgnoreCase("ASIO"))
                return type;
        return nullptr;
    }();

    const auto asioDevices = asioType != nullptr
        ? asioType->getDeviceNames(false)
        : juce::StringArray();
    logDebug(
        "audio.discovery",
        asioDevices.isEmpty()
            ? "No ASIO drivers found."
            : "ASIO drivers: "
                + asioDevices.joinIntoString(", "));
    const auto asioDeviceName =
        preferredAsioDeviceName(asioDevices);
    if (asioDeviceName.isNotEmpty())
    {
        const auto setup = preferredAsioDeviceSetup(asioDeviceName);
        const auto error = juce::AudioDeviceManager::initialise(
            1,
            2,
            nullptr,
            false,
            {},
            &setup);
        if (error.isNotEmpty())
        {
            logResult(
                StudioLogLevel::error,
                "ASIO device "
                + asioDeviceName
                + " failed: "
                + error);
            return juce::Result::fail(
                "ASIO device "
                + asioDeviceName
                + " failed to start: "
                + error
                + ". Open I/O to select another driver or reset the device.");
        }

        logResult(
            StudioLogLevel::info,
            "Opened ASIO device " + asioDeviceName);
        return juce::Result::ok();
    }

    addWindowsFallbackDeviceTypes();
    const auto error = initialiseWithDefaultDevices(1, 2);
#else
    const auto error = initialiseWithDefaultDevices(1, 2);
#endif
    if (error.isNotEmpty())
    {
        logResult(
            StudioLogLevel::error,
            "Default device failed: " + error);
        return juce::Result::fail(
            "Audio device setup failed: "
            + error
            + ". Open I/O to select another device.");
    }

    if (getCurrentAudioDevice() == nullptr)
    {
        logResult(
            StudioLogLevel::error,
            "No default device opened.");
        return juce::Result::fail(
            "No audio device could be opened. Open I/O to select a device.");
    }

    logResult(
        StudioLogLevel::info,
        "opened "
        + getCurrentAudioDeviceType()
        + " device "
        + getCurrentAudioDevice()->getName());
    return juce::Result::ok();
}

juce::Result StudioAudioDeviceManager::saveCurrentSetup() const
{
    if (getCurrentAudioDevice() == nullptr)
        return juce::Result::fail(
            "No open audio device is available to save.");

    const auto result = writeTextAtomically(
        settingsFile,
        currentSetupXml(*this)->toString());
    if (result.wasOk())
    {
        logDebug(
            "audio.settings",
            "Saved "
                + getCurrentAudioDeviceType()
                + " setup for "
                + getCurrentAudioDevice()->getName()
                + ".");
    }
    return result;
}

void StudioAudioDeviceManager::prepareDeviceTypesForSettings()
{
#if JUCE_WINDOWS
    addWindowsFallbackDeviceTypes();
#endif
}

void StudioAudioDeviceManager::addWindowsFallbackDeviceTypes()
{
#if JUCE_WINDOWS
    if (windowsFallbackDeviceTypesAdded)
        return;

    logDebug(
        "audio.discovery",
        "Preparing WASAPI and DirectSound device types.");
    const auto addType = [this](juce::AudioIODeviceType* rawType)
    {
        auto type = std::unique_ptr<juce::AudioIODeviceType>(rawType);
        if (type == nullptr)
            return;
        if (deviceTypesScanned)
            type->scanForDevices();
        addAudioDeviceType(std::move(type));
    };
    addType(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
        juce::WASAPIDeviceMode::shared));
    addType(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
        juce::WASAPIDeviceMode::exclusive));
    addType(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(
        juce::WASAPIDeviceMode::sharedLowLatency));
    addType(
        juce::AudioIODeviceType::createAudioIODeviceType_DirectSound());
    windowsFallbackDeviceTypesAdded = true;
#endif
}

void StudioAudioDeviceManager::markDeviceTypesScanned() noexcept
{
    deviceTypesScanned = true;
}
}
