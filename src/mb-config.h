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
    bool autoconnect;
    bool pair;
    bool trust;
    bool auto_reconnect;
    bool enable_tx;
} MbDeviceConfig;

typedef struct MbConfig {
    char *address;
    char *name;
    bool pair;
    bool trust;
    bool auto_reconnect;
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
    unsigned stats_interval_ms;

    /*
     * Multi-device configuration model.
     *
     * Existing single-device configs using [device] remain valid. When no
     * devices.d/*.ini entries are loaded, a single compatibility device is
     * synthesized from [device] if address is set.
     */
    GPtrArray *devices; /* element type: MbDeviceConfig* */
} MbConfig;

bool mb_config_load(MbConfig *cfg, const char *path);
bool mb_config_load_dir(MbConfig *cfg, const char *dir_path);
void mb_config_clear(MbConfig *cfg);

unsigned mb_config_device_count(const MbConfig *cfg);
const MbDeviceConfig *mb_config_get_device(const MbConfig *cfg, unsigned index);

#endif
