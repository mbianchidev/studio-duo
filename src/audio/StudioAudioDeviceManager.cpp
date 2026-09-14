#include "StudioAudioDeviceManager.h"

#include "AudioDeviceProbe.h"
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

#if JUCE_WINDOWS
std::vector<AudioDeviceTypeAvailability> deviceTypeAvailability(
    const juce::OwnedArray<juce::AudioIODeviceType>& deviceTypes)
{
    std::vector<AudioDeviceTypeAvailability> result;
    result.reserve(static_cast<std::size_t>(deviceTypes.size()));
    for (auto* type : deviceTypes)
    {
        const auto inputNames = type->getDeviceNames(true);
        const auto outputNames = type->getDeviceNames(false);
        result.push_back({
            type->getTypeName(),
            !inputNames.isEmpty(),
            !outputNames.isEmpty()
        });
        logDebug(
            "audio.discovery",
            type->getTypeName()
                + " inputs: "
                + (inputNames.isEmpty()
                       ? juce::String("none")
                       : inputNames.joinIntoString(", "))
                + "; outputs: "
                + (outputNames.isEmpty()
                       ? juce::String("none")
                       : outputNames.joinIntoString(", ")));
    }
    return result;
}
#endif

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
    const auto orderedNames = orderedAsioDeviceNames(deviceNames);
    return orderedNames.isEmpty() ? juce::String() : orderedNames[0];
}

juce::StringArray orderedAsioDeviceNames(
    const juce::StringArray& deviceNames,
    juce::String deviceNameToTryLast)
{
    juce::StringArray result;
    for (const auto& name : deviceNames)
    {
        if (name.isNotEmpty() && !isGenericAsioWrapper(name))
            result.add(name);
    }
    for (const auto& name : deviceNames)
    {
        if (name.isNotEmpty() && isGenericAsioWrapper(name))
            result.add(name);
    }
    const auto retryIndex = result.indexOf(deviceNameToTryLast, true);
    if (retryIndex >= 0)
        result.move(retryIndex, result.size() - 1);
    return result;
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

juce::String preferredAvailableAudioDeviceType(
    const juce::String& currentType,
    const std::vector<AudioDeviceTypeAvailability>& deviceTypes,
    bool deviceIsOpen)
{
    if (!deviceIsOpen)
        for (const auto& type : deviceTypes)
            if (type.typeName.equalsIgnoreCase("Windows Audio"))
                return type.typeName;

    for (const auto& type : deviceTypes)
    {
        if (type.typeName.equalsIgnoreCase(currentType)
            && type.hasInputDevices)
        {
            return type.typeName;
        }
    }

    for (const auto& type : deviceTypes)
    {
        if (type.hasInputDevices && type.hasOutputDevices)
            return type.typeName;
    }

    for (const auto& type : deviceTypes)
    {
        if (type.hasInputDevices)
            return type.typeName;
    }

    return currentType;
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

juce::Result probeNativeAudioDeviceSetup(const juce::XmlElement& setup)
{
#if JUCE_WINDOWS
    if (setup.getStringAttribute("deviceType") == midiDeviceDiscoveryProbeType)
    {
        logInfo("audio.probe", "Creating the audio manager and enumerating MIDI devices.");
        flushStudioLog();
        juce::AudioDeviceManager manager;
        const auto inputCount = juce::MidiInput::getAvailableDevices().size();
        const auto outputCount = juce::MidiOutput::getAvailableDevices().size();
        if (!juce::MessageManager::getInstance()->runDispatchLoopUntil(100))
            return juce::Result::fail("MIDI discovery stopped the probe event loop.");
        logInfo(
            "audio.probe",
            "MIDI discovery completed: " + juce::String(inputCount)
                + " inputs, " + juce::String(outputCount) + " outputs.");
        return juce::Result::ok();
    }
    if (!setup.getStringAttribute("deviceType").equalsIgnoreCase("ASIO"))
        return juce::Result::fail("The isolated audio probe received an unsupported device type.");
    auto asio = std::unique_ptr<juce::AudioIODeviceType>(
        juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    if (asio == nullptr)
        return juce::Result::fail("ASIO support is unavailable.");

    juce::AudioDeviceManager manager;
    manager.addAudioDeviceType(std::move(asio));
    logInfo("audio.probe", "Opening the requested ASIO setup in the worker.");
    flushStudioLog();
    const auto error = manager.initialise(1, 2, &setup, false);
    auto result = error.isNotEmpty()
        ? juce::Result::fail(error)
        : manager.getCurrentAudioDevice() == nullptr
            ? juce::Result::fail("The ASIO probe did not open a device.")
            : juce::Result::ok();
    if (result.wasOk()
        && !juce::MessageManager::getInstance()->runDispatchLoopUntil(100))
    {
        result = juce::Result::fail(
            "The ASIO driver stopped the probe event loop during startup.");
    }
    logInfo("audio.probe", "Closing the ASIO probe device.");
    flushStudioLog();
    manager.closeAudioDevice();
    return result;
#else
    juce::ignoreUnused(setup);
    return juce::Result::fail("Native audio startup checks are supported only on Windows.");
#endif
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
    logInfo("audio.startup", "Discovering audio devices and reading the saved setup.");
    flushStudioLog();
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
            const auto error = initialiseCheckedSetup(*saved);
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
#if JUCE_WINDOWS
            if (saved->getStringAttribute("deviceType")
                    .equalsIgnoreCase("ASIO"))
            {
                juce::StringArray asioDevices;
                for (auto* type : getAvailableDeviceTypes())
                {
                    if (type->getTypeName().equalsIgnoreCase("ASIO"))
                    {
                        asioDevices = type->getDeviceNames(false);
                        break;
                    }
                }
                const auto retryResult =
                    initialiseAsioDevices(
                        asioDevices,
                        audioDeviceSetupName(*saved));
                if (retryResult.wasOk())
                    return retryResult;
                return initialiseWindowsAudioFallback(
                    "Saved ASIO setup failed: "
                    + detail
                    + ". "
                    + retryResult.getErrorMessage());
            }
#endif
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
        const auto asioResult = initialiseAsioDevices(
            asioDevices,
            {});
        if (asioResult.wasOk())
            return asioResult;
        logResult(
            StudioLogLevel::error,
            asioResult.getErrorMessage());
        return initialiseWindowsAudioFallback(
            asioResult.getErrorMessage());
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

juce::String StudioAudioDeviceManager::initialiseCheckedSetup(
    const juce::XmlElement& setup)
{
#if JUCE_WINDOWS
    if (setup.getStringAttribute("deviceType").equalsIgnoreCase("ASIO"))
    {
        if (const auto result = probeAudioDeviceSetup(setup); result.failed())
            return result.getErrorMessage();
    }
#endif
    logInfo(
        "audio.startup",
        "Opening " + setup.getStringAttribute("deviceType")
            + " device " + audioDeviceSetupName(setup) + ".");
    flushStudioLog();
    return juce::AudioDeviceManager::initialise(1, 2, &setup, false);
}

#if JUCE_WINDOWS
juce::Result StudioAudioDeviceManager::initialiseAsioDevices(
    const juce::StringArray& deviceNames,
    const juce::String& deviceNameToTryLast)
{
    juce::StringArray failures;
    for (const auto& deviceName :
         orderedAsioDeviceNames(deviceNames, deviceNameToTryLast))
    {
        const auto setup = preferredAsioDeviceSetup(deviceName);
        juce::XmlElement xml("DEVICESETUP");
        xml.setAttribute("deviceType", "ASIO");
        xml.setAttribute("audioInputDeviceName", setup.inputDeviceName);
        xml.setAttribute("audioOutputDeviceName", setup.outputDeviceName);
        xml.setAttribute(
            "audioDeviceInChans",
            setup.inputChannels.toString(2));
        xml.setAttribute(
            "audioDeviceOutChans",
            setup.outputChannels.toString(2));

        const auto error = initialiseCheckedSetup(xml);
        if (error.isEmpty() && getCurrentAudioDevice() != nullptr)
        {
            logInfo(
                "audio.startup",
                "Opened ASIO device " + deviceName + ".");
            return juce::Result::ok();
        }

        const auto detail = error.isNotEmpty()
            ? error
            : juce::String("the device did not open");
        failures.add(deviceName + ": " + detail);
        logError(
            "audio.startup",
            "ASIO device " + deviceName + " failed: " + detail);
    }

    return juce::Result::fail(
        failures.isEmpty()
            ? juce::String("No ASIO drivers were found.")
            : "No ASIO driver could start. "
                + failures.joinIntoString("; "));
}

juce::Result StudioAudioDeviceManager::initialiseWindowsAudioFallback(
    const juce::String& asioFailure)
{
    addWindowsFallbackDeviceTypes();
    const auto& deviceTypes = getAvailableDeviceTypes();
    markDeviceTypesScanned();
    const auto availability = deviceTypeAvailability(deviceTypes);

    juce::String fallbackType;
    for (const auto& type : availability)
    {
        if (type.typeName.equalsIgnoreCase("Windows Audio")
            && type.hasInputDevices)
        {
            fallbackType = type.typeName;
            break;
        }
    }
    if (fallbackType.isEmpty())
    {
        for (const auto& type : availability)
        {
            if (!type.typeName.equalsIgnoreCase("ASIO")
                && type.hasInputDevices
                && type.hasOutputDevices)
            {
                fallbackType = type.typeName;
                break;
            }
        }
    }
    if (fallbackType.isEmpty())
    {
        return juce::Result::fail(
            asioFailure
            + ". No Windows audio input device was found. "
              "Check Windows microphone privacy settings and open Settings "
              "after reconnecting the device.");
    }

    setCurrentAudioDeviceType(fallbackType, false);
    const auto fallbackError = initialiseWithDefaultDevices(1, 2);
    if (fallbackError.isNotEmpty() || getCurrentAudioDevice() == nullptr)
    {
        const auto detail = fallbackError.isNotEmpty()
            ? fallbackError
            : juce::String("the fallback device did not open");
        return juce::Result::fail(
            asioFailure
            + ". "
            + fallbackType
            + " fallback also failed: "
            + detail
            + ". Open Settings to select another device.");
    }

    startupNotice =
        "ASIO could not start, so Studio Duo opened "
        + fallbackType
        + ". Open Settings to retry the preferred ASIO driver.";
    logInfo(
        "audio.startup",
        startupNotice + " ASIO detail: " + asioFailure);
    return juce::Result::ok();
}
#endif

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

juce::String StudioAudioDeviceManager::takeStartupNotice()
{
    auto notice = startupNotice;
    startupNotice.clear();
    return notice;
}

void StudioAudioDeviceManager::prepareDeviceTypesForSettings()
{
#if JUCE_WINDOWS
    getAvailableDeviceTypes();
    markDeviceTypesScanned();
    addWindowsFallbackDeviceTypes();
    const auto& deviceTypes = getAvailableDeviceTypes();
    for (auto* type : deviceTypes)
        type->scanForDevices();

    const auto availability = deviceTypeAvailability(deviceTypes);
    const auto currentType = getCurrentAudioDeviceType();
    auto* device = getCurrentAudioDevice();
    const auto deviceIsOpen = device != nullptr && device->isOpen();
    const auto preferredType = preferredAvailableAudioDeviceType(
        currentType,
        availability,
        deviceIsOpen);
    if (preferredType.isNotEmpty()
        && !preferredType.equalsIgnoreCase(currentType))
    {
        logInfo(
            "audio.discovery",
            !deviceIsOpen
                ? "No audio device is open; selecting " + preferredType + " for Settings."
                : "The " + currentType + " backend has no input devices; selecting "
                    + preferredType + " for Settings.");
        flushStudioLog();
        setCurrentAudioDeviceType(preferredType, false);
    }
#endif
}

void StudioAudioDeviceManager::addWindowsFallbackDeviceTypes()
{
#if JUCE_WINDOWS
    if (windowsFallbackDeviceTypesAdded)
        return;

    logInfo(
        "audio.discovery",
        "Preparing WASAPI and DirectSound device types.");
    flushStudioLog();
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
