#include "ClapPluginInstance.h"

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
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
    static constexpr std::size_t maximumScheduledAutomationEvents =
        static_cast<std::size_t>(
            PluginBridgeSharedState::maxParameterEvents)
        * 4;

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
        explicit Host(Impl& implementation)
            : owner(&implementation)
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
            const clap_host_t* host,
            const char* id)
        {
            if (id == nullptr)
                return nullptr;
            if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
                return &threadCheck;
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
                return &hostParams;
            if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
                return &hostLatency;
            if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
                return &hostAudioPorts;
            if (std::strcmp(id, CLAP_EXT_STATE) == 0)
                return &hostState;
            juce::ignoreUnused(host);
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

        static bool CLAP_ABI isMainThread(const clap_host_t*)
        {
            const auto* messages =
                juce::MessageManager::getInstanceWithoutCreating();
            return messages != nullptr
                && messages->isThisTheMessageThread()
                && audioThreadDepth == 0;
        }

        static bool CLAP_ABI isAudioThread(const clap_host_t*)
        {
            return audioThreadDepth > 0;
        }

        static void CLAP_ABI rescanParams(const clap_host_t* host,
                                          clap_param_rescan_flags flags)
        {
            auto& owner = *static_cast<Host*>(host->host_data)->owner;
            owner.parameterRescanFlags.fetch_or(
                flags,
                std::memory_order_release);
        }

        static void CLAP_ABI clearParam(const clap_host_t* host,
                                        clap_id,
                                        clap_param_clear_flags)
        {
            static_cast<Host*>(host->host_data)
                ->owner->parameterRescanFlags.fetch_or(
                    CLAP_PARAM_RESCAN_VALUES,
                    std::memory_order_release);
        }

        static void CLAP_ABI requestFlush(const clap_host_t* host)
        {
            static_cast<Host*>(host->host_data)
                ->owner->flushRequested.store(
                    true,
                    std::memory_order_release);
        }

        static void CLAP_ABI latencyChanged(const clap_host_t* host)
        {
            static_cast<Host*>(host->host_data)
                ->owner->latencyRefreshRequested.store(
                    true,
                    std::memory_order_release);
        }

        static bool CLAP_ABI supportsAudioPortRescan(
            const clap_host_t*,
            std::uint32_t flags)
        {
            constexpr std::uint32_t supported =
                CLAP_AUDIO_PORTS_RESCAN_NAMES
                | CLAP_AUDIO_PORTS_RESCAN_FLAGS;
            return (flags & ~supported) == 0;
        }

        static void CLAP_ABI rescanAudioPorts(
            const clap_host_t* host,
            std::uint32_t flags)
        {
            static_cast<Host*>(host->host_data)
                ->owner->audioPortRescanFlags.fetch_or(
                    flags,
                    std::memory_order_release);
        }

        static void CLAP_ABI markStateDirty(const clap_host_t* host)
        {
            static_cast<Host*>(host->host_data)
                ->owner->stateDirty.store(true, std::memory_order_release);
        }

        struct AudioThreadScope
        {
            AudioThreadScope()
            {
                ++audioThreadDepth;
            }

            ~AudioThreadScope()
            {
                --audioThreadDepth;
            }
        };

        inline static thread_local int audioThreadDepth = 0;
        inline static const clap_host_thread_check_t threadCheck {
            isMainThread,
            isAudioThread
        };
        inline static const clap_host_params_t hostParams {
            rescanParams,
            clearParam,
            requestFlush
        };
        inline static const clap_host_latency_t hostLatency {
            latencyChanged
        };
        inline static const clap_host_audio_ports_t hostAudioPorts {
            supportsAudioPortRescan,
            rescanAudioPorts
        };
        inline static const clap_host_state_t hostState {
            markStateDirty
        };

        clap_host_t value {};
        Impl* owner = nullptr;
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
            if (isRemoved())
                return;
            plainValue.store(
                info.min_value
                    + (info.max_value - info.min_value)
                        * juce::jlimit(0.0f, 1.0f, normalized),
                std::memory_order_relaxed);
            dirty.store(true, std::memory_order_release);
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

        [[nodiscard]] double plainForNormalized(
            float normalized) const noexcept
        {
            return info.min_value
                + (info.max_value - info.min_value)
                    * juce::jlimit(0.0f, 1.0f, normalized);
        }

        void refresh()
        {
            double value = info.default_value;
            if (params != nullptr
                && params->get_value(pluginInstance, info.id, &value))
                plainValue.store(value, std::memory_order_relaxed);
            dirty.store(false, std::memory_order_release);
        }

        [[nodiscard]] bool consumeDirty() noexcept
        {
            return dirty.exchange(false, std::memory_order_acq_rel);
        }

        [[nodiscard]] bool isDirty() const noexcept
        {
            return dirty.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool requiresProcess() const noexcept
        {
            return (info.flags & CLAP_PARAM_REQUIRES_PROCESS) != 0;
        }

        void updateFromPlugin(double value) noexcept
        {
            plainValue.store(value, std::memory_order_relaxed);
            dirty.store(false, std::memory_order_release);
        }

        void updateInfo(const clap_param_info_t& value)
        {
            info = value;
            removed.store(false, std::memory_order_release);
            refresh();
        }

        void markRemoved()
        {
            info.cookie = nullptr;
            constexpr auto mutableFlags =
                static_cast<clap_param_info_flags>(
                    CLAP_PARAM_IS_AUTOMATABLE
                    | CLAP_PARAM_IS_MODULATABLE
                    | CLAP_PARAM_REQUIRES_PROCESS);
            info.flags &= ~mutableFlags;
            info.flags |=
                CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_HIDDEN;
            std::snprintf(
                info.name,
                sizeof(info.name),
                "Removed parameter");
            dirty.store(false, std::memory_order_release);
            removed.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool isRemoved() const noexcept
        {
            return removed.load(std::memory_order_acquire);
        }

        clap_param_info_t info {};

    private:
        const clap_plugin_t* pluginInstance = nullptr;
        const clap_plugin_params_t* params = nullptr;
        std::atomic<double> plainValue { 0.0 };
        std::atomic<bool> dirty { false };
        std::atomic<bool> removed { false };
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
        std::vector<clap_event_param_value_t> values;
        std::uint32_t count = 0;
    };

    struct OutputEvents
    {
        explicit OutputEvents(Impl& implementation)
            : owner(&implementation)
        {
            interface = { this, tryPush };
        }

        static bool CLAP_ABI tryPush(
            const clap_output_events_t* list,
            const clap_event_header_t* header)
        {
            if (list == nullptr
                || header == nullptr
                || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_PARAM_VALUE
                || header->size < sizeof(clap_event_param_value_t))
                return true;
            const auto* event =
                reinterpret_cast<const clap_event_param_value_t*>(
                    header);
            auto* events = static_cast<OutputEvents*>(list->ctx);
            if (auto* parameter =
                    events->owner->parameterFor(event->param_id))
                parameter->updateFromPlugin(event->value);
            return true;
        }

        clap_output_events_t interface {};
        Impl* owner = nullptr;
    };

    Impl()
        : outputEvents(*this)
    {
    }

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
        implementation->host =
            std::make_unique<Host>(*implementation);
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
        destination.clear();
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

    [[nodiscard]] Parameter* parameterFor(clap_id id) const noexcept
    {
        const auto found = std::find_if(
            parameters.begin(),
            parameters.end(),
            [id](const auto* parameter)
            {
                return !parameter->isRemoved()
                    && parameter->info.id == id;
            });
        return found != parameters.end() ? *found : nullptr;
    }

    [[nodiscard]] bool hasDirtyParameters() const noexcept
    {
        return std::any_of(
            parameters.cbegin(),
            parameters.cend(),
            [](const auto* parameter)
            {
                return !parameter->isRemoved()
                    && parameter->isDirty();
            });
    }

    [[nodiscard]] bool hasDirtyProcessParameters() const noexcept
    {
        return std::any_of(
            parameters.cbegin(),
            parameters.cend(),
            [](const auto* parameter)
            {
                return !parameter->isRemoved()
                    && parameter->requiresProcess()
                    && parameter->isDirty();
            });
    }

    void buildDirtyParameterEvents(bool processDelivery)
    {
        inputEvents.count = 0;
        for (auto* parameter : parameters)
        {
            if (parameter->isRemoved())
                continue;
            if (!processDelivery && parameter->requiresProcess())
                continue;
            if (!parameter->consumeDirty())
                continue;
            if (inputEvents.count >= inputEvents.values.size())
                break;
            auto& event = inputEvents.values[inputEvents.count++];
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
    }

    void waitForAudioCalls() const noexcept
    {
        while (processCallsInFlight.load(std::memory_order_seq_cst) > 0)
            juce::Thread::sleep(1);
    }

    void stopProcessingOnAudioThread() noexcept
    {
        if (!processing)
            return;
        Host::AudioThreadScope audioThread;
        plugin->stop_processing(plugin);
        processing = false;
    }

    void flushParameters(bool force)
    {
        if (params == nullptr)
            return;
        const auto recentlyProcessed =
            juce::Time::getMillisecondCounterHiRes()
                - lastProcessTimeMilliseconds.load(
                    std::memory_order_acquire)
            < 100.0;
        if (!force
            && active.load(std::memory_order_acquire)
            && processing.load(std::memory_order_acquire)
            && recentlyProcessed)
            return;

        lifecycleChanging.store(true, std::memory_order_seq_cst);
        waitForAudioCalls();
        const auto wasProcessing =
            processing.load(std::memory_order_acquire);
        if (wasProcessing)
            stopProcessingOnAudioThread();
        buildDirtyParameterEvents(false);
        const auto explicitlyRequested =
            flushRequested.exchange(false, std::memory_order_acq_rel);
        if (inputEvents.count == 0 && !explicitlyRequested)
        {
            lifecycleChanging.store(false, std::memory_order_seq_cst);
            return;
        }
        std::optional<Host::AudioThreadScope> audioThread;
        if (active.load(std::memory_order_acquire))
            audioThread.emplace();
        params->flush(
            plugin,
            &inputEvents.interface,
            &outputEvents.interface);
        if (wasProcessing)
            host->processRequested.store(
                true,
                std::memory_order_release);
        lifecycleChanging.store(false, std::memory_order_seq_cst);
    }

    void refreshParameters()
    {
        for (auto* parameter : parameters)
            parameter->refresh();
    }

    void prepareAudioBuffers()
    {
        inputBuffers.resize(inputs.size());
        outputBuffers.resize(outputs.size());
        inputPointers.resize(inputs.size());
        outputPointers.resize(outputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index)
            inputPointers[index].resize(inputs[index].channels);
        for (std::size_t index = 0; index < outputs.size(); ++index)
            outputPointers[index].resize(outputs[index].channels);
    }

    void rebuildParameterMetadata(
        ClapPluginInstance& owner,
        bool rebuildAll)
    {
        if (params == nullptr)
            return;

        lifecycleChanging.store(true, std::memory_order_seq_cst);
        waitForAudioCalls();
        const auto wasActive = active.load(std::memory_order_acquire);
        if (rebuildAll)
        {
            stopProcessingOnAudioThread();
            if (active.exchange(false, std::memory_order_acq_rel))
                plugin->deactivate(plugin);
        }

        std::vector<Parameter*> hosted;
        for (auto* parameter : owner.getParameters())
            if (auto* clapParameter =
                    dynamic_cast<Parameter*>(parameter))
                hosted.push_back(clapParameter);

        const auto count = params->count(plugin);
        std::vector<clap_id> seenIds;
        seenIds.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            clap_param_info_t info {};
            if (!params->get_info(plugin, index, &info))
                continue;
            if (std::find(
                    seenIds.begin(),
                    seenIds.end(),
                    info.id) != seenIds.end())
                continue;
            seenIds.push_back(info.id);

            const auto existing = std::find_if(
                hosted.begin(),
                hosted.end(),
                [&info](const auto* parameter)
                {
                    return parameter->info.id == info.id;
                });
            if (existing != hosted.end())
            {
                (*existing)->updateInfo(info);
            }
            else
            {
                auto parameter = std::make_unique<Parameter>(
                    info,
                    plugin,
                    params);
                auto* added = dynamic_cast<Parameter*>(
                    owner.addClapHostedParameter(
                        std::move(parameter)));
                if (added != nullptr)
                    hosted.push_back(added);
            }
        }
        for (auto* parameter : hosted)
        {
            if (std::find(
                    seenIds.begin(),
                    seenIds.end(),
                    parameter->info.id) == seenIds.end())
                parameter->markRemoved();
        }
        parameters = std::move(hosted);
        inputEvents.values.resize(
            parameters.size()
            + maximumScheduledAutomationEvents);
        scheduledAutomationEventCount = 0;

        if (rebuildAll && wasActive)
        {
            if (plugin->activate(
                    plugin,
                    sampleRate,
                    1,
                    static_cast<std::uint32_t>(
                        maximumBlockSize)))
            {
                active.store(true, std::memory_order_release);
                host->processRequested.store(
                    true,
                    std::memory_order_release);
                refreshLatency(owner);
            }
        }
        lifecycleChanging.store(false, std::memory_order_seq_cst);
        owner.updateHostDisplay(
            juce::AudioProcessor::ChangeDetails {}
                .withParameterInfoChanged(true)
                .withLatencyChanged(rebuildAll));
    }

    void refreshLatency(ClapPluginInstance& owner)
    {
        if (latency != nullptr)
        {
            owner.setLatencySamples(static_cast<int>(
                latency->get(plugin)));
        }
    }

    void drainMainThread(ClapPluginInstance& owner)
    {
        if (host->callbackRequested.exchange(
                false,
                std::memory_order_acq_rel)
            && plugin->on_main_thread != nullptr)
        {
            plugin->on_main_thread(plugin);
            refreshParameters();
        }

        const auto restart = host->restartRequested.exchange(
            false,
            std::memory_order_acq_rel);
        const auto portFlags = audioPortRescanFlags.exchange(
            0,
            std::memory_order_acq_rel);
        if (restart)
        {
            owner.prepareToPlay(sampleRate, maximumBlockSize);
            refreshParameters();
            owner.updateHostDisplay();
        }
        else if (portFlags != 0)
        {
            owner.updateHostDisplay();
        }
        else if (latencyRefreshRequested.exchange(
                     false,
                     std::memory_order_acq_rel))
        {
            refreshLatency(owner);
            owner.updateHostDisplay();
        }

        if (host->processRequested.exchange(
                false,
                std::memory_order_acq_rel)
            && active.load(std::memory_order_acquire))
        {
            lifecycleChanging.store(true, std::memory_order_seq_cst);
            waitForAudioCalls();
            if (active.load(std::memory_order_acquire)
                && !processing.load(std::memory_order_acquire))
            {
                Host::AudioThreadScope audioThread;
                processing.store(
                    plugin->start_processing(plugin),
                    std::memory_order_release);
            }
            lifecycleChanging.store(false, std::memory_order_seq_cst);
        }

        const auto rescanFlags = parameterRescanFlags.exchange(
            0,
            std::memory_order_acq_rel);
        if ((rescanFlags & CLAP_PARAM_RESCAN_ALL) != 0)
            rebuildParameterMetadata(owner, true);
        else if ((rescanFlags & CLAP_PARAM_RESCAN_INFO) != 0)
            rebuildParameterMetadata(owner, false);
        else if ((rescanFlags & CLAP_PARAM_RESCAN_VALUES) != 0)
            refreshParameters();
        if ((rescanFlags & CLAP_PARAM_RESCAN_TEXT) != 0)
        {
            owner.updateHostDisplay(
                juce::AudioProcessor::ChangeDetails {}
                    .withParameterInfoChanged(true));
        }

        if (flushRequested.load(std::memory_order_acquire)
            || hasDirtyParameters())
        {
            flushParameters(false);
        }
    }

    juce::Result validateSavedState(
        const juce::MemoryBlock& candidate)
    {
        auto validationHost = std::make_unique<Host>(*this);
        const auto id = pluginId(description.fileOrIdentifier);
        const auto* validationPlugin =
            module->factory->create_plugin(
                module->factory,
                &validationHost->value,
                id.toRawUTF8());
        if (validationPlugin == nullptr
            || validationPlugin == plugin)
            return juce::Result::fail(
                "The CLAP plugin could not create a disposable state-validation instance.");

        struct PluginGuard
        {
            ~PluginGuard()
            {
                if (value != nullptr)
                    value->destroy(value);
            }
            const clap_plugin_t* value = nullptr;
        } pluginGuard { validationPlugin };
        if (!validationPlugin->init(validationPlugin))
            return juce::Result::fail(
                "The CLAP state-validation instance failed to initialize.");

        const auto* validationState =
            static_cast<const clap_plugin_state_t*>(
                validationPlugin->get_extension(
                    validationPlugin,
                    CLAP_EXT_STATE));
        if (validationState == nullptr)
            return juce::Result::fail(
                "The disposable CLAP instance does not expose state loading.");

        juce::MemoryInputStream input(candidate, false);
        clap_istream_t stream {
            &input,
            [](const clap_istream_t* value,
               void* destination,
               std::uint64_t bytes) -> std::int64_t
            {
                auto* source =
                    static_cast<juce::MemoryInputStream*>(
                        value->ctx);
                const auto requested = static_cast<int>(
                    std::min<std::uint64_t>(
                        bytes,
                        static_cast<std::uint64_t>(INT_MAX)));
                return static_cast<std::int64_t>(
                    source->read(destination, requested));
            }
        };
        return validationState->load(
                   validationPlugin,
                   &stream)
            ? juce::Result::ok()
            : juce::Result::fail(
                  "The CLAP plugin rejected its saved state in a disposable validation instance.");
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
    std::vector<clap_audio_buffer_t> inputBuffers;
    std::vector<clap_audio_buffer_t> outputBuffers;
    std::vector<std::vector<float*>> inputPointers;
    std::vector<std::vector<float*>> outputPointers;
    InputEvents inputEvents;
    std::vector<clap_event_param_value_t> scheduledAutomationEvents;
    std::uint32_t scheduledAutomationEventCount = 0;
    std::atomic<std::uint64_t> scheduledAutomationEventsDropped { 0 };
    OutputEvents outputEvents;
    std::atomic<bool> active { false };
    std::atomic<bool> processing { false };
    double sampleRate = 48000.0;
    int maximumBlockSize = 512;
    std::int64_t steadyTime = 0;
    std::atomic<bool> lifecycleChanging { false };
    std::atomic<int> processCallsInFlight { 0 };
    std::atomic<bool> flushRequested { false };
    std::atomic<std::uint32_t> parameterRescanFlags { 0 };
    std::atomic<std::uint32_t> audioPortRescanFlags { 0 };
    std::atomic<bool> latencyRefreshRequested { false };
    std::atomic<bool> stateDirty { false };
    std::atomic<double> lastProcessTimeMilliseconds { 0.0 };
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
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && !messages->isThisTheMessageThread())
    {
        auto created = juce::MessageManager::callSync(
            [&description, sampleRate, blockSize, &error]
            {
                return ClapPluginInstance::create(
                    description,
                    sampleRate,
                    blockSize,
                    error);
            });
        if (created.has_value())
            return std::move(*created);
        error = "The CLAP main thread is unavailable.";
        return {};
    }
    if (messages == nullptr)
    {
        error = "CLAP hosting requires an initialized message thread.";
        return {};
    }

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
    impl->prepareAudioBuffers();
    if (impl->params == nullptr)
    {
        startTimerHz(50);
        return;
    }
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
        addClapHostedParameter(std::move(parameter));
    }
    impl->scheduledAutomationEvents.resize(
        Impl::maximumScheduledAutomationEvents);
    impl->inputEvents.values.resize(
        impl->parameters.size()
        + Impl::maximumScheduledAutomationEvents);
    startTimerHz(50);
}

juce::AudioProcessorParameter*
ClapPluginInstance::addClapHostedParameter(
    std::unique_ptr<juce::HostedAudioProcessorParameter> parameter)
{
    auto* result = parameter.get();
    addHostedParameter(std::move(parameter));
    return result;
}

ClapPluginInstance::~ClapPluginInstance()
{
    stopTimer();
    releaseResources();
    auto* implementation = impl.release();
    if (implementation == nullptr)
        return;
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || messages->isThisTheMessageThread())
    {
        delete implementation;
        return;
    }
    if (!juce::MessageManager::callSync(
            [implementation]
            {
                delete implementation;
            }))
    {
        juce::Logger::writeToLog(
            "clap.lifecycle: destruction main thread unavailable");
    }
}

const juce::String ClapPluginInstance::getName() const
{
    return impl->description.name;
}

void ClapPluginInstance::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    const auto activate = [this,
                           sampleRate,
                           blockSize = std::max(1, maximumBlockSize)]
    {
        impl->lifecycleChanging.store(true, std::memory_order_seq_cst);
        impl->waitForAudioCalls();
        impl->stopProcessingOnAudioThread();
        if (impl->active.exchange(false, std::memory_order_acq_rel))
            impl->plugin->deactivate(impl->plugin);
        impl->sampleRate = sampleRate;
        impl->maximumBlockSize = blockSize;
        impl->inputScratch.setSize(
            std::max(1, getTotalNumInputChannels()),
            impl->maximumBlockSize,
            false,
            true,
            false);
        impl->inputScratch.clear();
        impl->steadyTime = 0;
        if (impl->plugin->activate(
                impl->plugin,
                impl->sampleRate,
                1,
                static_cast<std::uint32_t>(
                    impl->maximumBlockSize)))
        {
            impl->active.store(true, std::memory_order_release);
            impl->host->processRequested.store(
                true,
                std::memory_order_release);
            impl->refreshLatency(*this);
        }
        impl->lifecycleChanging.store(false, std::memory_order_seq_cst);
    };
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && messages->isThisTheMessageThread())
        activate();
    else if (!juce::MessageManager::callSync(
                  [activate]
                  {
                      activate();
                  }))
        juce::Logger::writeToLog(
            "clap.lifecycle: activation main thread unavailable");
}

void ClapPluginInstance::releaseResources()
{
    if (impl == nullptr || impl->plugin == nullptr)
        return;
    const auto deactivate = [this]
    {
        impl->lifecycleChanging.store(true, std::memory_order_seq_cst);
        impl->waitForAudioCalls();
        impl->stopProcessingOnAudioThread();
        if (impl->active.exchange(false, std::memory_order_acq_rel))
            impl->plugin->deactivate(impl->plugin);
        impl->lifecycleChanging.store(false, std::memory_order_seq_cst);
    };
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || messages->isThisTheMessageThread())
        deactivate();
    else if (!juce::MessageManager::callSync(
                  [deactivate]
                  {
                      deactivate();
                  }))
        juce::Logger::writeToLog(
            "clap.lifecycle: deactivation main thread unavailable");
}

void ClapPluginInstance::processBlock(juce::AudioBuffer<float>& audio,
                                      juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (impl->lifecycleChanging.load(std::memory_order_seq_cst))
    {
        audio.clear();
        return;
    }
    impl->processCallsInFlight.fetch_add(1, std::memory_order_seq_cst);
    struct ProcessScope
    {
        ~ProcessScope()
        {
            count.fetch_sub(1, std::memory_order_seq_cst);
        }
        std::atomic<int>& count;
    } processScope { impl->processCallsInFlight };
    if (impl->lifecycleChanging.load(std::memory_order_seq_cst)
        || !impl->active.load(std::memory_order_acquire))
    {
        audio.clear();
        return;
    }

    Impl::Host::AudioThreadScope audioThread;
    impl->lastProcessTimeMilliseconds.store(
        juce::Time::getMillisecondCounterHiRes(),
        std::memory_order_release);
    if (!impl->processing.load(std::memory_order_acquire))
    {
        impl->host->processRequested.store(
            false,
            std::memory_order_release);
        if (!impl->plugin->start_processing(impl->plugin))
        {
            audio.clear();
            return;
        }
        impl->processing.store(true, std::memory_order_release);
    }
    impl->buildDirtyParameterEvents(true);
    for (std::uint32_t index = 0;
         index < impl->scheduledAutomationEventCount
         && impl->inputEvents.count < impl->inputEvents.values.size();
         ++index)
    {
        if (impl->scheduledAutomationEvents[index].header.time
            < static_cast<std::uint32_t>(audio.getNumSamples()))
        {
            impl->inputEvents.values[impl->inputEvents.count++] =
                impl->scheduledAutomationEvents[index];
        }
    }
    impl->scheduledAutomationEventCount = 0;
    std::sort(
        impl->inputEvents.values.begin(),
        impl->inputEvents.values.begin() + impl->inputEvents.count,
        [](const auto& left, const auto& right)
        {
            return left.header.time == right.header.time
                ? left.param_id < right.param_id
                : left.header.time < right.header.time;
        });

    const auto inputCount = impl->inputs.size();
    const auto outputCount = impl->outputs.size();

    auto scratchChannel = 0;
    for (std::size_t bus = 0; bus < inputCount; ++bus)
    {
        const auto channels = std::min(
            getChannelCountOfBus(true, static_cast<int>(bus)),
            static_cast<int>(impl->inputPointers[bus].size()));
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
                impl->inputPointers[bus][static_cast<std::size_t>(channel)] =
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
                impl->inputPointers[bus][static_cast<std::size_t>(channel)] =
                    impl->inputScratch.getWritePointer(
                        scratchChannel + channel);
            }
        }
        scratchChannel += channels;
        impl->inputBuffers[bus] = {
            impl->inputPointers[bus].data(),
            nullptr,
            static_cast<std::uint32_t>(channels),
            0,
            0
        };
    }
    for (std::size_t bus = 0; bus < outputCount; ++bus)
    {
        const auto channels = std::min(
            getChannelCountOfBus(false, static_cast<int>(bus)),
            static_cast<int>(impl->outputPointers[bus].size()));
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto bufferChannel = getChannelIndexInProcessBlockBuffer(
                false,
                static_cast<int>(bus),
                channel);
            impl->outputPointers[bus][static_cast<std::size_t>(channel)] =
                audio.getWritePointer(bufferChannel);
        }
        impl->outputBuffers[bus] = {
            impl->outputPointers[bus].data(),
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
        impl->inputBuffers.data(),
        impl->outputBuffers.data(),
        static_cast<std::uint32_t>(inputCount),
        static_cast<std::uint32_t>(outputCount),
        &impl->inputEvents.interface,
        &impl->outputEvents.interface
    };
    const auto status = impl->plugin->process(impl->plugin, &process);
    impl->steadyTime += audio.getNumSamples();
    if (status == CLAP_PROCESS_ERROR)
        audio.clear();
    else if (status == CLAP_PROCESS_SLEEP)
    {
        impl->plugin->stop_processing(impl->plugin);
        impl->processing.store(false, std::memory_order_release);
    }
}

double ClapPluginInstance::getTailLengthSeconds() const
{
    if (impl->tail == nullptr)
        return 0.0;
    const auto query = [this]
    {
        return impl->tail->get(impl->plugin);
    };
    auto samples = std::uint32_t {};
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && messages->isThisTheMessageThread())
        samples = query();
    else if (auto result = juce::MessageManager::callSync(query);
             result.has_value())
        samples = *result;
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
    if (const auto result = saveValidatedState(destination);
        result.failed())
    {
        juce::Logger::writeToLog(
            "clap.state: " + result.getErrorMessage());
    }
}

void ClapPluginInstance::setStateInformation(const void* data, int size)
{
    if (const auto result = restoreValidatedState(data, size);
        result.failed())
    {
        juce::Logger::writeToLog(
            "clap.state: " + result.getErrorMessage());
    }
}

juce::Result ClapPluginInstance::saveValidatedState(
    juce::MemoryBlock& destination)
{
    destination.reset();
    if (impl->state == nullptr)
        return juce::Result::ok();

    const auto save = [this, &destination]
    {
        if (impl->hasDirtyProcessParameters())
        {
            const auto wasActive =
                impl->active.load(std::memory_order_acquire);
            if (!wasActive)
                prepareToPlay(
                    impl->sampleRate,
                    impl->maximumBlockSize);
            juce::AudioBuffer<float> silence(
                std::max({
                    1,
                    getTotalNumInputChannels(),
                    getTotalNumOutputChannels()
                }),
                std::max(
                    1,
                    std::min(64, impl->maximumBlockSize)));
            silence.clear();
            juce::MidiBuffer midi;
            processBlock(silence, midi);
            if (!wasActive)
                releaseResources();
            if (impl->hasDirtyProcessParameters())
                return juce::Result::fail(
                    "The CLAP plugin did not accept a process-only parameter before save.");
        }
        impl->flushParameters(true);
        juce::MemoryBlock candidate;
        juce::MemoryOutputStream output(candidate, true);
        clap_ostream_t stream {
            &output,
            [](const clap_ostream_t* value,
               const void* data,
               std::uint64_t size) -> std::int64_t
            {
                auto* target = static_cast<juce::MemoryOutputStream*>(
                    value->ctx);
                return target->write(
                           data,
                           static_cast<std::size_t>(size))
                    ? static_cast<std::int64_t>(size)
                    : -1;
            }
        };
        const auto saved = impl->state->save(
            impl->plugin,
            &stream);
        output.flush();
        if (!saved || candidate.isEmpty())
            return juce::Result::fail(
                "The CLAP plugin failed to save complete state.");

        if (const auto validation =
                impl->validateSavedState(candidate);
            validation.failed())
            return validation;
        destination = std::move(candidate);
        impl->stateDirty.store(false, std::memory_order_release);
        return juce::Result::ok();
    };
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && messages->isThisTheMessageThread())
        return save();
    const auto result = juce::MessageManager::callSync(save);
    return result.has_value()
        ? *result
        : juce::Result::fail(
              "The CLAP state save main thread is unavailable.");
}

juce::Result ClapPluginInstance::restoreValidatedState(
    const void* data,
    int size)
{
    if (impl->state == nullptr)
        return juce::Result::ok();
    if (data == nullptr || size <= 0)
        return juce::Result::fail("CLAP state data is empty.");

    const auto load = [this, data, size]
    {
        impl->lifecycleChanging.store(true, std::memory_order_seq_cst);
        impl->waitForAudioCalls();
        juce::MemoryInputStream input(
            data,
            static_cast<std::size_t>(size),
            false);
        clap_istream_t stream {
            &input,
            [](const clap_istream_t* value,
               void* destination,
               std::uint64_t bytes) -> std::int64_t
            {
                auto* source =
                    static_cast<juce::MemoryInputStream*>(
                        value->ctx);
                const auto requested = static_cast<int>(
                    std::min<std::uint64_t>(
                        bytes,
                        static_cast<std::uint64_t>(INT_MAX)));
                return static_cast<std::int64_t>(
                    source->read(destination, requested));
            }
        };
        if (!impl->state->load(impl->plugin, &stream))
        {
            impl->lifecycleChanging.store(
                false,
                std::memory_order_seq_cst);
            return juce::Result::fail(
                "The CLAP plugin rejected restored state.");
        }
        impl->refreshParameters();
        if (impl->active.load(std::memory_order_acquire))
        {
            Impl::Host::AudioThreadScope audioThread;
            impl->plugin->reset(impl->plugin);
        }
        impl->steadyTime = 0;
        impl->stateDirty.store(false, std::memory_order_release);
        impl->lifecycleChanging.store(false, std::memory_order_seq_cst);
        return juce::Result::ok();
    };
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && messages->isThisTheMessageThread())
        return load();
    const auto result = juce::MessageManager::callSync(load);
    return result.has_value()
        ? *result
        : juce::Result::fail(
              "The CLAP state load main thread is unavailable.");
}

bool ClapPluginInstance::supportsSampleAccurateAutomation(
    std::span<const PluginBridgeParameterEvent> events) const noexcept
{
    if (impl->lifecycleChanging.load(std::memory_order_seq_cst))
        return false;

    impl->processCallsInFlight.fetch_add(
        1,
        std::memory_order_seq_cst);
    struct SupportScope
    {
        ~SupportScope()
        {
            count.fetch_sub(1, std::memory_order_seq_cst);
        }
        std::atomic<int>& count;
    } supportScope { impl->processCallsInFlight };
    if (impl->lifecycleChanging.load(std::memory_order_seq_cst))
        return false;

    std::uint64_t required = 0;
    for (const auto& event : events)
    {
        if (event.parameterIndex
                >= static_cast<std::uint32_t>(
                    impl->parameters.size())
            || impl->parameters[
                   static_cast<std::size_t>(
                       event.parameterIndex)]->isRemoved())
            return false;

        const auto expanded = (event.flags
                                  & PluginBridgeParameterEvent::rampFlag)
                    != 0
                && event.rampEndOffset > event.sampleOffset
            ? static_cast<std::uint64_t>(
                  event.rampEndOffset - event.sampleOffset)
                + 1
            : 1;
        required += expanded;
        if (required
            > Impl::maximumScheduledAutomationEvents)
            return false;
    }
    return true;
}

void ClapPluginInstance::processBlockWithAutomation(
    juce::AudioBuffer<float>& audio,
    juce::MidiBuffer& midi,
    std::span<const PluginBridgeParameterEvent> events) noexcept
{
    if (impl->lifecycleChanging.load(std::memory_order_seq_cst))
    {
        audio.clear();
        return;
    }
    impl->processCallsInFlight.fetch_add(
        1,
        std::memory_order_seq_cst);
    struct AutomationScope
    {
        ~AutomationScope()
        {
            count.fetch_sub(1, std::memory_order_seq_cst);
        }
        std::atomic<int>& count;
    } automationScope { impl->processCallsInFlight };
    if (impl->lifecycleChanging.load(std::memory_order_seq_cst))
    {
        audio.clear();
        return;
    }

    std::uint64_t required = 0;
    for (const auto& event : events)
    {
        if (event.parameterIndex
                >= static_cast<std::uint32_t>(
                    impl->parameters.size())
            || impl->parameters[
                   static_cast<std::size_t>(
                       event.parameterIndex)]->isRemoved())
        {
            impl->scheduledAutomationEventsDropped.fetch_add(
                1,
                std::memory_order_relaxed);
            processBlock(audio, midi);
            return;
        }
        required += (event.flags
                        & PluginBridgeParameterEvent::rampFlag)
                    != 0
                && event.rampEndOffset > event.sampleOffset
            ? static_cast<std::uint64_t>(
                  event.rampEndOffset - event.sampleOffset)
                + 1
            : 1;
        if (required
            > Impl::maximumScheduledAutomationEvents)
        {
            impl->scheduledAutomationEventsDropped.fetch_add(
                static_cast<std::uint64_t>(events.size()),
                std::memory_order_relaxed);
            processBlock(audio, midi);
            return;
        }
    }

    struct ScheduledAutomationScope
    {
        ~ScheduledAutomationScope()
        {
            count = 0;
        }
        std::uint32_t& count;
    } scheduledScope { impl->scheduledAutomationEventCount };
    impl->scheduledAutomationEventCount = 0;
    const auto append = [this](std::uint32_t parameterIndex,
                               int sampleOffset,
                               float value)
    {
        jassert(parameterIndex
            < static_cast<std::uint32_t>(
                impl->parameters.size()));
        jassert(impl->scheduledAutomationEventCount
            < impl->scheduledAutomationEvents.size());
        const auto* parameter =
            impl->parameters[static_cast<std::size_t>(
                parameterIndex)];
        auto& scheduled = impl->scheduledAutomationEvents[
            impl->scheduledAutomationEventCount++];
        scheduled = {};
        scheduled.header.size = sizeof(scheduled);
        scheduled.header.time = static_cast<std::uint32_t>(
            sampleOffset);
        scheduled.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        scheduled.header.type = CLAP_EVENT_PARAM_VALUE;
        scheduled.param_id = parameter->info.id;
        scheduled.cookie = parameter->info.cookie;
        scheduled.note_id = -1;
        scheduled.port_index = -1;
        scheduled.channel = -1;
        scheduled.key = -1;
        scheduled.value = parameter->plainForNormalized(value);
    };

    for (const auto& event : events)
    {
        const auto start = juce::jlimit(
            0,
            std::max(0, audio.getNumSamples() - 1),
            static_cast<int>(event.sampleOffset));
        if ((event.flags & PluginBridgeParameterEvent::rampFlag)
                != 0
            && event.rampEndOffset > event.sampleOffset)
        {
            const auto end = std::min(
                audio.getNumSamples() - 1,
                static_cast<int>(event.rampEndOffset));
            const auto duration = static_cast<float>(
                event.rampEndOffset - event.sampleOffset);
            for (auto sample = start; sample <= end; ++sample)
            {
                const auto progress = static_cast<float>(
                    sample - static_cast<int>(event.sampleOffset))
                    / duration;
                append(
                    event.parameterIndex,
                    sample,
                    event.value
                        + (event.rampEndValue - event.value)
                            * progress);
            }
        }
        else
        {
            append(
                event.parameterIndex,
                start,
                event.value);
        }
    }
    for (const auto& event : events)
    {
        if (event.parameterIndex
            >= static_cast<std::uint32_t>(
                impl->parameters.size()))
            continue;
        auto* parameter = impl->parameters[
            static_cast<std::size_t>(event.parameterIndex)];
        parameter->updateFromPlugin(
            parameter->plainForNormalized(
                (event.flags & PluginBridgeParameterEvent::rampFlag)
                        != 0
                    ? event.rampEndValue
                    : event.value));
    }
    processBlock(audio, midi);
}

void ClapPluginInstance::timerCallback()
{
    if (impl != nullptr && impl->plugin != nullptr)
        impl->drainMainThread(*this);
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
