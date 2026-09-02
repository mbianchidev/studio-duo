#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_plugin {
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_thread_check_t* thread_check;
    const clap_host_params_t* host_params;
    const clap_host_latency_t* host_latency;
    double parameters[301];
    uint32_t cookies[301];
    uint32_t thread_violations;
    uint32_t activation_count;
    uint32_t main_thread_callbacks;
    bool active;
    bool processing;
    bool requested_host_actions;
    bool requested_parameter_rescan;
    bool latency_change_requested;
    bool latency_change_reported;
    uint32_t metadata_epoch;
} test_plugin_t;

static const char* features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    NULL
};

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
    .features = features
};

static test_plugin_t* self(const clap_plugin_t* plugin)
{
    return (test_plugin_t*) plugin->plugin_data;
}

static void expect_main_thread(const clap_plugin_t* plugin)
{
    test_plugin_t* instance = self(plugin);
    if (instance->thread_check == NULL
        || !instance->thread_check->is_main_thread(instance->host))
        ++instance->thread_violations;
}

static void expect_audio_thread(const clap_plugin_t* plugin)
{
    test_plugin_t* instance = self(plugin);
    if (instance->thread_check == NULL
        || !instance->thread_check->is_audio_thread(instance->host))
        ++instance->thread_violations;
}

static bool plugin_init(const clap_plugin_t* plugin)
{
    expect_main_thread(plugin);
    return true;
}

static void plugin_destroy(const clap_plugin_t* plugin)
{
    expect_main_thread(plugin);
    free(self(plugin));
}

static bool plugin_activate(const clap_plugin_t* plugin,
                            double sample_rate,
                            uint32_t min_frames,
                            uint32_t max_frames)
{
    test_plugin_t* instance = self(plugin);
    expect_main_thread(plugin);
    instance->active = true;
    ++instance->activation_count;
    return sample_rate > 0.0 && min_frames > 0
        && max_frames >= min_frames;
}

static void plugin_deactivate(const clap_plugin_t* plugin)
{
    expect_main_thread(plugin);
    self(plugin)->active = false;
}

static bool plugin_start_processing(const clap_plugin_t* plugin)
{
    expect_audio_thread(plugin);
    self(plugin)->processing = true;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t* plugin)
{
    expect_audio_thread(plugin);
    self(plugin)->processing = false;
}

static void plugin_reset(const clap_plugin_t* plugin)
{
    expect_audio_thread(plugin);
}

static uint32_t input_event_count(const clap_input_events_t* events)
{
    return events != NULL ? events->size(events) : 0;
}

static clap_process_status plugin_process(const clap_plugin_t* plugin,
                                          const clap_process_t* process)
{
    test_plugin_t* instance = self(plugin);
    expect_audio_thread(plugin);
    if (!instance->requested_host_actions)
    {
        instance->requested_host_actions = true;
        instance->host->request_callback(instance->host);
        instance->host->request_process(instance->host);
        instance->host->request_restart(instance->host);
    }
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
                if (value->param_id >= 1
                    && value->param_id <= 301
                    && value->cookie
                        != &instance->cookies[value->param_id - 1])
                    ++instance->thread_violations;
                if (value->param_id == 1)
                    instance->parameters[0] = value->value;
                else if (value->param_id >= 2
                         && (value->param_id <= 298
                             || value->param_id == 301))
                    instance->parameters[value->param_id - 1] =
                        value->value;
                if (value->param_id == 293
                    && value->value > 0.9
                    && !instance->latency_change_requested)
                {
                    instance->latency_change_requested = true;
                    instance->host->request_callback(
                        instance->host);
                }
            }
            ++event_index;
        }

        for (uint32_t port = 0; port < process->audio_outputs_count; ++port)
        {
            clap_audio_buffer_t* output = &process->audio_outputs[port];
            for (uint32_t channel = 0; channel < output->channel_count; ++channel)
                output->data32[channel][frame] = 0.0f;
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
                output->data32[channel][frame] +=
                    source * (float) instance->parameters[0];
            }
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t* plugin, bool is_input)
{
    expect_main_thread(plugin);
    (void) is_input;
    return 1;
}

static bool audio_ports_get(const clap_plugin_t* plugin,
                            uint32_t index,
                            bool is_input,
                            clap_audio_port_info_t* info)
{
    expect_main_thread(plugin);
    if (index != 0 || info == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = is_input ? 10 : 20;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = is_input ? 2 : 1;
    info->port_type = is_input ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static uint32_t params_count(const clap_plugin_t* plugin)
{
    expect_main_thread(plugin);
    return 300;
}

static clap_id parameter_id_for_index(
    const clap_plugin_t* plugin,
    uint32_t index)
{
    if (self(plugin)->metadata_epoch == 0)
        return index + 1;
    if (index < 249)
        return index + 1;
    if (index < 299)
        return index + 2;
    return 301;
}

static bool params_get_info(const clap_plugin_t* plugin,
                            uint32_t index,
                            clap_param_info_t* info)
{
    expect_main_thread(plugin);
    if (index >= params_count(plugin) || info == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    const clap_id parameter_id =
        parameter_id_for_index(plugin, index);
    info->id = parameter_id;
    info->flags = parameter_id == 299
            || parameter_id == 300
        ? CLAP_PARAM_IS_READONLY
        : CLAP_PARAM_IS_AUTOMATABLE;
    if (parameter_id == 298)
        info->flags |= CLAP_PARAM_REQUIRES_PROCESS;
    if (parameter_id == 1)
        snprintf(
            info->name,
            sizeof(info->name),
            "%s",
            self(plugin)->metadata_epoch > 0
                ? "Gain rescanned"
                : "Gain");
    else if (parameter_id == 299)
        snprintf(info->name, sizeof(info->name), "Main thread callbacks");
    else if (parameter_id == 300)
        snprintf(info->name, sizeof(info->name), "Thread violations");
    else if (parameter_id == 301)
        snprintf(info->name, sizeof(info->name), "Added parameter");
    else
        snprintf(info->name, sizeof(info->name), "Parameter %u", parameter_id);
    info->min_value = parameter_id == 2 ? -24.0 : 0.0;
    info->max_value = parameter_id == 2
        ? 24.0
        : parameter_id == 299 || parameter_id == 300
        ? 1000.0
        : 1.0;
    info->default_value = parameter_id == 2
        ? 0.0
        : parameter_id == 299 || parameter_id == 300
        ? 0.0
        : 0.5;
    info->cookie =
        &self(plugin)->cookies[parameter_id - 1];
    return true;
}

static bool params_get_value(const clap_plugin_t* plugin,
                             clap_id param_id,
                             double* value)
{
    expect_main_thread(plugin);
    if (param_id < 1 || param_id > 301
        || (self(plugin)->metadata_epoch > 0
            && param_id == 250)
        || value == NULL)
        return false;
    if (param_id == 299)
        *value = self(plugin)->main_thread_callbacks;
    else if (param_id == 300)
        *value = self(plugin)->thread_violations;
    else if (param_id <= 298)
        *value = self(plugin)->parameters[param_id - 1];
    else
        *value = self(plugin)->parameters[param_id - 1];
    return true;
}

static bool params_value_to_text(const clap_plugin_t* plugin,
                                 clap_id param_id,
                                 double value,
                                 char* text,
                                 uint32_t capacity)
{
    expect_main_thread(plugin);
    if (param_id < 1 || param_id > 301
        || (self(plugin)->metadata_epoch > 0
            && param_id == 250)
        || text == NULL || capacity == 0)
        return false;
    snprintf(text, capacity, "%.3f", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t* plugin,
                                 clap_id param_id,
                                 const char* text,
                                 double* value)
{
    expect_main_thread(plugin);
    if (param_id < 1 || param_id > 298
        || text == NULL || value == NULL)
        return false;
    *value = strtod(text, NULL);
    return true;
}

static void params_flush(const clap_plugin_t* plugin,
                         const clap_input_events_t* input,
                         const clap_output_events_t* output)
{
    (void) output;
    if (self(plugin)->active)
        expect_audio_thread(plugin);
    else
        expect_main_thread(plugin);
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
            if (value->param_id >= 1
                && value->param_id <= 301
                && value->cookie
                    != &self(plugin)->cookies[value->param_id - 1])
                ++self(plugin)->thread_violations;
            if (value->param_id == 298)
                ++self(plugin)->thread_violations;
            if (value->param_id >= 1 && value->param_id <= 298)
                self(plugin)->parameters[value->param_id - 1] =
                    value->value;
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
    expect_main_thread(plugin);
    if (self(plugin)->parameters[295] > 0.9)
        return false;
    const auto bytes = self(plugin)->parameters[296] > 0.9
        ? sizeof(self(plugin)->parameters) / 2
        : sizeof(self(plugin)->parameters);
    return state_write(
               stream,
               self(plugin)->parameters,
               bytes)
        == (int64_t) bytes;
}

static bool state_load(const clap_plugin_t* plugin,
                       const clap_istream_t* stream)
{
    expect_main_thread(plugin);
    double parameters[301];
    if (state_read(stream, parameters, sizeof(parameters))
        != (int64_t) sizeof(parameters))
        return false;
    if (parameters[294] > 0.9)
    {
        self(plugin)->parameters[0] = 0.125;
        return false;
    }
    memcpy(self(plugin)->parameters, parameters, sizeof(parameters));
    return true;
}

static uint32_t latency_get(const clap_plugin_t* plugin)
{
    expect_main_thread(plugin);
    if (self(plugin)->latency_change_reported)
        return 64;
    return self(plugin)->activation_count > 1 ? 32 : 0;
}

static uint32_t tail_get(const clap_plugin_t* plugin)
{
    expect_main_thread(plugin);
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
    expect_main_thread(plugin);
    ++self(plugin)->main_thread_callbacks;
    if (!self(plugin)->requested_parameter_rescan
        && self(plugin)->host_params != NULL)
    {
        self(plugin)->requested_parameter_rescan = true;
        self(plugin)->metadata_epoch = 1;
        self(plugin)->host_params->rescan(
            self(plugin)->host,
            CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL);
    }
    if (self(plugin)->latency_change_requested
        && !self(plugin)->latency_change_reported
        && self(plugin)->host_latency != NULL)
    {
        self(plugin)->latency_change_reported = true;
        self(plugin)->host_latency->changed(
            self(plugin)->host);
    }
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
    instance->host = host;
    instance->thread_check =
        (const clap_host_thread_check_t*) host->get_extension(
            host,
            CLAP_EXT_THREAD_CHECK);
    instance->host_params =
        (const clap_host_params_t*) host->get_extension(
            host,
            CLAP_EXT_PARAMS);
    instance->host_latency =
        (const clap_host_latency_t*) host->get_extension(
            host,
            CLAP_EXT_LATENCY);
    for (uint32_t index = 0; index < 301; ++index)
    {
        instance->cookies[index] = index + 1;
    }
    for (uint32_t index = 0; index < 298; ++index)
        instance->parameters[index] = 0.5;
    instance->parameters[1] = 0.0;
    if (instance->thread_check == NULL
        || !instance->thread_check->is_main_thread(host))
        ++instance->thread_violations;
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
