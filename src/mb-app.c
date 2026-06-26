#include "mb-app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "mb-alsa-port.h"
#include "mb-bluez.h"
#include "mb-gatt-midi.h"

bool mb_app_load_config(Config *cfg, const char *path) {
    return mb_config_load((MbConfig *)cfg, path);
}

static void mb_app_free_config(Config *cfg) {
    mb_config_clear((MbConfig *)cfg);
}

static bool uuid_equal(const char *a, const char *b) {
    return a && b && g_ascii_strcasecmp(a, b) == 0;
}

static bool string_contains_casefold(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle)
        return false;

    char *h = g_ascii_strdown(haystack, -1);
    char *n = g_ascii_strdown(needle, -1);
    bool ok = strstr(h, n) != NULL;
    g_free(h);
    g_free(n);
    return ok;
}

static bool string_array_contains_uuid(GVariant *array, const char *uuid) {
    if (!array)
        return false;

    GVariantIter iter;
    const char *s = NULL;

    g_variant_iter_init(&iter, array);
    while (g_variant_iter_next(&iter, "&s", &s)) {
        if (uuid_equal(s, uuid))
            return true;
    }

    return false;
}

char *mb_app_find_device(App *app) {
    GVariant *objects = mb_bluez_get_managed_objects(app->bus);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    char *best = NULL;
    int best_score = -1;

    g_variant_iter_init(&iter, objects);

    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces, DEVICE_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *v_address = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
        GVariant *v_name = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
        GVariant *v_alias = g_variant_lookup_value(props, "Alias", G_VARIANT_TYPE_STRING);
        GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

        const char *address = v_address ? g_variant_get_string(v_address, NULL) : NULL;
        const char *name = v_name ? g_variant_get_string(v_name, NULL) : NULL;
        const char *alias = v_alias ? g_variant_get_string(v_alias, NULL) : NULL;

        bool exact_address = app->cfg.address && *app->cfg.address &&
            address && g_ascii_strcasecmp(address, app->cfg.address) == 0;
        bool name_match = app->cfg.name && *app->cfg.name &&
            ((name && string_contains_casefold(name, app->cfg.name)) ||
             (alias && string_contains_casefold(alias, app->cfg.name)));
        bool has_service_uuid = string_array_contains_uuid(v_uuids, app->cfg.service_uuid);

        if (exact_address || name_match || has_service_uuid) {
            int score = 0;
            if (exact_address) score += 10000;
            if (has_service_uuid) score += 1000;
            if (name && string_contains_casefold(name, "midi")) score += 100;
            if (alias && string_contains_casefold(alias, "midi")) score += 100;

            g_print("Candidate Device1: %s\n", path);
            g_print("  Address: %s\n", address ? address : "(none)");
            g_print("  Name:    %s\n", name ? name : "(none)");
            g_print("  Alias:   %s\n", alias ? alias : "(none)");
            g_print("  BLE-MIDI UUID in Device1.UUIDs: %s\n", has_service_uuid ? "yes" : "no");
            g_print("  Score: %d\n", score);

            if (score > best_score) {
                g_free(best);
                best = g_strdup(path);
                best_score = score;
            }
        }

        if (v_address) g_variant_unref(v_address);
        if (v_name) g_variant_unref(v_name);
        if (v_alias) g_variant_unref(v_alias);
        if (v_uuids) g_variant_unref(v_uuids);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);

    if (best)
        g_print("Selected Device1: %s\n", best);

    return best;
}

bool mb_app_get_device_bool_property(App *app, const char *property, bool *out) {
    return mb_bluez_get_device_bool_property(app->bus, app->device_path, property, out);
}

bool mb_app_set_device_trusted(App *app) {
    return mb_bluez_set_device_trusted(app->bus, app->device_path);
}

bool mb_app_pair_device(App *app) {
    return mb_bluez_pair_device(app->bus, app->device_path);
}

bool mb_app_connect_device(App *app) {
    return mb_bluez_connect_device(app->bus, app->device_path);
}

bool mb_app_wait_services_resolved(App *app, int timeout_ms) {
    return mb_bluez_wait_services_resolved(app->bus, app->device_path, timeout_ms);
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
