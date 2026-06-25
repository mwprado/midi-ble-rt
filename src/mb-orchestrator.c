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
#include "mb-session.h"
#include "mb-stats.h"

typedef struct {
    App app;
    MbSession session;
    MbDuplexRuntime runtime;
    MbStats stats;
    GMutex alsa_lock;
    GMutex gatt_write_lock;
    GMutex session_lock;
    GMutex stats_lock;
    guint notify_sub_id;
    guint device_sub_id;
    guint stats_source_id;
    guint health_source_id;
    bool runtime_started;
    bool session_initialized;
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

static bool orchestrator_session_event(MbOrchestrator *orc, MbEventType event) {
    g_mutex_lock(&orc->session_lock);
    MbSessionState before = orc->session.state;
    MbErrorCode before_error = orc->session.error;
    bool ok = mb_session_handle_event(&orc->session, event);
    MbSessionState after = orc->session.state;
    MbErrorCode after_error = orc->session.error;
    g_mutex_unlock(&orc->session_lock);

    if (!ok || before != after || before_error != after_error) {
        g_print("Session: %s --%s--> %s",
                mb_session_state_name(before),
                mb_event_name(event),
                mb_session_state_name(after));
        if (after_error != MB_ERR_NONE)
            g_print(" error=%s", mb_error_name(after_error));
        if (!ok)
            g_print(" rejected");
        g_print("\n");
    }

    return ok;
}

static MbSessionState orchestrator_session_state(MbOrchestrator *orc) {
    g_mutex_lock(&orc->session_lock);
    MbSessionState state = orc->session.state;
    g_mutex_unlock(&orc->session_lock);
    return state;
}

static const char *orchestrator_session_state_name(MbOrchestrator *orc) {
    return mb_session_state_name(orchestrator_session_state(orc));
}

static bool orchestrator_session_can_mark_disconnected(MbOrchestrator *orc) {
    MbSessionState state = orchestrator_session_state(orc);
    return state != MB_SESSION_IDLE &&
           state != MB_SESSION_RECONNECTING &&
           state != MB_SESSION_ERROR;
}

static void orchestrator_mark_device_disconnected(MbOrchestrator *orc, const char *reason) {
    if (!orchestrator_session_can_mark_disconnected(orc))
        return;

    g_print("BlueZ device disconnected%s%s.\n",
            reason && *reason ? ": " : "",
            reason && *reason ? reason : "");
    orchestrator_session_event(orc, MB_EV_BLUEZ_DISCONNECTED);
}

static void orchestrator_stats_rx_packet(MbOrchestrator *orc, size_t bytes, uint64_t now_ns) {
    g_mutex_lock(&orc->stats_lock);
    mb_stats_rx_packet(&orc->stats, bytes, now_ns);
    g_mutex_unlock(&orc->stats_lock);
}

static void orchestrator_stats_tx_packet(MbOrchestrator *orc, size_t bytes, uint64_t now_ns) {
    g_mutex_lock(&orc->stats_lock);
    mb_stats_tx_packet(&orc->stats, bytes, now_ns);
    g_mutex_unlock(&orc->stats_lock);
}

static void orchestrator_stats_rx_drop(MbOrchestrator *orc) {
    g_mutex_lock(&orc->stats_lock);
    mb_stats_rx_drop(&orc->stats);
    g_mutex_unlock(&orc->stats_lock);
}

static void orchestrator_stats_tx_drop(MbOrchestrator *orc) {
    g_mutex_lock(&orc->stats_lock);
    mb_stats_tx_drop(&orc->stats);
    g_mutex_unlock(&orc->stats_lock);
}

static bool orchestrator_ble_midi_write_packet(MbOrchestrator *orc, const uint8_t *midi, size_t len) {
    App *app = &orc->app;

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
            orchestrator_stats_tx_drop(orc);
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
            orchestrator_stats_tx_drop(orc);
            orchestrator_session_event(orc, MB_EV_GATT_WRITE_FAILED);
            return false;
        }

        g_variant_unref(ret);
        orchestrator_stats_tx_packet(orc, chunk, orchestrator_now_ns());
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
    orchestrator_ble_midi_write_packet(orc, data, len);
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
            if (r != -EAGAIN) {
                g_printerr("snd_seq_event_input failed: %s\n", snd_strerror(r));
                orchestrator_session_event(orc, MB_EV_ALSA_IO_FAILED);
            }
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
            orchestrator_stats_tx_drop(orc);
            orchestrator_session_event(orc, MB_EV_ALSA_DECODE_FAILED);
            continue;
        }
        g_mutex_unlock(&orc->alsa_lock);

        if (n == 0)
            continue;

        if (app->cfg.print_midi_events)
            print_midi_bytes("ALSA->BLE MIDI:", midi, (size_t)n);

        if (!mb_duplex_runtime_push_tx(&orc->runtime, midi, (size_t)n, orchestrator_now_ns())) {
            g_printerr("TX runtime queue push failed; ALSA event dropped.\n");
            orchestrator_stats_tx_drop(orc);
            orchestrator_session_event(orc, MB_EV_RUNTIME_FAILED);
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean orchestrator_device_health_cb(gpointer user_data) {
    MbOrchestrator *orc = user_data;
    App *app = &orc->app;

    if (!app->device_path || !orchestrator_session_can_mark_disconnected(orc))
        return G_SOURCE_CONTINUE;

    bool connected = false;
    if (!get_device_bool_property(app, "Connected", &connected) || !connected)
        orchestrator_mark_device_disconnected(orc, "health check");

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
    App *app = &orc->app;
    const char *iface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, &invalidated);

    if (g_strcmp0(iface, DEVICE_IFACE) == 0 &&
        app->device_path && g_strcmp0(object_path, app->device_path) == 0) {
        GVariant *v_connected = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN);
        if (v_connected) {
            gboolean connected = g_variant_get_boolean(v_connected);
            if (!connected)
                orchestrator_mark_device_disconnected(orc, "Device1.Connected=false");
            g_variant_unref(v_connected);
        }
    }

    if (g_strcmp0(iface, GATT_CHRC_IFACE) == 0 &&
        app->char_path && g_strcmp0(object_path, app->char_path) == 0) {
        GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (value) {
            gsize len = 0;
            const guint8 *bytes = g_variant_get_fixed_array(value, &len, sizeof(guint8));
            uint64_t now_ns = orchestrator_now_ns();
            if (!mb_duplex_runtime_push_rx(&orc->runtime, bytes, len, now_ns)) {
                g_printerr("RX runtime queue push failed; BLE notification dropped.\n");
                orchestrator_stats_rx_drop(orc);
                orchestrator_session_event(orc, MB_EV_RUNTIME_FAILED);
            } else {
                orchestrator_stats_rx_packet(orc, len, now_ns);
            }
            g_variant_unref(value);
        }
    }

    if (changed) g_variant_unref(changed);
    if (invalidated) g_variant_unref(invalidated);
}

static bool orchestrator_start_device_watch(MbOrchestrator *orc) {
    App *app = &orc->app;

    if (!app->device_path)
        return false;

    orc->device_sub_id = g_dbus_connection_signal_subscribe(
        app->bus, BLUEZ_BUS, PROPERTIES_IFACE, "PropertiesChanged",
        app->device_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        orchestrator_on_properties_changed, orc, NULL);

    if (orc->device_sub_id == 0) {
        g_printerr("Failed to subscribe to Device1 PropertiesChanged.\n");
        return false;
    }

    return true;
}

static bool orchestrator_start_notify(MbOrchestrator *orc) {
    App *app = &orc->app;
    GError *error = NULL;

    orc->notify_sub_id = g_dbus_connection_signal_subscribe(
        app->bus, BLUEZ_BUS, PROPERTIES_IFACE, "PropertiesChanged",
        app->char_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        orchestrator_on_properties_changed, orc, NULL);

    if (orc->notify_sub_id == 0) {
        g_printerr("Failed to subscribe to GattCharacteristic1 PropertiesChanged.\n");
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

static gboolean orchestrator_stats_export_cb(gpointer user_data) {
    MbOrchestrator *orc = user_data;
    App *app = &orc->app;
    GError *error = NULL;
    uint64_t now_ns = orchestrator_now_ns();
    const char *label = app->cfg.name && *app->cfg.name ? app->cfg.name : "BLE-MIDI";
    const char *address = app->cfg.address && *app->cfg.address ? app->cfg.address : "-";
    const char *state = orchestrator_session_state_name(orc);

    g_mutex_lock(&orc->stats_lock);
    bool ok = mb_stats_export_tsv(&orc->stats,
                                  label,
                                  address,
                                  state,
                                  mb_duplex_runtime_rx_depth(&orc->runtime),
                                  mb_duplex_runtime_tx_depth(&orc->runtime),
                                  now_ns,
                                  &error);
    g_mutex_unlock(&orc->stats_lock);

    if (!ok) {
        g_printerr("Stats export failed: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    return G_SOURCE_CONTINUE;
}

static void orchestrator_cleanup(MbOrchestrator *orc) {
    App *app = &orc->app;

    if (orc->stats_source_id) {
        g_source_remove(orc->stats_source_id);
        orc->stats_source_id = 0;
    }

    if (orc->health_source_id) {
        g_source_remove(orc->health_source_id);
        orc->health_source_id = 0;
    }

    if (app->alsa_rx_source_id) {
        g_source_remove(app->alsa_rx_source_id);
        app->alsa_rx_source_id = 0;
    }

    if (orc->notify_sub_id && app->bus) {
        g_dbus_connection_signal_unsubscribe(app->bus, orc->notify_sub_id);
        orc->notify_sub_id = 0;
    }

    if (orc->device_sub_id && app->bus) {
        g_dbus_connection_signal_unsubscribe(app->bus, orc->device_sub_id);
        orc->device_sub_id = 0;
    }

    if (orc->runtime_started) {
        mb_duplex_runtime_stop(&orc->runtime);
        orc->runtime_started = false;
    }

    mb_duplex_runtime_clear(&orc->runtime);
    g_mutex_lock(&orc->stats_lock);
    mb_stats_clear(&orc->stats);
    g_mutex_unlock(&orc->stats_lock);
    if (orc->session_initialized) {
        g_mutex_lock(&orc->session_lock);
        mb_session_clear(&orc->session);
        g_mutex_unlock(&orc->session_lock);
        orc->session_initialized = false;
    }
    app_cleanup(app);
    g_mutex_clear(&orc->alsa_lock);
    g_mutex_clear(&orc->gatt_write_lock);
    g_mutex_clear(&orc->session_lock);
    g_mutex_clear(&orc->stats_lock);
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
    g_mutex_init(&orc.session_lock);
    g_mutex_init(&orc.stats_lock);

    if (!orchestrator_load_config(&app->cfg, config_path)) {
        orchestrator_cleanup(&orc);
        return 1;
    }

    mb_session_init(&orc.session, NULL, app->cfg.address, app->cfg.name);
    orc.session.auto_reconnect = app->cfg.auto_reconnect;
    orc.session_initialized = true;

    mb_stats_init(&orc.stats, app->cfg.stats_enabled, app->cfg.stats_interval_ms);

    g_print("midi-ble-rtd\n");
    g_print("Runtime: orchestrator\n");
    g_print("Config: %s\n", config_path);
    g_print("Address: %s\n", app->cfg.address);
    g_print("Service UUID: %s\n", app->cfg.service_uuid);
    g_print("I/O UUID: %s\n", app->cfg.io_uuid);
    g_print("I/O alias: %s\n", app->cfg.io_uuid_alias);
    g_print("GATT decides; Name/Alias is diagnostic only.\n");
    g_print("TX ALSA->BLE: %s\n", app->cfg.enable_tx ? "enabled" : "disabled");
    g_print("Stats: %s\n", app->cfg.stats_enabled ? "enabled" : "disabled");
    if (app->cfg.stats_enabled && orc.stats.path)
        g_print("Stats path: %s\n", orc.stats.path);

    GError *error = NULL;
    app->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!app->bus) {
        g_printerr("Could not connect to system bus: %s\n", error->message);
        g_clear_error(&error);
        orchestrator_session_event(&orc, MB_EV_FATAL);
        orchestrator_cleanup(&orc);
        return 1;
    }

    app->device_path = find_device(app);
    if (!app->device_path) {
        g_printerr("No matching BlueZ Device1 found. Scan/connect once or check the address.\n");
        orchestrator_session_event(&orc, MB_EV_TIMEOUT);
        orchestrator_cleanup(&orc);
        return 1;
    }

    g_mutex_lock(&orc.session_lock);
    mb_session_set_identity(&orc.session, app->device_path, app->cfg.address, app->cfg.name);
    g_mutex_unlock(&orc.session_lock);

    if (!orchestrator_start_device_watch(&orc)) {
        orchestrator_session_event(&orc, MB_EV_FATAL);
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (app->cfg.pair && !pair_device(app)) {
        orchestrator_session_event(&orc, MB_EV_BLUEZ_CONNECT_FAILED);
        orchestrator_cleanup(&orc);
        return 1;
    }

    if (app->cfg.trust)
        set_device_trusted(app);

    orchestrator_session_event(&orc, MB_EV_CMD_CONNECT);
    if (!connect_device(app)) {
        orchestrator_session_event(&orc, MB_EV_BLUEZ_CONNECT_FAILED);
        orchestrator_cleanup(&orc);
        return 1;
    }
    orchestrator_session_event(&orc, MB_EV_BLUEZ_CONNECTED);

    if (!wait_services_resolved(app, 15000)) {
        orchestrator_session_event(&orc, MB_EV_TIMEOUT);
        orchestrator_cleanup(&orc);
        return 1;
    }
    orchestrator_session_event(&orc, MB_EV_BLUEZ_SERVICES_RESOLVED);

    app->service_path = find_ble_midi_service(app);
    if (!app->service_path) {
        g_printerr("BLE-MIDI service not found. Connect MIDI before Audio on Roland GO:KEYS.\n");
        orchestrator_session_event(&orc, MB_EV_MIDI_SERVICE_NOT_FOUND);
        orchestrator_cleanup(&orc);
        return 1;
    }

    app->char_path = find_ble_midi_characteristic(app);
    if (!app->char_path) {
        g_printerr("BLE-MIDI I/O characteristic not found.\n");
        orchestrator_session_event(&orc, MB_EV_MIDI_CHAR_NOT_FOUND);
        orchestrator_cleanup(&orc);
        return 1;
    }

    g_mutex_lock(&orc.session_lock);
    mb_session_set_midi_binding(&orc.session,
                                app->service_path,
                                app->char_path,
                                app->cfg.io_uuid);
    g_mutex_unlock(&orc.session_lock);
    orchestrator_session_event(&orc, MB_EV_MIDI_CHAR_FOUND);

    if (!alsa_init(app)) {
        orchestrator_session_event(&orc, MB_EV_ALSA_FAILED);
        orchestrator_cleanup(&orc);
        return 1;
    }

    g_mutex_lock(&orc.session_lock);
    mb_session_set_alsa_port(&orc.session, app->alsa_port);
    g_mutex_unlock(&orc.session_lock);
    orchestrator_session_event(&orc, MB_EV_ALSA_READY);

    mb_duplex_runtime_init(&orc.runtime,
                           orchestrator_rx_consume, &orc,
                           orchestrator_tx_consume, &orc);
    if (!mb_duplex_runtime_start(&orc.runtime)) {
        g_printerr("Could not start duplex runtime workers.\n");
        orchestrator_session_event(&orc, MB_EV_RUNTIME_FAILED);
        orchestrator_cleanup(&orc);
        return 1;
    }
    orc.runtime_started = true;

    if (!orchestrator_start_notify(&orc)) {
        orchestrator_session_event(&orc, MB_EV_NOTIFY_FAILED);
        orchestrator_cleanup(&orc);
        return 1;
    }
    orchestrator_session_event(&orc, MB_EV_NOTIFY_OK);

    if (app->cfg.enable_tx)
        app->alsa_rx_source_id = g_timeout_add(1, orchestrator_alsa_rx_poll_cb, &orc);

    orc.health_source_id = g_timeout_add(1000, orchestrator_device_health_cb, &orc);

    if (app->cfg.stats_enabled)
        orc.stats_source_id = g_timeout_add(app->cfg.stats_interval_ms,
                                            orchestrator_stats_export_cb,
                                            &orc);

    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);

    orchestrator_cleanup(&orc);
    return 0;
}
