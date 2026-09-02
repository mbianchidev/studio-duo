#include "ClapPluginInstance.h"

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace studio
{
namespace
{
juce::String pluginFile(const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.upToFirstOccurrenceOf("|", false, false);
}

juce::String pluginId(const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.fromFirstOccurrenceOf("|", false, false);
}

juce::File executableFor(const juce::File& plugin)
{
#if JUCE_MAC
    if (plugin.isDirectory())
    {
        const auto executableDirectory = plugin.getChildFile("Contents")
                                             .getChildFile("MacOS");
        const auto expected = executableDirectory.getChildFile(
            plugin.getFileNameWithoutExtension());
        if (expected.existsAsFile())
            return expected;
        juce::Array<juce::File> executables;
        executableDirectory.findChildFiles(
            executables,
            juce::File::findFiles,
            false);
        if (!executables.isEmpty())
            return executables.getFirst();
    }
#endif
    return plugin;
}

juce::AudioChannelSet channelSet(std::uint32_t channels)
{
    if (channels == 1)
        return juce::AudioChannelSet::mono();
    if (channels == 2)
        return juce::AudioChannelSet::stereo();
    return juce::AudioChannelSet::discreteChannels(
        static_cast<int>(channels));
}
}

class ClapPluginInstance::Impl
{
public:
    struct Module
    {
        ~Module()
        {
            if (initialized && entry != nullptr)
                entry->deinit();
        }

        static std::shared_ptr<Module> open(const juce::File& bundle,
                                            juce::String& error)
        {
            auto module = std::make_shared<Module>();
            module->bundle = bundle;
            const auto executable = executableFor(bundle);
            if (!executable.existsAsFile()
                || !module->library.open(executable.getFullPathName()))
            {
                error = "Could not load the CLAP module.";
                return {};
            }
            module->entry =
                reinterpret_cast<const clap_plugin_entry_t*>(
                    module->library.getFunction("clap_entry"));
            if (module->entry == nullptr
                || !clap_version_is_compatible(
                    module->entry->clap_version))
            {
                error = "The CLAP module has no compatible entry point.";
                return {};
            }
            if (!module->entry->init(bundle.getFullPathName().toRawUTF8()))
            {
                error = "The CLAP module failed to initialize.";
                return {};
            }
            module->initialized = true;
            module->factory =
                static_cast<const clap_plugin_factory_t*>(
                    module->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
            if (module->factory == nullptr)
            {
                error = "The CLAP module has no plugin factory.";
                return {};
            }
            return module;
        }

        juce::File bundle;
        juce::DynamicLibrary library;
        const clap_plugin_entry_t* entry = nullptr;
        const clap_plugin_factory_t* factory = nullptr;
        bool initialized = false;
    };

    struct Host
    {
        Host()
        {
            value = {
                CLAP_VERSION,
                this,
                "Studio Duo",
                "Studio Duo",
                "https://github.com/mbianchidev/studio-duo",
                STUDIO_DUO_VERSION,
                getExtension,
                requestRestart,
                requestProcess,
                requestCallback
            };
        }

        static const void* CLAP_ABI getExtension(
            const clap_host_t*,
            const char*)
        {
            return nullptr;
        }

        static void CLAP_ABI requestRestart(const clap_host_t* host)
        {
            static_cast<Host*>(host->host_data)
                ->restartRequested.store(true, std::memory_order_release);
        }

        static void CLAP_ABI requestProcess(const clap_host_t* host)
        {
            static_cast<Host*>(host->host_data)
                ->processRequested.store(true, std::memory_order_release);
        }

        static void CLAP_ABI requestCallback(const clap_host_t* host)
        {
            static_cast<Host*>(host->host_data)
                ->callbackRequested.store(true, std::memory_order_release);
        }

        clap_host_t value {};
        std::atomic<bool> restartRequested { false };
        std::atomic<bool> processRequested { false };
        std::atomic<bool> callbackRequested { false };
    };

    struct Port
    {
        juce::String name;
        std::uint32_t channels = 0;
        clap_id id = CLAP_INVALID_ID;
        clap_id inPlacePair = CLAP_INVALID_ID;
    };

    class Parameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        Parameter(const clap_param_info_t& value,
                  const clap_plugin_t* pluginToUse,
                  const clap_plugin_params_t* extension)
            : info(value),
              pluginInstance(pluginToUse),
              params(extension)
        {
            double initial = info.default_value;
            if (params != nullptr)
                params->get_value(pluginInstance, info.id, &initial);
            plainValue.store(initial, std::memory_order_relaxed);
        }

        float getValue() const override
        {
            const auto range = info.max_value - info.min_value;
            return range > 0.0
                ? static_cast<float>(
                      (plainValue.load(std::memory_order_relaxed)
                       - info.min_value)
                      / range)
                : 0.0f;
        }

        void setValue(float normalized) override
        {
            plainValue.store(
                info.min_value
                    + (info.max_value - info.min_value)
                        * juce::jlimit(0.0f, 1.0f, normalized),
                std::memory_order_relaxed);
        }

        float getDefaultValue() const override
        {
            const auto range = info.max_value - info.min_value;
            return range > 0.0
                ? static_cast<float>(
                      (info.default_value - info.min_value) / range)
                : 0.0f;
        }

        juce::String getName(int maximumLength) const override
        {
            return juce::String::fromUTF8(info.name)
                .substring(0, maximumLength);
        }

        juce::String getLabel() const override
        {
            return {};
        }

        float getValueForText(const juce::String& text) const override
        {
            auto plain = info.default_value;
            if (params == nullptr
                || !params->text_to_value(
                    pluginInstance,
                    info.id,
                    text.toRawUTF8(),
                    &plain))
                plain = text.getDoubleValue();
            const auto range = info.max_value - info.min_value;
            return range > 0.0
                ? static_cast<float>((plain - info.min_value) / range)
                : 0.0f;
        }

        juce::String getText(float normalized,
                             int maximumLength) const override
        {
            const auto plain = info.min_value
                + (info.max_value - info.min_value)
                    * juce::jlimit(0.0f, 1.0f, normalized);
            std::array<char, CLAP_NAME_SIZE> text {};
            if (params != nullptr
                && params->value_to_text(pluginInstance,
                                         info.id,
                                         plain,
                                         text.data(),
                                         text.size()))
                return juce::String::fromUTF8(text.data())
                    .substring(0, maximumLength);
            return juce::String(plain, 3).substring(0, maximumLength);
        }

        bool isAutomatable() const override
        {
            return (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        }

        bool isDiscrete() const override
        {
            return (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        }

        int getNumSteps() const override
        {
            return isDiscrete()
                ? static_cast<int>(info.max_value - info.min_value + 1.0)
                : juce::AudioProcessor::getDefaultNumParameterSteps();
        }

        juce::String getParameterID() const override
        {
            return juce::String(static_cast<juce::int64>(info.id));
        }

        [[nodiscard]] double plain() const noexcept
        {
            return plainValue.load(std::memory_order_relaxed);
        }

        void refresh()
        {
            double value = info.default_value;
            if (params != nullptr
                && params->get_value(pluginInstance, info.id, &value))
                plainValue.store(value, std::memory_order_relaxed);
        }

        clap_param_info_t info {};

    private:
        const clap_plugin_t* pluginInstance = nullptr;
        const clap_plugin_params_t* params = nullptr;
        std::atomic<double> plainValue { 0.0 };
    };

    struct InputEvents
    {
        InputEvents()
        {
            interface = { this, size, get };
        }

        static std::uint32_t CLAP_ABI size(
            const clap_input_events_t* list)
        {
            return static_cast<InputEvents*>(list->ctx)->count;
        }

        static const clap_event_header_t* CLAP_ABI get(
            const clap_input_events_t* list,
            std::uint32_t index)
        {
            const auto* events = static_cast<InputEvents*>(list->ctx);
            return index < events->count
                ? &events->values[index].header
                : nullptr;
        }

        clap_input_events_t interface {};
        std::array<clap_event_param_value_t, 256> values {};
        std::uint32_t count = 0;
    };

    struct OutputEvents
    {
        OutputEvents()
        {
            interface = { this, tryPush };
        }

        static bool CLAP_ABI tryPush(const clap_output_events_t*,
                                     const clap_event_header_t*)
        {
            return true;
        }

        clap_output_events_t interface {};
    };

    static std::unique_ptr<Impl> create(
        const juce::PluginDescription& description,
        juce::String& error)
    {
        auto implementation = std::make_unique<Impl>();
        implementation->description = description;
        implementation->module = Module::open(
            juce::File(pluginFile(description.fileOrIdentifier)),
            error);
        if (implementation->module == nullptr)
            return {};
        implementation->host = std::make_unique<Host>();
        const auto id = pluginId(description.fileOrIdentifier);
        if (id.isEmpty())
        {
            error = "The CLAP plugin identifier is missing.";
            return {};
        }
        implementation->plugin =
            implementation->module->factory->create_plugin(
                implementation->module->factory,
                &implementation->host->value,
                id.toRawUTF8());
        if (implementation->plugin == nullptr
            || !implementation->plugin->init(implementation->plugin))
        {
            error = "The CLAP plugin failed to initialize.";
            return {};
        }

        implementation->audioPorts =
            static_cast<const clap_plugin_audio_ports_t*>(
                implementation->plugin->get_extension(
                    implementation->plugin,
                    CLAP_EXT_AUDIO_PORTS));
        implementation->params =
            static_cast<const clap_plugin_params_t*>(
                implementation->plugin->get_extension(
                    implementation->plugin,
                    CLAP_EXT_PARAMS));
        implementation->state =
            static_cast<const clap_plugin_state_t*>(
                implementation->plugin->get_extension(
                    implementation->plugin,
                    CLAP_EXT_STATE));
        implementation->latency =
            static_cast<const clap_plugin_latency_t*>(
                implementation->plugin->get_extension(
                    implementation->plugin,
                    CLAP_EXT_LATENCY));
        implementation->tail =
            static_cast<const clap_plugin_tail_t*>(
                implementation->plugin->get_extension(
                    implementation->plugin,
                    CLAP_EXT_TAIL));
        implementation->scanPorts(true, implementation->inputs);
        implementation->scanPorts(false, implementation->outputs);
        return implementation;
    }

    ~Impl()
    {
        if (plugin != nullptr)
            plugin->destroy(plugin);
    }

    void scanPorts(bool input, std::vector<Port>& destination)
    {
        if (audioPorts == nullptr)
            return;
        const auto count = audioPorts->count(plugin, input);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            clap_audio_port_info_t info {};
            if (!audioPorts->get(plugin, index, input, &info))
                continue;
            destination.push_back({
                juce::String::fromUTF8(info.name),
                info.channel_count,
                info.id,
                info.in_place_pair
            });
        }
    }

    std::shared_ptr<Module> module;
    std::unique_ptr<Host> host;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_audio_ports_t* audioPorts = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_latency_t* latency = nullptr;
    const clap_plugin_tail_t* tail = nullptr;
    juce::PluginDescription description;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    std::vector<Parameter*> parameters;
    juce::AudioBuffer<float> inputScratch;
    InputEvents inputEvents;
    OutputEvents outputEvents;
    bool active = false;
    bool processing = false;
    double sampleRate = 48000.0;
    std::int64_t steadyTime = 0;
};

juce::AudioProcessor::BusesProperties ClapPluginInstance::busesFor(
    const Impl& impl)
{
    BusesProperties buses;
    for (const auto& input : impl.inputs)
        buses.addBus(true, input.name, channelSet(input.channels), true);
    for (const auto& output : impl.outputs)
        buses.addBus(false, output.name, channelSet(output.channels), true);
    return buses;
}

std::unique_ptr<ClapPluginInstance> ClapPluginInstance::create(
    const juce::PluginDescription& description,
    double sampleRate,
    int blockSize,
    juce::String& error)
{
    auto implementation = Impl::create(description, error);
    if (implementation == nullptr)
        return {};
    const auto buses = busesFor(*implementation);
    auto instance = std::unique_ptr<ClapPluginInstance>(
        new ClapPluginInstance(
            std::move(implementation),
            buses));
    instance->setRateAndBufferSizeDetails(sampleRate, blockSize);
    return instance;
}

ClapPluginInstance::ClapPluginInstance(
    std::unique_ptr<Impl> implementation,
    const BusesProperties& buses)
    : juce::AudioPluginInstance(buses),
      impl(std::move(implementation))
{
    if (impl->params == nullptr)
        return;
    const auto count = impl->params->count(impl->plugin);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        clap_param_info_t info {};
        if (!impl->params->get_info(impl->plugin, index, &info))
            continue;
        auto parameter = std::make_unique<Impl::Parameter>(
            info,
            impl->plugin,
            impl->params);
        impl->parameters.push_back(parameter.get());
        addHostedParameter(std::move(parameter));
    }
}

ClapPluginInstance::~ClapPluginInstance()
{
    releaseResources();
}

const juce::String ClapPluginInstance::getName() const
{
    return impl->description.name;
}

void ClapPluginInstance::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    releaseResources();
    impl->sampleRate = sampleRate;
    impl->inputScratch.setSize(
        std::max(1, getTotalNumInputChannels()),
        std::max(1, maximumBlockSize),
        false,
        true,
        false);
    impl->inputScratch.clear();
    if (!impl->plugin->activate(impl->plugin,
                                sampleRate,
                                1,
                                static_cast<std::uint32_t>(
                                    std::max(1, maximumBlockSize))))
        return;
    impl->active = true;
    impl->processing = impl->plugin->start_processing(impl->plugin);
    if (impl->latency != nullptr)
        setLatencySamples(static_cast<int>(
            impl->latency->get(impl->plugin)));
}

void ClapPluginInstance::releaseResources()
{
    if (impl == nullptr || impl->plugin == nullptr)
        return;
    if (impl->processing)
    {
        impl->plugin->stop_processing(impl->plugin);
        impl->processing = false;
    }
    if (impl->active)
    {
        impl->plugin->deactivate(impl->plugin);
        impl->active = false;
    }
}

void ClapPluginInstance::processBlock(juce::AudioBuffer<float>& audio,
                                      juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (!impl->active || !impl->processing)
    {
        audio.clear();
        return;
    }

    impl->inputEvents.count = static_cast<std::uint32_t>(
        std::min<std::size_t>(impl->parameters.size(),
                              impl->inputEvents.values.size()));
    for (std::uint32_t index = 0;
         index < impl->inputEvents.count;
         ++index)
    {
        const auto* parameter = impl->parameters[index];
        auto& event = impl->inputEvents.values[index];
        event = {};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.param_id = parameter->info.id;
        event.cookie = parameter->info.cookie;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = parameter->plain();
    }

    std::array<clap_audio_buffer_t, 16> inputBuffers {};
    std::array<clap_audio_buffer_t, 16> outputBuffers {};
    std::array<std::array<float*, 16>, 16> inputPointers {};
    std::array<std::array<float*, 16>, 16> outputPointers {};
    const auto inputCount = std::min<std::size_t>(
        impl->inputs.size(),
        inputBuffers.size());
    const auto outputCount = std::min<std::size_t>(
        impl->outputs.size(),
        outputBuffers.size());

    auto scratchChannel = 0;
    for (std::size_t bus = 0; bus < inputCount; ++bus)
    {
        const auto channels = std::min<int>(
            getChannelCountOfBus(true, static_cast<int>(bus)),
            static_cast<int>(inputPointers[bus].size()));
        const auto inputId = impl->inputs[bus].id;
        const auto inPlacePair = impl->inputs[bus].inPlacePair;
        const auto pairedOutput = inPlacePair
                != CLAP_INVALID_ID
            ? std::find_if(
                  impl->outputs.cbegin(),
                  impl->outputs.cend(),
                  [inputId, inPlacePair](const auto& output)
                  {
                      return output.id == inPlacePair
                          && output.inPlacePair == inputId;
                  })
            : impl->outputs.cend();
        const auto canProcessInPlace =
            pairedOutput != impl->outputs.cend();
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto bufferChannel = getChannelIndexInProcessBlockBuffer(
                true,
                static_cast<int>(bus),
                channel);
            if (canProcessInPlace)
            {
                inputPointers[bus][static_cast<std::size_t>(channel)] =
                    audio.getWritePointer(bufferChannel);
            }
            else
            {
                impl->inputScratch.copyFrom(
                    scratchChannel + channel,
                    0,
                    audio,
                    bufferChannel,
                    0,
                    audio.getNumSamples());
                inputPointers[bus][static_cast<std::size_t>(channel)] =
                    impl->inputScratch.getWritePointer(
                        scratchChannel + channel);
            }
        }
        scratchChannel += channels;
        inputBuffers[bus] = {
            inputPointers[bus].data(),
            nullptr,
            static_cast<std::uint32_t>(channels),
            0,
            0
        };
    }
    for (std::size_t bus = 0; bus < outputCount; ++bus)
    {
        const auto channels = std::min<int>(
            getChannelCountOfBus(false, static_cast<int>(bus)),
            static_cast<int>(outputPointers[bus].size()));
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto bufferChannel = getChannelIndexInProcessBlockBuffer(
                false,
                static_cast<int>(bus),
                channel);
            outputPointers[bus][static_cast<std::size_t>(channel)] =
                audio.getWritePointer(bufferChannel);
        }
        outputBuffers[bus] = {
            outputPointers[bus].data(),
            nullptr,
            static_cast<std::uint32_t>(channels),
            0,
            0
        };
    }

    clap_process_t process {
        impl->steadyTime,
        static_cast<std::uint32_t>(audio.getNumSamples()),
        nullptr,
        inputBuffers.data(),
        outputBuffers.data(),
        static_cast<std::uint32_t>(inputCount),
        static_cast<std::uint32_t>(outputCount),
        &impl->inputEvents.interface,
        &impl->outputEvents.interface
    };
    const auto status = impl->plugin->process(impl->plugin, &process);
    impl->steadyTime += audio.getNumSamples();
    if (status == CLAP_PROCESS_ERROR)
        audio.clear();
}

double ClapPluginInstance::getTailLengthSeconds() const
{
    if (impl->tail == nullptr)
        return 0.0;
    const auto samples = impl->tail->get(impl->plugin);
    if (samples >= static_cast<std::uint32_t>(INT32_MAX))
        return std::numeric_limits<double>::infinity();
    return static_cast<double>(samples) / impl->sampleRate;
}

bool ClapPluginInstance::acceptsMidi() const
{
    return false;
}

bool ClapPluginInstance::producesMidi() const
{
    return false;
}

juce::AudioProcessorEditor* ClapPluginInstance::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

bool ClapPluginInstance::hasEditor() const
{
    return true;
}

int ClapPluginInstance::getNumPrograms()
{
    return 1;
}

int ClapPluginInstance::getCurrentProgram()
{
    return 0;
}

void ClapPluginInstance::setCurrentProgram(int)
{
}

const juce::String ClapPluginInstance::getProgramName(int)
{
    return "Default";
}

void ClapPluginInstance::changeProgramName(int, const juce::String&)
{
}

void ClapPluginInstance::getStateInformation(
    juce::MemoryBlock& destination)
{
    destination.reset();
    if (impl->state == nullptr)
        return;
    juce::MemoryOutputStream output(destination, true);
    clap_ostream_t stream {
        &output,
        [](const clap_ostream_t* value,
           const void* data,
           std::uint64_t size) -> std::int64_t
        {
            auto* target = static_cast<juce::MemoryOutputStream*>(
                value->ctx);
            return target->write(data, static_cast<std::size_t>(size))
                ? static_cast<std::int64_t>(size)
                : -1;
        }
    };
    impl->state->save(impl->plugin, &stream);
}

void ClapPluginInstance::setStateInformation(const void* data, int size)
{
    if (impl->state == nullptr || data == nullptr || size <= 0)
        return;
    juce::MemoryInputStream input(data, static_cast<std::size_t>(size), false);
    clap_istream_t stream {
        &input,
        [](const clap_istream_t* value,
           void* destination,
           std::uint64_t bytes) -> std::int64_t
        {
            auto* source = static_cast<juce::MemoryInputStream*>(
                value->ctx);
            const auto requested = static_cast<int>(
                std::min<std::uint64_t>(bytes,
                                        static_cast<std::uint64_t>(INT_MAX)));
            return static_cast<std::int64_t>(
                source->read(destination, requested));
        }
    };
    if (impl->state->load(impl->plugin, &stream))
        for (auto* parameter : impl->parameters)
            parameter->refresh();
}

void ClapPluginInstance::fillInPluginDescription(
    juce::PluginDescription& destination) const
{
    destination = impl->description;
    destination.numInputChannels = getTotalNumInputChannels();
    destination.numOutputChannels = getTotalNumOutputChannels();
}

bool ClapPluginInstance::isBusesLayoutSupported(
    const BusesLayout& layouts) const
{
    if (layouts.inputBuses.size() != static_cast<int>(impl->inputs.size())
        || layouts.outputBuses.size()
            != static_cast<int>(impl->outputs.size()))
        return false;
    for (int index = 0; index < layouts.inputBuses.size(); ++index)
        if (layouts.inputBuses[index].size()
            != static_cast<int>(
                impl->inputs[static_cast<std::size_t>(index)].channels))
            return false;
    for (int index = 0; index < layouts.outputBuses.size(); ++index)
        if (layouts.outputBuses[index].size()
            != static_cast<int>(
                impl->outputs[static_cast<std::size_t>(index)].channels))
            return false;
    return true;
}
}
