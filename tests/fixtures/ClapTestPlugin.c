#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_plugin {
    clap_plugin_t plugin;
    double gain;
} test_plugin_t;

static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION,
    .id = "dev.mbianchi.studioduo.test-gain",
    .name = "Studio Duo CLAP Test Gain",
    .vendor = "Studio Duo",
    .url = "https://github.com/mbianchidev/studio-duo",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "Deterministic CLAP host fixture",
    .features = (const char*[]) {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
        CLAP_PLUGIN_FEATURE_UTILITY,
        NULL
    }
};

static test_plugin_t* self(const clap_plugin_t* plugin)
{
    return (test_plugin_t*) plugin->plugin_data;
}

static bool plugin_init(const clap_plugin_t* plugin)
{
    (void) plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t* plugin)
{
    free(self(plugin));
}

static bool plugin_activate(const clap_plugin_t* plugin,
                            double sample_rate,
                            uint32_t min_frames,
                            uint32_t max_frames)
{
    (void) plugin;
    return sample_rate > 0.0 && min_frames > 0 && max_frames >= min_frames;
}

static void plugin_deactivate(const clap_plugin_t* plugin)
{
    (void) plugin;
}

static bool plugin_start_processing(const clap_plugin_t* plugin)
{
    (void) plugin;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t* plugin)
{
    (void) plugin;
}

static void plugin_reset(const clap_plugin_t* plugin)
{
    (void) plugin;
}

static uint32_t input_event_count(const clap_input_events_t* events)
{
    return events != NULL ? events->size(events) : 0;
}

static clap_process_status plugin_process(const clap_plugin_t* plugin,
                                          const clap_process_t* process)
{
    test_plugin_t* instance = self(plugin);
    uint32_t event_index = 0;
    const uint32_t event_count = input_event_count(process->in_events);
    for (uint32_t frame = 0; frame < process->frames_count; ++frame)
    {
        while (event_index < event_count)
        {
            const clap_event_header_t* header =
                process->in_events->get(process->in_events, event_index);
            if (header == NULL || header->time > frame)
                break;
            if (header->space_id == CLAP_CORE_EVENT_SPACE_ID
                && header->type == CLAP_EVENT_PARAM_VALUE)
            {
                const clap_event_param_value_t* value =
                    (const clap_event_param_value_t*) header;
                if (value->param_id == 1)
                    instance->gain = value->value;
            }
            ++event_index;
        }

        for (uint32_t port = 0; port < process->audio_outputs_count; ++port)
        {
            clap_audio_buffer_t* output = &process->audio_outputs[port];
            const clap_audio_buffer_t* input =
                port < process->audio_inputs_count
                ? &process->audio_inputs[port]
                : NULL;
            for (uint32_t channel = 0; channel < output->channel_count; ++channel)
            {
                const float source =
                    input != NULL
                        && input->data32 != NULL
                        && channel < input->channel_count
                    ? input->data32[channel][frame]
                    : 0.0f;
                output->data32[channel][frame] =
                    source * (float) instance->gain;
            }
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t* plugin, bool is_input)
{
    (void) plugin;
    (void) is_input;
    return 1;
}

static bool audio_ports_get(const clap_plugin_t* plugin,
                            uint32_t index,
                            bool is_input,
                            clap_audio_port_info_t* info)
{
    (void) plugin;
    if (index != 0 || info == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = is_input ? 10 : 20;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = is_input ? 20 : 10;
    return true;
}

static uint32_t params_count(const clap_plugin_t* plugin)
{
    (void) plugin;
    return 1;
}

static bool params_get_info(const clap_plugin_t* plugin,
                            uint32_t index,
                            clap_param_info_t* info)
{
    (void) plugin;
    if (index != 0 || info == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 1;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof(info->name), "Gain");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.5;
    return true;
}

static bool params_get_value(const clap_plugin_t* plugin,
                             clap_id param_id,
                             double* value)
{
    if (param_id != 1 || value == NULL)
        return false;
    *value = self(plugin)->gain;
    return true;
}

static bool params_value_to_text(const clap_plugin_t* plugin,
                                 clap_id param_id,
                                 double value,
                                 char* text,
                                 uint32_t capacity)
{
    (void) plugin;
    if (param_id != 1 || text == NULL || capacity == 0)
        return false;
    snprintf(text, capacity, "%.3f", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t* plugin,
                                 clap_id param_id,
                                 const char* text,
                                 double* value)
{
    (void) plugin;
    if (param_id != 1 || text == NULL || value == NULL)
        return false;
    *value = strtod(text, NULL);
    return true;
}

static void params_flush(const clap_plugin_t* plugin,
                         const clap_input_events_t* input,
                         const clap_output_events_t* output)
{
    (void) output;
    const uint32_t count = input_event_count(input);
    for (uint32_t index = 0; index < count; ++index)
    {
        const clap_event_header_t* header = input->get(input, index);
        if (header != NULL
            && header->space_id == CLAP_CORE_EVENT_SPACE_ID
            && header->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* value =
                (const clap_event_param_value_t*) header;
            if (value->param_id == 1)
                self(plugin)->gain = value->value;
        }
    }
}

static int64_t state_write(const clap_ostream_t* stream,
                           const void* data,
                           uint64_t size)
{
    return stream->write(stream, data, size);
}

static int64_t state_read(const clap_istream_t* stream,
                          void* data,
                          uint64_t size)
{
    return stream->read(stream, data, size);
}

static bool state_save(const clap_plugin_t* plugin,
                       const clap_ostream_t* stream)
{
    const double gain = self(plugin)->gain;
    return state_write(stream, &gain, sizeof(gain)) == (int64_t) sizeof(gain);
}

static bool state_load(const clap_plugin_t* plugin,
                       const clap_istream_t* stream)
{
    double gain = 0.0;
    if (state_read(stream, &gain, sizeof(gain)) != (int64_t) sizeof(gain))
        return false;
    self(plugin)->gain = gain;
    return true;
}

static uint32_t latency_get(const clap_plugin_t* plugin)
{
    (void) plugin;
    return 0;
}

static uint32_t tail_get(const clap_plugin_t* plugin)
{
    (void) plugin;
    return 0;
}

static const clap_plugin_audio_ports_t audio_ports = {
    .count = audio_ports_count,
    .get = audio_ports_get
};

static const clap_plugin_params_t params = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush
};

static const clap_plugin_state_t state = {
    .save = state_save,
    .load = state_load
};

static const clap_plugin_latency_t latency = {
    .get = latency_get
};

static const clap_plugin_tail_t tail = {
    .get = tail_get
};

static const void* plugin_get_extension(const clap_plugin_t* plugin,
                                        const char* id)
{
    (void) plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audio_ports;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (strcmp(id, CLAP_EXT_STATE) == 0)
        return &state;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &latency;
    if (strcmp(id, CLAP_EXT_TAIL) == 0)
        return &tail;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t* plugin)
{
    (void) plugin;
}

static const clap_plugin_t* create_plugin(const clap_plugin_factory_t* factory,
                                          const clap_host_t* host,
                                          const char* plugin_id)
{
    (void) factory;
    (void) host;
    if (strcmp(plugin_id, descriptor.id) != 0)
        return NULL;
    test_plugin_t* instance = (test_plugin_t*) calloc(1, sizeof(*instance));
    if (instance == NULL)
        return NULL;
    instance->gain = 0.5;
    instance->plugin.desc = &descriptor;
    instance->plugin.plugin_data = instance;
    instance->plugin.init = plugin_init;
    instance->plugin.destroy = plugin_destroy;
    instance->plugin.activate = plugin_activate;
    instance->plugin.deactivate = plugin_deactivate;
    instance->plugin.start_processing = plugin_start_processing;
    instance->plugin.stop_processing = plugin_stop_processing;
    instance->plugin.reset = plugin_reset;
    instance->plugin.process = plugin_process;
    instance->plugin.get_extension = plugin_get_extension;
    instance->plugin.on_main_thread = plugin_on_main_thread;
    return &instance->plugin;
}

static uint32_t factory_count(const clap_plugin_factory_t* factory)
{
    (void) factory;
    return 1;
}

static const clap_plugin_descriptor_t* factory_descriptor(
    const clap_plugin_factory_t* factory,
    uint32_t index)
{
    (void) factory;
    return index == 0 ? &descriptor : NULL;
}

static const clap_plugin_factory_t factory = {
    .get_plugin_count = factory_count,
    .get_plugin_descriptor = factory_descriptor,
    .create_plugin = create_plugin
};

static bool entry_init(const char* plugin_path)
{
    return plugin_path != NULL;
}

static void entry_deinit(void)
{
}

static const void* entry_get_factory(const char* factory_id)
{
    return strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory
};
