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

static int midi_data_len(uint8_t status) {
    if (status < 0x80) return -1;
    if (status >= 0x80 && status <= 0xBF) return 2;
    if (status >= 0xC0 && status <= 0xDF) return 1;
    if (status >= 0xE0 && status <= 0xEF) return 2;

    switch (status) {
        case 0xF1:
        case 0xF3:
            return 1;
        case 0xF2:
            return 2;
        case 0xF6:
        case 0xF7:
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            return 0;
        case 0xF0:
            return -2;
        default:
            return -1;
    }
}

void mb_app_ble_midi_decode_packet(App *app, const uint8_t *p, size_t len) {
    if (!p || len < 3)
        return;

    if (app->cfg.print_ble_packets) {
        g_print("BLE packet:");
        for (size_t k = 0; k < len; k++)
            g_print(" %02x", p[k]);
        g_print("\n");
    }

    if ((p[0] & 0x80) == 0) {
        g_printerr("Invalid BLE-MIDI header byte: 0x%02x\n", p[0]);
        return;
    }

    size_t i = 1;

    while (i < len) {
        uint8_t ts = p[i];

        if ((ts & 0x80) == 0) {
            i++;
            continue;
        }

        i++;
        if (i >= len)
            break;

        uint8_t status = p[i];

        if (status < 0x80) {
            if (app->running_status >= 0x80) {
                int n = midi_data_len(app->running_status);
                if (n > 0 && i + (size_t)n <= len) {
                    alsa_emit_midi_byte(app, app->running_status);
                    for (int j = 0; j < n; j++)
                        alsa_emit_midi_byte(app, p[i++]);
                    continue;
                }
            }
            i++;
            continue;
        }

        i++;
        alsa_emit_midi_byte(app, status);

        if (status < 0xF0)
            app->running_status = status;
        else if (status != 0xF7)
            app->running_status = 0;

        int data_len = midi_data_len(status);

        if (data_len >= 0) {
            for (int j = 0; j < data_len && i < len; j++) {
                if (p[i] & 0x80)
                    break;
                alsa_emit_midi_byte(app, p[i++]);
            }
            continue;
        }

        if (data_len == -2) {
            while (i < len) {
                alsa_emit_midi_byte(app, p[i]);
                if (p[i] == 0xF7) {
                    i++;
                    break;
                }
                i++;
            }
        }
    }
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
