/*
 * midi-ble-rtd
 *
 * BLE-MIDI/GATT -> ALSA Sequencer bridge.
 *
 * Current implementation:
 *   - config file via GLib GKeyFile
 *   - BlueZ Device1 connect/trust
 *   - direct GATT discovery
 *   - official BLE-MIDI I/O UUID and Roland 00006bf3 alias
 *   - StartNotify via D-Bus
 *   - minimal BLE-MIDI decoder
 *   - ALSA Sequencer duplex port
 *   - ALSA -> BLE-MIDI WriteValue path for short MIDI messages
 *
 * Future:
 *   - Agent1
 *   - AcquireNotify
 *   - RT thread / mlockall / jitter metrics
 */

#include <alsa/asoundlib.h>
#include <gio/gio.h>
#include <glib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mb-bluez.h"
#include "mb-alsa-port.h"
#include "mb-alsa.h"
#include "mb-gatt-midi.h"

typedef struct {
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
} Config;

typedef struct {
    GDBusConnection *bus;
    GMainLoop *loop;

    Config cfg;

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

static char *keyfile_get_string_default(GKeyFile *kf, const char *group, const char *key, const char *fallback) {
    GError *error = NULL;
    char *value = g_key_file_get_string(kf, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return g_strdup(fallback);
    }
    return value;
}

static bool keyfile_get_bool_default(GKeyFile *kf, const char *group, const char *key, bool fallback) {
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(kf, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return fallback;
    }
    return value;
}

static bool load_config(Config *cfg, const char *path) {
    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
        g_printerr("Failed to load config %s: %s\n", path, error->message);
        g_clear_error(&error);
        g_key_file_unref(kf);
        return false;
    }

    cfg->address = keyfile_get_string_default(kf, "device", "address", "");
    cfg->name = keyfile_get_string_default(kf, "device", "name", "");
    cfg->pair = keyfile_get_bool_default(kf, "device", "pair", false);
    cfg->trust = keyfile_get_bool_default(kf, "device", "trust", true);
    cfg->auto_reconnect = keyfile_get_bool_default(kf, "device", "auto_reconnect", true);

    cfg->service_uuid = keyfile_get_string_default(kf, "gatt", "service_uuid",
        "03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    cfg->io_uuid = keyfile_get_string_default(kf, "gatt", "io_uuid",
        "7772e5db-3868-4112-a1a9-f2669d106bf3");
    cfg->io_uuid_alias = keyfile_get_string_default(kf, "gatt", "io_uuid_alias",
        "00006bf3-0000-1000-8000-00805f9b34fb");
    cfg->require_notify = keyfile_get_bool_default(kf, "gatt", "require_notify", true);
    cfg->require_write_without_response = keyfile_get_bool_default(kf, "gatt", "require_write_without_response", true);

    cfg->alsa_client_name = keyfile_get_string_default(kf, "alsa", "client_name", "midi-ble-rt");
    cfg->alsa_port_name = keyfile_get_string_default(kf, "alsa", "port_name", "BLE-MIDI In");

    cfg->print_ble_packets = keyfile_get_bool_default(kf, "debug", "print_ble_packets", false);
    cfg->print_midi_events = keyfile_get_bool_default(kf, "debug", "print_midi_events", false);
    cfg->enable_tx = keyfile_get_bool_default(kf, "midi", "enable_tx", true);

    g_key_file_unref(kf);
    return true;
}

static void free_config(Config *cfg) {
    g_free(cfg->address);
    g_free(cfg->name);
    g_free(cfg->service_uuid);
    g_free(cfg->io_uuid);
    g_free(cfg->io_uuid_alias);
    g_free(cfg->alsa_client_name);
    g_free(cfg->alsa_port_name);
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

static GVariant *get_managed_objects(App *app) {
    return mb_bluez_get_managed_objects(app->bus);
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

static char *find_device(App *app) {
    GVariant *objects = get_managed_objects(app);
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

static bool get_device_bool_property(App *app, const char *property, bool *out) {
    return mb_bluez_get_device_bool_property(app->bus, app->device_path, property, out);
}

static bool set_device_trusted(App *app) {
    return mb_bluez_set_device_trusted(app->bus, app->device_path);
}

static bool pair_device(App *app) {
    return mb_bluez_pair_device(app->bus, app->device_path);
}

static bool connect_device(App *app) {
    return mb_bluez_connect_device(app->bus, app->device_path);
}

static bool wait_services_resolved(App *app, int timeout_ms) {
    return mb_bluez_wait_services_resolved(app->bus, app->device_path, timeout_ms);
}

static char *find_ble_midi_service(App *app) {
    return mb_gatt_midi_find_service(app->bus, app->device_path, app->cfg.service_uuid);
}

static char *find_ble_midi_characteristic(App *app) {
    return mb_gatt_midi_find_characteristic(app->bus,
                                            app->device_path,
                                            app->service_path,
                                            app->cfg.io_uuid,
                                            app->cfg.io_uuid_alias);
}

static bool alsa_init(App *app) {
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

static void ble_midi_decode_packet(App *app, const uint8_t *p, size_t len) {
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

static uint16_t ble_midi_timestamp_13bit(void) {
    gint64 ms = g_get_monotonic_time() / 1000;
    return (uint16_t)(ms & 0x1fff);
}

static bool ble_midi_write_packet(App *app, const uint8_t *midi, size_t len) {
    if (!midi || len == 0)
        return true;

    const size_t max_midi_per_packet = 18; /* safe for 20-byte ATT payloads */
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > max_midi_per_packet)
            chunk = max_midi_per_packet;

        uint8_t packet[20];
        uint16_t ts = ble_midi_timestamp_13bit();
        packet[0] = 0x80 | ((ts >> 7) & 0x3f);
        packet[1] = 0x80 | (ts & 0x7f);
        memcpy(&packet[2], &midi[off], chunk);
        size_t packet_len = chunk + 2;

        if (app->cfg.print_ble_packets) {
            g_print("BLE write:");
            for (size_t i = 0; i < packet_len; i++)
                g_print(" %02x", packet[i]);
            g_print("\n");
        }

        if (!mb_gatt_midi_write_value_command(app->bus,
                                               app->char_path,
                                               packet,
                                               packet_len,
                                               5000)) {
            return false;
        }
        off += chunk;
    }

    return true;
}

static void print_midi_bytes(const char *prefix, const uint8_t *bytes, size_t len) {
    g_print("%s", prefix);
    for (size_t i = 0; i < len; i++)
        g_print(" %02x", bytes[i]);
    g_print("\n");
}

static gboolean alsa_rx_poll_cb(gpointer user_data) {
    App *app = user_data;

    if (!app->cfg.enable_tx)
        return G_SOURCE_CONTINUE;

    for (;;) {
        int pending = snd_seq_event_input_pending(app->seq, 1);
        if (pending <= 0)
            break;

        snd_seq_event_t *ev = NULL;
        int r = snd_seq_event_input(app->seq, &ev);
        if (r < 0) {
            if (r != -EAGAIN)
                g_printerr("snd_seq_event_input failed: %s\n", snd_strerror(r));
            break;
        }
        if (!ev)
            break;

        if (!mb_alsa_event_is_midi_payload(ev)) {
            if (app->cfg.print_midi_events)
                g_print("Ignoring ALSA Sequencer control event type %d (%s).\n",
                        ev->type, mb_alsa_event_type_name(ev->type));
            continue;
        }

        uint8_t midi[1024];
        long n = snd_midi_event_decode(app->alsa_midi_decoder, midi, sizeof(midi), ev);
        if (n < 0) {
            g_printerr("ALSA MIDI decode failed for event type %d: %s\n", ev->type, snd_strerror((int)n));
            snd_midi_event_reset_decode(app->alsa_midi_decoder);
            continue;
        }
        if (n == 0)
            continue;

        if (app->cfg.print_midi_events)
            print_midi_bytes("ALSA->BLE MIDI:", midi, (size_t)n);

        ble_midi_write_packet(app, midi, (size_t)n);
    }

    return G_SOURCE_CONTINUE;
}

static void on_properties_changed(GDBusConnection *connection, const gchar *sender_name,
                                  const gchar *object_path, const gchar *interface_name,
                                  const gchar *signal_name, GVariant *parameters,
                                  gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    App *app = user_data;
    const char *iface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, &invalidated);

    if (g_strcmp0(iface, GATT_CHRC_IFACE) == 0) {
        GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (value) {
            gsize len = 0;
            const guint8 *bytes = g_variant_get_fixed_array(value, &len, sizeof(guint8));
            ble_midi_decode_packet(app, bytes, len);
            g_variant_unref(value);
        }
    }

    if (changed) g_variant_unref(changed);
    if (invalidated) g_variant_unref(invalidated);
}

static bool start_notify(App *app) {
    GError *error = NULL;

    guint sub_id = g_dbus_connection_signal_subscribe(
        app->bus, BLUEZ_BUS, PROPERTIES_IFACE, "PropertiesChanged",
        app->char_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed, app, NULL);

    if (sub_id == 0) {
        g_printerr("Failed to subscribe to PropertiesChanged.\n");
        return false;
    }

    g_print("Subscribing to BlueZ PropertiesChanged for %s.\n", app->char_path);
    g_print("Calling StartNotify on %s.\n", app->char_path);

    GVariant *ret = g_dbus_connection_call_sync(
        app->bus, BLUEZ_BUS, app->char_path, GATT_CHRC_IFACE, "StartNotify",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, &error);

    if (!ret) {
        g_printerr("StartNotify failed: %s\n", error->message);
        g_printerr("If authorization is required, pair/trust once with bluetoothctl or implement Agent1.\n");
        g_printerr("If TX works but RX is silent after reconnects, restart bluetooth and midi-ble-rtd to clear stale BlueZ GATT state.\n");
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("StartNotify ok on %s. ALSA MIDI port is ready.\n", app->char_path);
    return true;
}

static void app_cleanup(App *app) {
    if (app->alsa_rx_source_id)
        g_source_remove(app->alsa_rx_source_id);
    if (app->alsa_midi_decoder)
        snd_midi_event_free(app->alsa_midi_decoder);
    if (app->alsa_midi_encoder)
        snd_midi_event_free(app->alsa_midi_encoder);
    if (app->seq)
        snd_seq_close(app->seq);
    if (app->loop)
        g_main_loop_unref(app->loop);
    if (app->bus)
        g_object_unref(app->bus);

    g_free(app->device_path);
    g_free(app->service_path);
    g_free(app->char_path);
    free_config(&app->cfg);
}

static void print_usage(const char *argv0) {
    g_printerr("Usage: %s --config PATH\n", argv0);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!config_path) {
        print_usage(argv[0]);
        return 2;
    }

    App app = {0};

    if (!load_config(&app.cfg, config_path))
        return 1;

    g_print("midi-ble-rtd\n");
    g_print("Config: %s\n", config_path);
    g_print("Address: %s\n", app.cfg.address);
    g_print("Service UUID: %s\n", app.cfg.service_uuid);
    g_print("I/O UUID: %s\n", app.cfg.io_uuid);
    g_print("I/O alias: %s\n", app.cfg.io_uuid_alias);
    g_print("GATT decides; Name/Alias is diagnostic only.\n");
    g_print("TX ALSA->BLE: %s\n", app.cfg.enable_tx ? "enabled" : "disabled");

    GError *error = NULL;
    app.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!app.bus) {
        g_printerr("Could not connect to system bus: %s\n", error->message);
        g_clear_error(&error);
        app_cleanup(&app);
        return 1;
    }

    app.device_path = find_device(&app);
    if (!app.device_path) {
        g_printerr("No matching BlueZ Device1 found. Scan/connect once or check the address.\n");
        app_cleanup(&app);
        return 1;
    }

    if (app.cfg.pair && !pair_device(&app)) {
        app_cleanup(&app);
        return 1;
    }

    if (app.cfg.trust)
        set_device_trusted(&app);

    if (!connect_device(&app)) {
        app_cleanup(&app);
        return 1;
    }

    if (!wait_services_resolved(&app, 15000)) {
        app_cleanup(&app);
        return 1;
    }

    app.service_path = find_ble_midi_service(&app);
    if (!app.service_path) {
        g_printerr("BLE-MIDI service not found. Connect MIDI before Audio on Roland GO:KEYS.\n");
        app_cleanup(&app);
        return 1;
    }

    app.char_path = find_ble_midi_characteristic(&app);
    if (!app.char_path) {
        g_printerr("BLE-MIDI I/O characteristic not found.\n");
        app_cleanup(&app);
        return 1;
    }

    if (!alsa_init(&app)) {
        app_cleanup(&app);
        return 1;
    }

    if (!start_notify(&app)) {
        app_cleanup(&app);
        return 1;
    }

    if (app.cfg.enable_tx)
        app.alsa_rx_source_id = g_timeout_add(1, alsa_rx_poll_cb, &app);

    app.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app.loop);

    app_cleanup(&app);
    return 0;
}
