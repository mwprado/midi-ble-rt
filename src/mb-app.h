#ifndef MB_APP_H
#define MB_APP_H

#include <alsa/asoundlib.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mb-config.h"

typedef struct {
    GDBusConnection *bus;
    GMainLoop *loop;

    MbConfig cfg;

    char *device_path;
    char *service_path;
    char *char_path;

    snd_seq_t *seq;
    int alsa_port;
    snd_midi_event_t *alsa_midi_encoder;
    snd_midi_event_t *alsa_midi_decoder;
    guint alsa_rx_source_id;

    uint8_t running_status;
} App;

void mb_app_cleanup(App *app);


char *mb_app_find_ble_midi_service(App *app);
char *mb_app_find_ble_midi_characteristic(App *app);

bool mb_app_alsa_init(App *app);
void mb_app_ble_midi_decode_packet(App *app, const uint8_t *p, size_t len);
void mb_app_print_midi_bytes(const char *prefix, const uint8_t *bytes, size_t len);

#endif /* MB_APP_H */
