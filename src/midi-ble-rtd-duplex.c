/*
 * Experimental duplex runtime daemon.
 *
 * This wrapper reuses the current daemon implementation, renames its main(),
 * and provides a new main() that connects BlueZ RX and ALSA TX to
 * MbDuplexRuntime. The stable midi-ble-rtd target remains unchanged.
 */

#define main midi_ble_rtd_legacy_main
#include "midi-ble-rtd.c"
#undef main

#include "mb-duplex-runtime.h"

typedef struct {
    App app;
    MbDuplexRuntime runtime;
    GMutex alsa_lock;
    GMutex gatt_write_lock;
    guint notify_sub_id;
    bool runtime_started;
} DuplexApp;

static uint64_t duplex_now_ns(void) {
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static void duplex_rx_consume(MbRuntimeFlow *flow,
                              const MbSliceRingItem *item,
                              const uint8_t *data,
                              size_t len,
                              void *user_data) {
    (void)flow;
    (void)item;
    DuplexApp *dapp = user_data;

    g_mutex_lock(&dapp->alsa_lock);
    ble_midi_decode_packet(&dapp->app, data, len);
    g_mutex_unlock(&dapp->alsa_lock);
}

static void duplex_tx_consume(MbRuntimeFlow *flow,
                              const MbSliceRingItem *item,
                              const uint8_t *data,
                              size_t len,
                              void *user_data) {
    (void)flow;
    (void)item;
    DuplexApp *dapp = user_data;

    g_mutex_lock(&dapp->gatt_write_lock);
    ble_midi_write_packet(&dapp->app, data, len);
    g_mutex_unlock(&dapp->gatt_write_lock);
}

static gboolean duplex_alsa_rx_poll_cb(gpointer user_data) {
    DuplexApp *dapp = user_data;
    App *app = &dapp->app;

    if (!app->cfg.enable_tx)
        return G_SOURCE_CONTINUE;

    for (;;) {
        g_mutex_lock(&dapp->alsa_lock);
        int pending = snd_seq_event_input_pending(app->seq, 1);
        if (pending <= 0) {
            g_mutex_unlock(&dapp->alsa_lock);
            break;
        }

        snd_seq_event_t *ev = NULL;
        int r = snd_seq_event_input(app->seq, &ev);
        if (r < 0) {
            if (r != -EAGAIN)
                g_printerr("snd_seq_event_input failed: %s\n", snd_strerror(r));
            g_mutex_unlock(&dapp->alsa_lock);
            break;
        }
        if (!ev) {
            g_mutex_unlock(&dapp->alsa_lock);
            break;
        }

        uint8_t midi[1024];
        long n = snd_midi_event_decode(app->alsa_midi_decoder, midi, sizeof(midi), ev);
        if (n < 0) {
            g_printerr("ALSA MIDI decode failed for event type %d: %s\n", ev->type, snd_strerror((int)n));
            snd_midi_event_reset_decode(app->alsa_midi_decoder);
            g_mutex_unlock(&dapp->alsa_lock);
            continue;
        }
        g_mutex_unlock(&dapp->alsa_lock);

        if (n == 0)
            continue;

        if (app->cfg.print_midi_events)
            print_midi_bytes("ALSA->BLE MIDI:", midi, (size_t)n);

        if (!mb_duplex_runtime_push_tx(&dapp->runtime, midi, (size_t)n, duplex_now_ns()))
            g_printerr("TX runtime queue push failed; ALSA event dropped.\n");
    }

    return G_SOURCE_CONTINUE;
}

static void duplex_on_properties_changed(GDBusConnection *connection,
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

    DuplexApp *dapp = user_data;
    const char *iface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, &invalidated);

    if (g_strcmp0(iface, GATT_CHRC_IFACE) == 0) {
        GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (value) {
            gsize len = 0;
            const guint8 *bytes = g_variant_get_fixed_array(value, &len, sizeof(guint8));
            if (!mb_duplex_runtime_push_rx(&dapp->runtime, bytes, len, duplex_now_ns()))
                g_printerr("RX runtime queue push failed; BLE notification dropped.\n");
            g_variant_unref(value);
        }
    }

    if (changed) g_variant_unref(changed);
    if (invalidated) g_variant_unref(invalidated);
}

static bool duplex_start_notify(DuplexApp *dapp) {
    App *app = &dapp->app;
    GError *error = NULL;

    dapp->notify_sub_id = g_dbus_connection_signal_subscribe(
        app->bus, BLUEZ_BUS, PROPERTIES_IFACE, "PropertiesChanged",
        app->char_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        duplex_on_properties_changed, dapp, NULL);

    if (dapp->notify_sub_id == 0) {
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
    g_print("StartNotify ok. Duplex runtime ALSA MIDI port is ready.\n");
    return true;
}

static void duplex_cleanup(DuplexApp *dapp) {
    App *app = &dapp->app;

    if (app->alsa_rx_source_id) {
        g_source_remove(app->alsa_rx_source_id);
        app->alsa_rx_source_id = 0;
    }

    if (dapp->notify_sub_id && app->bus) {
        g_dbus_connection_signal_unsubscribe(app->bus, dapp->notify_sub_id);
        dapp->notify_sub_id = 0;
    }

    if (dapp->runtime_started) {
        mb_duplex_runtime_stop(&dapp->runtime);
        dapp->runtime_started = false;
    }

    mb_duplex_runtime_clear(&dapp->runtime);
    app_cleanup(app);
    g_mutex_clear(&dapp->alsa_lock);
    g_mutex_clear(&dapp->gatt_write_lock);
}

static void duplex_print_usage(const char *argv0) {
    g_printerr("Usage: %s --config PATH\n", argv0);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            duplex_print_usage(argv[0]);
            return 2;
        }
    }

    if (!config_path) {
        duplex_print_usage(argv[0]);
        return 2;
    }

    DuplexApp dapp = {0};
    App *app = &dapp.app;
    g_mutex_init(&dapp.alsa_lock);
    g_mutex_init(&dapp.gatt_write_lock);

    if (!load_config(&app->cfg, config_path)) {
        duplex_cleanup(&dapp);
        return 1;
    }

    g_print("midi-ble-rtd-duplex\n");
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
        duplex_cleanup(&dapp);
        return 1;
    }

    app->device_path = find_device(app);
    if (!app->device_path) {
        g_printerr("No matching BlueZ Device1 found. Scan/connect once or check the address.\n");
        duplex_cleanup(&dapp);
        return 1;
    }

    if (app->cfg.pair && !pair_device(app)) {
        duplex_cleanup(&dapp);
        return 1;
    }

    if (app->cfg.trust)
        set_device_trusted(app);

    if (!connect_device(app)) {
        duplex_cleanup(&dapp);
        return 1;
    }

    if (!wait_services_resolved(app, 15000)) {
        duplex_cleanup(&dapp);
        return 1;
    }

    app->service_path = find_ble_midi_service(app);
    if (!app->service_path) {
        g_printerr("BLE-MIDI service not found. Connect MIDI before Audio on Roland GO:KEYS.\n");
        duplex_cleanup(&dapp);
        return 1;
    }

    app->char_path = find_ble_midi_characteristic(app);
    if (!app->char_path) {
        g_printerr("BLE-MIDI I/O characteristic not found.\n");
        duplex_cleanup(&dapp);
        return 1;
    }

    if (!alsa_init(app)) {
        duplex_cleanup(&dapp);
        return 1;
    }

    mb_duplex_runtime_init(&dapp.runtime,
                           duplex_rx_consume, &dapp,
                           duplex_tx_consume, &dapp);
    if (!mb_duplex_runtime_start(&dapp.runtime)) {
        g_printerr("Could not start duplex runtime workers.\n");
        duplex_cleanup(&dapp);
        return 1;
    }
    dapp.runtime_started = true;

    if (!duplex_start_notify(&dapp)) {
        duplex_cleanup(&dapp);
        return 1;
    }

    if (app->cfg.enable_tx)
        app->alsa_rx_source_id = g_timeout_add(1, duplex_alsa_rx_poll_cb, &dapp);

    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);

    duplex_cleanup(&dapp);
    return 0;
}
