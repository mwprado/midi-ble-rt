#ifndef MB_CONFIG_H
#define MB_CONFIG_H

#include <stdbool.h>

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
} MbConfig;

bool mb_config_load(MbConfig *cfg, const char *path);
void mb_config_clear(MbConfig *cfg);

#endif
