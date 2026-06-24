/*
 * Runtime/session orchestrator for midi-ble-rtd.
 *
 * This layer owns the threaded duplex data path.  It currently reuses the
 * legacy daemon core by including midi-ble-rtd.c with its main() renamed; the
 * next refactor step is to move those lower-level static helpers into explicit
 * core modules.  The public binary is now a single daemon: midi-ble-rtd.
 */

#include "mb-orchestrator.h"

#define main midi_ble_rtd_legacy_main
#include "midi-ble-rtd.c"
#undef main

#include "mb-alsa.h"
#include "mb-ble-midi.h"
#include "mb-config.h"
#include "mb-duplex-runtime.h"

typedef struct {
    App app;
    MbDuplexRuntime runtime;
    GMutex alsa_lock;
    GMutex gatt_write_lock;
    guint notify_sub_id;
    bool runtime_started;
} MbOrchestrator;

static uint64_t orchestrator_now_ns(void) {
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static uint16_t orchestrator_ble_midi_timestamp_13bit(void) {
    gint64 ms = g_get_monotonic_time() / 1000;
    return (uint16_t)(ms & 0x1fff);
}

static bool orchestrator_load_config(Config *cfg, const char *path) {
    return mb_config_load((MbConfig *)cfg, path);
}

static bool orchestrator_ble_midi_write_packet(App *app, const uint8_t *midi, size_t len) {
    if (!midi || len == 0)
        return true;

    const size_t max_midi_per_packet = 18;
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > max_midi_per_packet)
            chunk = max_midi_per_packet;

        uint8_t packet[20];
        size_t packet_len = 0;
        if (!mb_ble_midi_make_packet(orchestrator_ble_midi_timestamp_13bit(),
                                     &midi[off], chunk,
                                     packet, sizeof(packet), &packet_len)) {
            g_printerr("BLE-MIDI packet construction failed.\n");
            return false;
        }

        if (app->cfg.print_ble_packets) {
            g_print("BLE write:");
            for (size_t i = 0; i < packet_len; i++)
                g_print(" %02x", packet[i]);
            g_print("\n");
        }

        GVariant *value = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                    packet, packet_len,
                                                    sizeof(guint8));
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string("command"));

        GError *error = NULL;
        GVariant *ret = g_dbus_connection_call_sync(
            app->bus, BLUEZ_BUS, app->char_path, GATT_CHRC_IFACE, "WriteValue",
            g_variant_new("(@aya{sv})", value, &options),
            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

        if (!ret) {
            g_printerr("WriteValue failed: %s\n", error->message);
            g_clear_error(&error);
            return false;
        }

        g_variant_unref(ret);
        off += chunk;
    }

    return true;
}

static void orchestrator_rx_consume(MbRuntimeFlow *flow,
                                    const MbSliceRingItem *item,
                                    const uint8_t *data,
                                    size_t len,
                                    void *user_data) {
    (void)flow;
    (void)item;
    MbOrchestrator *orc = user_data;

    g_mutex_lock(&orc->alsa_lock);
    ble_midi_decode_packet(&orc->app, data, len);
    g_mutex_unlock(&orc->alsa_lock);
}

static void orchestrator_tx_consume(MbRuntimeFlow *flow,
                                    const MbSliceRingItem *item,
                                    const uint8_t *data,
                                    size_t len,
                                    void *user_data) {
    (void)flow;
    (void)item;
    MbOrchestrator *orc = user_data;

    g_mutex_lock(&orc->gatt_write_lock);
    orchestrator_ble_midi_write_packet(&orc->app, data, len);
    g_mutex_unlock(&orc->gatt_write_lock);
}

static gboolean orchestrator_alsa_rx_poll_cb(gpointer user_data) {
    MbOrchestrator *orc = user_data;
    App *app = &orc->app;

    if (!app->cfg.enable_tx)
        return G_SOURCE_CONTINUE;

    for (;;) {
        g_mutex_lock(&orc->alsa_lock);
        int pending = snd_seq_event_input_pending(app->seq, 1);
        if (pending <= 0) {
            g_mutex_unlock(&orc->alsa_lock);
            break;
        }

        snd_seq_event_t *ev = NULL;
        int r = snd_seq_event_input(app->seq, &ev);
        if (r < 0) {
            if (r != -EAGAIN)
                g_printerr("snd_seq_event_input failed: %s\n", snd_strerror(r));
            g_mutex_unlock(&orc->alsa_lock);
            break;
        }
        if (!ev) {
            g_mutex_unlock(&orc->alsa_lock);
            break;
        }

        if (!mb_alsa_event_is_midi_payload(ev)) {
            if (app->cfg.print_midi_events)
                g_print("Ignoring ALSA Sequencer control event type %d (%s).\n",
                        ev->type, mb_alsa_event_type_name(ev->type));
            g_mutex_unlock(&orc->alsa_lock);
            continue;
        }

        uint8_t midi[1024];
        long n = snd_midi_event_decode(app->alsa_midi_decoder, midi, sizeof(midi), ev);
        if (n < 0) {
            g_printerr("ALSA MIDI decode failed for event type %d: %s\n", ev->type, snd_strerror((int)n));
            snd_midi_event_reset_decode(app->alsa_midi_decoder);
            g_mutex_unlock(&orc->alsa_lock);
            continue;
        }
        g_mutex_unlock(&orc->alsa_lock);

        if (n == 0)
            continue;

        if (app->cfg.print_midi_events)
            print_midi_bytes("ALSA->BLE MIDI:", midi, (size_t)n);

        if (!mb_duplex_runtime_push_tx(&orc->runtime, midi, (size_t)n, orchestrator_now_ns()))
            g_printerr("TX runtime queue push failed; ALSA event dropped.\n");
    }

    return G_SOURCE_CONTINUE;
}

static void orchestrator_on_properties_changed(GDBusConnection *connection,
                                               const gchar *sender_name,
                                               const gchar *object_path,
                                               const gchar *interface_name,
                                               const gchar *signal_name,
                                               GVariant *parameters,
                                               gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    MbOrchestrator *orc = user_data;
    const char *iface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, &invalidated);

    if (g_strcmp0(iface, GATT_CHRC_IFACE) == 0) {
        GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (value) {
            gsize len = 0;
            const guint8 *bytes = g_variant_get_fixed_array(value, &len, sizeof(guint8));
            if (!mb_duplex_runtime_push_rx(&orc->runtime, bytes, len, orchestrator_now_ns()))
                g_printerr("RX runtime queue push failed; BLE notification dropped.\n");
            g_variant_unref(value);
        }
    }

    if (changed) g_variant_unref(changed);
    if (invalidated) g_variant_unref(invalidated);
}

static bool orchestrator_start_notify(MbOrchestrator *orc) {
    App *app = &orc->app;
    GError *error = NULL;

    orc->notify_sub_id = g_dbus_connection_signal_subscribe(
        app->bus, BLUEZ_BUS, PROPERTIES_IFACE, "PropertiesChanged",
        app->char_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        orchestrator_on_properties_changed, orc, NULL);

    if (orc->notify_sub_id == 0) {
        g_printerr("Failed to subscribe to PropertiesChanged.\n");
        return false;
    }

    GVariant *ret = g_dbus_connection_call_sync(
        app->bus, BLUEZ_BUS, app->char_path, GATT_CHRC_IFACE, "StartNotify",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 15000, NULL, &error);

    if (!ret) {
        g_printerr("StartNotify failed: %s\n", error->message);
        g_printerr("If authorization is required, pair/trust once with bluetoothctl or implement Agent1.\n");
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("StartNotify ok. Orchestrated ALSA MIDI port is ready.\n");
    return true;
}

static void orchestrator_cleanup(MbOrchestrator *orc) {
    App *app = &orc->app;

    if (app->alsa_rx_source_id) {
        g_source_remove(app->alsa_rx_source_id);
        app->alsa_rx_source_id = 0;
    }

    if (orc->notify_sub_id && app->bus) {
        g_dbus_connection_signal_unsubscribe(app->bus, orc->notify_sub_id);
        orc->notify_sub_id = 0;
    }

    if (orc->runtime_started) {
        mb_duplex_runtime_stop(&orc->runtime);
        orc->runtime_started = false;
    }

    mb_duplex_runtime_clear(&orc->runtime);
    app_cleanup(app);
    g_mutex_clear(&orc->alsa_lock);
    g_mutex_clear(&orc->gatt_write_lock);
}

static void orchestrator_print_usage(const char *argv0) {
    g_printerr("Usage: %s --config PATH\n", argv0);
}

int mb_orchestrator_main(int argc, char **argv) {
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            orchestrator_print_usage(argv[0]);
            return 2;
        }
    }

    if (!config_path) {
        orchestrator_print_usage(argv[0]);
        return 2;
    }

    MbOrchestrator orc = {0};
    App *app = &orc.app;
    g_mutex_init(&orc.alsa_lock);
    g_mutex_init(&orc.gatt_write_lock);

    if (!orchestrator_load_config(&app->cfg, config_path)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    g_print("midi-ble-rtd\n");
    g_print("Runtime: orchestrator\n");
    g_print("Config: %s\n", config_path);
    g_print("Address: %s\n", app->cfg.address);
    g_print("Service UUID: %s\n", app->cfg.service_uuid);
    g_print("I/O UUID: %s\n", app->cfg.io_uuid);
    g_print("I/O alias: %s\n", app->cfg.io_uuid_alias);
    g_print("GATT decides; Name/Alias is diagnostic only.\n");
    g_print("TX ALSA->BLE: %s\n", app->cfg.enable_tx ? "enabled" : "disabled");

    GError *error = NULL;
    app->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!app->bus) {
        g_printerr("Could not connect to system bus: %s\n", error->message);
        g_clear_error(&error);
        orchestrator_cleanup(&orc);
        return 1;
    }

    app->device_path = find_device(app);
    if (!app->device_path) {
        g_printerr("No matching BlueZ Device1 found. Scan/connect once or check the address.\n");
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (app->cfg.pair && !pair_device(app)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (app->cfg.trust)
        set_device_trusted(app);

    if (!connect_device(app)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (!wait_services_resolved(app, 15000)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    app->service_path = find_ble_midi_service(app);
    if (!app->service_path) {
        g_printerr("BLE-MIDI service not found. Connect MIDI before Audio on Roland GO:KEYS.\n");
        orchestrator_cleanup(&orc);
        return 1;
    }

    app->char_path = find_ble_midi_characteristic(app);
    if (!app->char_path) {
        g_printerr("BLE-MIDI I/O characteristic not found.\n");
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (!alsa_init(app)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    mb_duplex_runtime_init(&orc.runtime,
                           orchestrator_rx_consume, &orc,
                           orchestrator_tx_consume, &orc);
    if (!mb_duplex_runtime_start(&orc.runtime)) {
        g_printerr("Could not start duplex runtime workers.\n");
        orchestrator_cleanup(&orc);
        return 1;
    }
    orc.runtime_started = true;

    if (!orchestrator_start_notify(&orc)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (app->cfg.enable_tx)
        app->alsa_rx_source_id = g_timeout_add(1, orchestrator_alsa_rx_poll_cb, &orc);

    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);

    orchestrator_cleanup(&orc);
    return 0;
}
