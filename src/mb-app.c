#include "mb-app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "mb-alsa-port.h"
#include "mb-bluez.h"
#include "mb-gatt-midi.h"

static void mb_app_free_config(MbConfig *cfg) {
    mb_config_clear(cfg);
}






char *mb_app_find_ble_midi_service(App *app) {
    return mb_gatt_midi_find_service(app->bus, app->device_path, app->cfg.service_uuid);
}

char *mb_app_find_ble_midi_characteristic(App *app) {
    return mb_gatt_midi_find_characteristic(app->bus,
                                            app->device_path,
                                            app->service_path,
                                            app->cfg.io_uuid,
                                            app->cfg.io_uuid_alias);
}

bool mb_app_alsa_init(App *app) {
    return mb_alsa_port_open_duplex(&app->seq,
                                    &app->alsa_port,
                                    &app->alsa_midi_encoder,
                                    &app->alsa_midi_decoder,
                                    app->cfg.alsa_client_name,
                                    app->cfg.alsa_port_name);
}

static void alsa_emit_midi_byte(App *app, uint8_t byte) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);

    int r = snd_midi_event_encode_byte(app->alsa_midi_encoder, byte, &ev);
    if (r < 0) {
        g_printerr("ALSA MIDI encode failed for byte 0x%02x: %s\n", byte, snd_strerror(r));
        snd_midi_event_reset_encode(app->alsa_midi_encoder);
        return;
    }

    if (r > 0) {
        snd_seq_ev_set_source(&ev, app->alsa_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);

        int out = snd_seq_event_output_direct(app->seq, &ev);
        if (out < 0)
            g_printerr("snd_seq_event_output_direct failed: %s\n", snd_strerror(out));
    }
}

static void mb_app_emit_midi_byte(uint8_t byte, void *user_data) {
    alsa_emit_midi_byte((App *)user_data, byte);
}

void mb_app_ble_midi_decode_packet(App *app, const uint8_t *p, size_t len) {
    if (!app || !p || len < 3)
        return;

    if (app->cfg.print_ble_packets) {
        g_print("BLE packet:");
        for (size_t k = 0; k < len; k++)
            g_print(" %02x", p[k]);
        g_print("\n");
    }

    MbBleMidiDecodeResult result =
        mb_ble_midi_decode_packet(&app->ble_midi_decoder,
                                  p,
                                  len,
                                  mb_app_emit_midi_byte,
                                  app);

    if (result == MB_BLE_MIDI_DECODE_INVALID_HEADER)
        g_printerr("Invalid BLE-MIDI header byte: 0x%02x\n", p[0]);
}

void mb_app_print_midi_bytes(const char *prefix, const uint8_t *bytes, size_t len) {
    g_print("%s", prefix);
    for (size_t i = 0; i < len; i++)
        g_print(" %02x", bytes[i]);
    g_print("\n");
}

void mb_app_cleanup(App *app) {
    if (app->alsa_rx_source_id)
        g_source_remove(app->alsa_rx_source_id);
    mb_alsa_port_close(&app->seq,
                       &app->alsa_midi_encoder,
                       &app->alsa_midi_decoder);
    if (app->loop)
        g_main_loop_unref(app->loop);
    if (app->bus)
        g_object_unref(app->bus);

    g_free(app->device_path);
    g_free(app->service_path);
    g_free(app->char_path);
    mb_app_free_config(&app->cfg);
}
