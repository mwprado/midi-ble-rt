#ifndef MB_CONFIG_H
#define MB_CONFIG_H

#include <glib.h>
#include <stdbool.h>

typedef struct MbDeviceConfig {
    char *id;
    char *address;
    char *name;
    char *profile;
    char *alsa_port_name;
    bool enabled;
    bool connect_on_start;
    bool pair;
    bool trust;
    bool reconnect_on_link_loss;
    bool enable_tx;
} MbDeviceConfig;

typedef struct MbConfig {
    char *address;
    char *name;
    bool pair;
    bool trust;
    bool reconnect_on_link_loss;
    char *service_uuid;
    char *io_uuid;
    char *io_uuid_alias;
    bool require_notify;
    bool require_write_without_response;
    char *alsa_client_name;
    char *alsa_port_name;
    bool print_ble_packets;
    bool print_midi_events;
    bool enable_tx;
    bool stats_enabled;
    bool stats_latency_diagnostics;
    unsigned stats_interval_ms;
    bool rtkit_enabled;
    unsigned rtkit_priority;

    GPtrArray *devices; /* element type: MbDeviceConfig* */
} MbConfig;

bool mb_config_load_dir(MbConfig *cfg, const char *dir_path);
void mb_config_clear(MbConfig *cfg);

unsigned mb_config_device_count(const MbConfig *cfg);
const MbDeviceConfig *mb_config_get_device(const MbConfig *cfg, unsigned index);

#endif
