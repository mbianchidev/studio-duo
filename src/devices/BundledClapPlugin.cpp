#include "AmpDeviceProcessor.h"
#include "DrumDeviceProcessor.h"
#include "plugin_host/ValidatedPluginStateTarget.h"

#include <clap/clap.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#if !defined(STUDIO_DUO_VERSION)
#define STUDIO_DUO_VERSION "0.0.0"
#endif

namespace
{
constexpr auto maximumParameterEvents = 1024;
constexpr auto maximumMidiEvents = 4096;
constexpr auto maximumStateBytes = 16U * 1024U * 1024U;

const char* drumFeatures[] {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_DRUM,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr
};

const char* guitarFeatures[] {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_DISTORTION,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr
};

const char* bassFeatures[] {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_DISTORTION,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr
};

const clap_plugin_descriptor_t descriptors[] {
    {
        CLAP_VERSION,
        "dev.mbianchi.studioduo.drum-composer",
        "Studio Duo Metal Drum Composer",
        "Studio Duo",
        "https://github.com/mbianchidev/studio-duo",
        "",
        "",
        STUDIO_DUO_VERSION,
        "Bundled deterministic metal drum instrument",
        drumFeatures
    },
    {
        CLAP_VERSION,
        "dev.mbianchi.studioduo.guitar-amp",
        "Studio Duo Guitar Amp",
        "Studio Duo",
        "https://github.com/mbianchidev/studio-duo",
        "",
        "",
        STUDIO_DUO_VERSION,
        "Bundled guitar amplifier and cabinet processor",
        guitarFeatures
    },
    {
        CLAP_VERSION,
        "dev.mbianchi.studioduo.bass-amp",
        "Studio Duo Bass Amp",
        "Studio Duo",
        "https://github.com/mbianchidev/studio-duo",
        "",
        "",
        STUDIO_DUO_VERSION,
        "Bundled bass amplifier and cabinet processor",
        bassFeatures
    }
};

struct ParameterEvent
{
    std::uint32_t time = 0;
    std::uint32_t index = 0;
    float value = 0.0f;
};

struct ClapDevice
{
    clap_plugin_t plugin {};
    const clap_host_t* host = nullptr;
    std::unique_ptr<juce::AudioProcessor> processor;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    juce::MidiBuffer midiSegment;
    std::array<ParameterEvent, maximumParameterEvents>
        parameterEvents {};
    int parameterEventCount = 0;
    int midiEventCount = 0;
    double sampleRate = 48000.0;
    std::uint32_t maximumFrames = 512;
    bool active = false;
};

ClapDevice* self(const clap_plugin_t* plugin)
{
    return static_cast<ClapDevice*>(plugin->plugin_data);
}

std::unique_ptr<juce::AudioProcessor> createProcessor(
    const char* pluginId)
{
    if (std::strcmp(pluginId, descriptors[0].id) == 0)
        return std::make_unique<studio::DrumDeviceProcessor>();
    if (std::strcmp(pluginId, descriptors[1].id) == 0)
        return std::make_unique<studio::AmpDeviceProcessor>(
            studio::AmpDeviceType::guitar);
    if (std::strcmp(pluginId, descriptors[2].id) == 0)
        return std::make_unique<studio::AmpDeviceProcessor>(
            studio::AmpDeviceType::bass);
    return {};
}

bool writeAll(const clap_ostream_t* stream,
              const void* data,
              std::uint64_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    auto written = std::uint64_t { 0 };
    while (written < size)
    {
        const auto result = stream->write(
            stream,
            bytes + written,
            size - written);
        if (result <= 0)
            return false;
        written += static_cast<std::uint64_t>(result);
    }
    return true;
}

bool readAll(const clap_istream_t* stream,
             void* data,
             std::uint64_t size)
{
    auto* bytes = static_cast<std::uint8_t*>(data);
    auto read = std::uint64_t { 0 };
    while (read < size)
    {
        const auto result = stream->read(
            stream,
            bytes + read,
            size - read);
        if (result <= 0)
            return false;
        read += static_cast<std::uint64_t>(result);
    }
    return true;
}

bool pluginInit(const clap_plugin_t*)
{
    return true;
}

void pluginDestroy(const clap_plugin_t* plugin)
{
    auto* instance = self(plugin);
    if (instance->active)
        instance->processor->releaseResources();
    delete instance;
}

bool pluginActivate(const clap_plugin_t* plugin,
                    double sampleRate,
                    std::uint32_t minimumFrames,
                    std::uint32_t maximumFrames)
{
    auto* instance = self(plugin);
    if (sampleRate <= 0.0
        || minimumFrames == 0
        || maximumFrames < minimumFrames
        || maximumFrames
            > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()))
        return false;
    instance->sampleRate = sampleRate;
    instance->maximumFrames = maximumFrames;
    instance->scratch.setSize(
        std::max({
            2,
            instance->processor->getTotalNumInputChannels(),
            instance->processor->getTotalNumOutputChannels()
        }),
        static_cast<int>(maximumFrames),
        false,
        true,
        false);
    instance->midi.ensureSize(65536);
    instance->midiSegment.ensureSize(65536);
    instance->processor->prepareToPlay(
        sampleRate,
        static_cast<int>(maximumFrames));
    instance->active = true;
    return true;
}

void pluginDeactivate(const clap_plugin_t* plugin)
{
    auto* instance = self(plugin);
    instance->processor->releaseResources();
    instance->active = false;
}

bool pluginStartProcessing(const clap_plugin_t*)
{
    return true;
}

void pluginStopProcessing(const clap_plugin_t*)
{
}

void pluginReset(const clap_plugin_t* plugin)
{
    self(plugin)->processor->reset();
}

bool addMidiEvent(ClapDevice& instance,
                  const clap_event_header_t& header)
{
    if (instance.midiEventCount >= maximumMidiEvents)
        return false;
    if (header.type == CLAP_EVENT_MIDI
        && header.size >= sizeof(clap_event_midi_t))
    {
        const auto& event = reinterpret_cast<
            const clap_event_midi_t&>(header);
        instance.midi.addEvent(
            juce::MidiMessage(event.data, 3),
            static_cast<int>(event.header.time));
        ++instance.midiEventCount;
        return true;
    }
    if ((header.type == CLAP_EVENT_NOTE_ON
         || header.type == CLAP_EVENT_NOTE_OFF)
        && header.size >= sizeof(clap_event_note_t))
    {
        const auto& event = reinterpret_cast<
            const clap_event_note_t&>(header);
        if (event.channel < 0 || event.channel > 15
            || event.key < 0 || event.key > 127)
            return true;
        const auto velocity = static_cast<juce::uint8>(
            juce::jlimit(
                0,
                127,
                static_cast<int>(
                    std::lround(event.velocity * 127.0))));
        const auto message = header.type == CLAP_EVENT_NOTE_ON
            ? juce::MidiMessage::noteOn(
                  event.channel + 1,
                  event.key,
                  velocity)
            : juce::MidiMessage::noteOff(
                  event.channel + 1,
                  event.key,
                  velocity);
        instance.midi.addEvent(
            message,
            static_cast<int>(event.header.time));
        ++instance.midiEventCount;
    }
    return true;
}

bool collectEvents(ClapDevice& instance,
                   const clap_input_events_t* input,
                   std::uint32_t frames)
{
    instance.parameterEventCount = 0;
    instance.midiEventCount = 0;
    instance.midi.clear();
    if (input == nullptr)
        return true;
    const auto count = input->size(input);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto* header = input->get(input, index);
        if (header == nullptr
            || header->time > frames
            || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        if (header->type == CLAP_EVENT_PARAM_VALUE
            && header->size
                >= sizeof(clap_event_param_value_t))
        {
            if (instance.parameterEventCount
                >= maximumParameterEvents)
                return false;
            const auto& event = reinterpret_cast<
                const clap_event_param_value_t&>(*header);
            if (event.param_id == CLAP_INVALID_ID
                || event.param_id == 0)
                continue;
            const auto parameterIndex =
                static_cast<std::uint32_t>(event.param_id - 1);
            if (parameterIndex
                >= static_cast<std::uint32_t>(
                    instance.processor->getParameters().size()))
                continue;
            auto& target = instance.parameterEvents[
                static_cast<std::size_t>(
                    instance.parameterEventCount++)];
            target.time = header->time;
            target.index = parameterIndex;
            target.value = static_cast<float>(
                juce::jlimit(0.0, 1.0, event.value));
            continue;
        }
        if (!addMidiEvent(instance, *header))
            return false;
    }
    return true;
}

void applyParameterEventsAt(ClapDevice& instance,
                            int& eventIndex,
                            std::uint32_t time)
{
    auto& parameters = instance.processor->getParameters();
    while (eventIndex < instance.parameterEventCount)
    {
        const auto& event = instance.parameterEvents[
            static_cast<std::size_t>(eventIndex)];
        if (event.time != time)
            break;
        parameters[static_cast<int>(event.index)]
            ->setValue(event.value);
        ++eventIndex;
    }
}

void processSegments(ClapDevice& instance,
                     int frames)
{
    if (instance.parameterEventCount == 0)
    {
        juce::AudioBuffer<float> view(
            instance.scratch.getArrayOfWritePointers(),
            instance.scratch.getNumChannels(),
            frames);
        instance.processor->processBlock(view, instance.midi);
        return;
    }

    auto cursor = 0;
    auto eventIndex = 0;
    while (cursor < frames)
    {
        applyParameterEventsAt(
            instance,
            eventIndex,
            static_cast<std::uint32_t>(cursor));
        const auto next = eventIndex < instance.parameterEventCount
            ? std::min(
                  frames,
                  static_cast<int>(
                      instance.parameterEvents[
                          static_cast<std::size_t>(eventIndex)]
                          .time))
            : frames;
        if (next <= cursor)
        {
            ++cursor;
            continue;
        }
        instance.midiSegment.clear();
        instance.midiSegment.addEvents(
            instance.midi,
            cursor,
            next - cursor,
            -cursor);
        juce::AudioBuffer<float> segment(
            instance.scratch.getArrayOfWritePointers(),
            instance.scratch.getNumChannels(),
            cursor,
            next - cursor);
        instance.processor->processBlock(
            segment,
            instance.midiSegment);
        cursor = next;
    }
    applyParameterEventsAt(
        instance,
        eventIndex,
        static_cast<std::uint32_t>(frames));
}

clap_process_status pluginProcess(const clap_plugin_t* plugin,
                                  const clap_process_t* process)
{
    auto* instance = self(plugin);
    if (!instance->active
        || process == nullptr
        || process->frames_count > instance->maximumFrames
        || !collectEvents(
            *instance,
            process->in_events,
            process->frames_count))
        return CLAP_PROCESS_ERROR;

    const auto frames = static_cast<int>(process->frames_count);
    instance->scratch.clear(0, frames);
    const auto inputBuses = std::min(
        process->audio_inputs_count,
        static_cast<std::uint32_t>(
            instance->processor->getBusCount(true)));
    for (std::uint32_t bus = 0; bus < inputBuses; ++bus)
    {
        const auto& source = process->audio_inputs[bus];
        if (source.data32 == nullptr)
            return CLAP_PROCESS_ERROR;
        const auto channels = std::min(
            source.channel_count,
            static_cast<std::uint32_t>(
                instance->processor->getChannelCountOfBus(
                    true,
                    static_cast<int>(bus))));
        for (std::uint32_t channel = 0; channel < channels; ++channel)
        {
            if (source.data32[channel] == nullptr)
                continue;
            const auto destination =
                instance->processor
                    ->getChannelIndexInProcessBlockBuffer(
                        true,
                        static_cast<int>(bus),
                        static_cast<int>(channel));
            instance->scratch.copyFrom(
                destination,
                0,
                source.data32[channel],
                frames);
        }
    }

    processSegments(*instance, frames);

    const auto outputBuses = std::min(
        process->audio_outputs_count,
        static_cast<std::uint32_t>(
            instance->processor->getBusCount(false)));
    for (std::uint32_t bus = 0; bus < outputBuses; ++bus)
    {
        auto& destination = process->audio_outputs[bus];
        if (destination.data32 == nullptr)
            return CLAP_PROCESS_ERROR;
        for (std::uint32_t channel = 0;
             channel < destination.channel_count;
             ++channel)
        {
            if (destination.data32[channel] == nullptr)
                continue;
            const auto busChannels =
                instance->processor->getChannelCountOfBus(
                    false,
                    static_cast<int>(bus));
            if (busChannels <= 0)
            {
                juce::FloatVectorOperations::clear(
                    destination.data32[channel],
                    frames);
                continue;
            }
            const auto source =
                instance->processor
                    ->getChannelIndexInProcessBlockBuffer(
                        false,
                        static_cast<int>(bus),
                        std::min(
                            static_cast<int>(channel),
                            busChannels - 1));
            juce::FloatVectorOperations::copy(
                destination.data32[channel],
                instance->scratch.getReadPointer(source),
                frames);
        }
    }
    for (auto bus = outputBuses;
         bus < process->audio_outputs_count;
         ++bus)
    {
        auto& destination = process->audio_outputs[bus];
        if (destination.data32 == nullptr)
            continue;
        for (std::uint32_t channel = 0;
             channel < destination.channel_count;
             ++channel)
        {
            if (destination.data32[channel] != nullptr)
                juce::FloatVectorOperations::clear(
                    destination.data32[channel],
                    frames);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

std::uint32_t audioPortsCount(const clap_plugin_t* plugin,
                              bool isInput)
{
    return static_cast<std::uint32_t>(
        self(plugin)->processor->getBusCount(isInput));
}

bool audioPortsGet(const clap_plugin_t* plugin,
                   std::uint32_t index,
                   bool isInput,
                   clap_audio_port_info_t* info)
{
    auto& processor = *self(plugin)->processor;
    if (info == nullptr
        || index
            >= static_cast<std::uint32_t>(
                processor.getBusCount(isInput)))
        return false;
    std::memset(info, 0, sizeof(*info));
    info->id = (isInput ? 100U : 200U) + index;
    const auto* bus = processor.getBus(
        isInput,
        static_cast<int>(index));
    const auto name = bus != nullptr
        ? bus->getName()
        : juce::String("Audio");
    std::snprintf(
        info->name,
        sizeof(info->name),
        "%s",
        name.toRawUTF8());
    info->flags = index == 0
        ? CLAP_AUDIO_PORT_IS_MAIN
        : 0;
    info->channel_count = static_cast<std::uint32_t>(
        processor.getChannelCountOfBus(
            isInput,
            static_cast<int>(index)));
    info->port_type = info->channel_count == 1
        ? CLAP_PORT_MONO
        : info->channel_count == 2 ? CLAP_PORT_STEREO : nullptr;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t audioPorts {
    audioPortsCount,
    audioPortsGet
};

std::uint32_t notePortsCount(const clap_plugin_t* plugin,
                             bool isInput)
{
    return isInput && self(plugin)->processor->acceptsMidi()
        ? 1U
        : 0U;
}

bool notePortsGet(const clap_plugin_t* plugin,
                  std::uint32_t index,
                  bool isInput,
                  clap_note_port_info_t* info)
{
    if (info == nullptr
        || index != 0
        || !isInput
        || !self(plugin)->processor->acceptsMidi())
        return false;
    std::memset(info, 0, sizeof(*info));
    info->id = 300;
    info->supported_dialects =
        CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::snprintf(info->name, sizeof(info->name), "%s", "MIDI Input");
    return true;
}

const clap_plugin_note_ports_t notePorts {
    notePortsCount,
    notePortsGet
};

std::uint32_t paramsCount(const clap_plugin_t* plugin)
{
    return static_cast<std::uint32_t>(
        self(plugin)->processor->getParameters().size());
}

bool paramsGetInfo(const clap_plugin_t* plugin,
                   std::uint32_t index,
                   clap_param_info_t* info)
{
    const auto& parameters = self(plugin)->processor->getParameters();
    if (info == nullptr
        || index >= static_cast<std::uint32_t>(parameters.size()))
        return false;
    std::memset(info, 0, sizeof(*info));
    const auto* parameter = parameters[static_cast<int>(index)];
    info->id = index + 1;
    info->flags = parameter->isAutomatable()
        ? CLAP_PARAM_IS_AUTOMATABLE
        : 0;
    info->cookie = const_cast<juce::AudioProcessorParameter*>(
        parameter);
    const auto name = parameter->getName(128);
    std::snprintf(
        info->name,
        sizeof(info->name),
        "%s",
        name.toRawUTF8());
    std::snprintf(
        info->module,
        sizeof(info->module),
        "%s",
        "Studio Duo");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = parameter->getDefaultValue();
    return true;
}

bool paramsGetValue(const clap_plugin_t* plugin,
                    clap_id parameterId,
                    double* value)
{
    const auto& parameters = self(plugin)->processor->getParameters();
    if (value == nullptr
        || parameterId == 0
        || parameterId > static_cast<clap_id>(parameters.size()))
        return false;
    *value = parameters[static_cast<int>(parameterId - 1)]
        ->getValue();
    return true;
}

bool paramsValueToText(const clap_plugin_t* plugin,
                       clap_id parameterId,
                       double value,
                       char* text,
                       std::uint32_t capacity)
{
    const auto& parameters = self(plugin)->processor->getParameters();
    if (text == nullptr
        || capacity == 0
        || parameterId == 0
        || parameterId > static_cast<clap_id>(parameters.size()))
        return false;
    const auto display =
        parameters[static_cast<int>(parameterId - 1)]
            ->getText(
                static_cast<float>(
                    juce::jlimit(0.0, 1.0, value)),
                static_cast<int>(capacity));
    std::snprintf(text, capacity, "%s", display.toRawUTF8());
    return true;
}

bool paramsTextToValue(const clap_plugin_t* plugin,
                       clap_id parameterId,
                       const char* text,
                       double* value)
{
    const auto& parameters = self(plugin)->processor->getParameters();
    if (text == nullptr
        || value == nullptr
        || parameterId == 0
        || parameterId > static_cast<clap_id>(parameters.size()))
        return false;
    *value = parameters[static_cast<int>(parameterId - 1)]
        ->getValueForText(juce::String::fromUTF8(text));
    return true;
}

void paramsFlush(const clap_plugin_t* plugin,
                 const clap_input_events_t* input,
                 const clap_output_events_t*)
{
    if (input == nullptr)
        return;
    auto& parameters = self(plugin)->processor->getParameters();
    const auto count = input->size(input);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto* header = input->get(input, index);
        if (header == nullptr
            || header->space_id != CLAP_CORE_EVENT_SPACE_ID
            || header->type != CLAP_EVENT_PARAM_VALUE
            || header->size < sizeof(clap_event_param_value_t))
            continue;
        const auto& event = reinterpret_cast<
            const clap_event_param_value_t&>(*header);
        if (event.param_id == 0
            || event.param_id
                > static_cast<clap_id>(parameters.size()))
            continue;
        parameters[static_cast<int>(event.param_id - 1)]
            ->setValue(static_cast<float>(
                juce::jlimit(0.0, 1.0, event.value)));
    }
}

const clap_plugin_params_t params {
    paramsCount,
    paramsGetInfo,
    paramsGetValue,
    paramsValueToText,
    paramsTextToValue,
    paramsFlush
};

bool stateSave(const clap_plugin_t* plugin,
               const clap_ostream_t* stream)
{
    if (stream == nullptr)
        return false;
    juce::MemoryBlock state;
    auto* processor = self(plugin)->processor.get();
    if (auto* validated =
            dynamic_cast<studio::ValidatedPluginStateTarget*>(
                processor))
    {
        if (validated->saveValidatedState(state).failed())
            return false;
    }
    else
    {
        processor->getStateInformation(state);
    }
    const auto size = static_cast<std::uint64_t>(state.getSize());
    return size > 0
        && size <= maximumStateBytes
        && writeAll(stream, &size, sizeof(size))
        && writeAll(stream, state.getData(), size);
}

bool stateLoad(const clap_plugin_t* plugin,
               const clap_istream_t* stream)
{
    if (stream == nullptr)
        return false;
    auto size = std::uint64_t { 0 };
    if (!readAll(stream, &size, sizeof(size))
        || size == 0
        || size > maximumStateBytes)
        return false;
    juce::MemoryBlock state(
        static_cast<std::size_t>(size),
        true);
    if (!readAll(stream, state.getData(), size))
        return false;
    auto* processor = self(plugin)->processor.get();
    if (auto* validated =
            dynamic_cast<studio::ValidatedPluginStateTarget*>(
                processor))
    {
        return validated->restoreValidatedState(
                   state.getData(),
                   static_cast<int>(state.getSize()))
            .wasOk();
    }
    processor->setStateInformation(
        state.getData(),
        static_cast<int>(state.getSize()));
    return true;
}

const clap_plugin_state_t state {
    stateSave,
    stateLoad
};

std::uint32_t latencyGet(const clap_plugin_t* plugin)
{
    return static_cast<std::uint32_t>(
        std::max(0, self(plugin)->processor->getLatencySamples()));
}

const clap_plugin_latency_t latency {
    latencyGet
};

std::uint32_t tailGet(const clap_plugin_t* plugin)
{
    const auto* instance = self(plugin);
    const auto samples = std::ceil(
        instance->processor->getTailLengthSeconds()
        * instance->sampleRate);
    return static_cast<std::uint32_t>(
        juce::jlimit(
            0.0,
            static_cast<double>(
                std::numeric_limits<std::uint32_t>::max() - 1U),
            samples));
}

const clap_plugin_tail_t tail {
    tailGet
};

const void* pluginGetExtension(const clap_plugin_t*,
                               const char* id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &notePorts;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &state;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &latency;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
        return &tail;
    return nullptr;
}

void pluginOnMainThread(const clap_plugin_t*)
{
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*,
                                  const clap_host_t* host,
                                  const char* pluginId)
{
    if (host == nullptr || pluginId == nullptr)
        return nullptr;
    auto processor = createProcessor(pluginId);
    if (processor == nullptr)
        return nullptr;
    const auto descriptor = std::find_if(
        std::begin(descriptors),
        std::end(descriptors),
        [pluginId](const auto& candidate)
        {
            return std::strcmp(candidate.id, pluginId) == 0;
        });
    if (descriptor == std::end(descriptors))
        return nullptr;

    auto instance = std::make_unique<ClapDevice>();
    instance->host = host;
    instance->processor = std::move(processor);
    instance->plugin.desc = &*descriptor;
    instance->plugin.plugin_data = instance.get();
    instance->plugin.init = pluginInit;
    instance->plugin.destroy = pluginDestroy;
    instance->plugin.activate = pluginActivate;
    instance->plugin.deactivate = pluginDeactivate;
    instance->plugin.start_processing = pluginStartProcessing;
    instance->plugin.stop_processing = pluginStopProcessing;
    instance->plugin.reset = pluginReset;
    instance->plugin.process = pluginProcess;
    instance->plugin.get_extension = pluginGetExtension;
    instance->plugin.on_main_thread = pluginOnMainThread;
    return &instance.release()->plugin;
}

std::uint32_t factoryCount(const clap_plugin_factory_t*)
{
    return static_cast<std::uint32_t>(std::size(descriptors));
}

const clap_plugin_descriptor_t* factoryDescriptor(
    const clap_plugin_factory_t*,
    std::uint32_t index)
{
    return index < std::size(descriptors)
        ? &descriptors[index]
        : nullptr;
}

const clap_plugin_factory_t factory {
    factoryCount,
    factoryDescriptor,
    createPlugin
};

bool entryInit(const char* pluginPath)
{
    return pluginPath != nullptr;
}

void entryDeinit()
{
}

const void* entryGetFactory(const char* factoryId)
{
    return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0
        ? &factory
        : nullptr;
}
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    entryInit,
    entryDeinit,
    entryGetFactory
};
