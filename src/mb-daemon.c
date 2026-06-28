/*
 * midi-ble-rtd process entry point.
 *
 * Single-file configurations are delegated to the existing orchestrator.  A
 * configuration directory is handled here as a multi-session runtime: one
 * MbDaemon, one shared BlueZ bus, one shared ALSA client, one MbSession and one
 * duplex runtime per configured device.
 */

#include "mb-orchestrator.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

#include "mb-alsa.h"
#include "mb-alsa-port.h"
#include "mb-ble-midi.h"
#include "mb-bluez.h"
#include "mb-config.h"
#include "mb-duplex-runtime.h"
#include "mb-gatt-midi.h"
#include "mb-session.h"

static const char *printable_string(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

typedef struct _ConfigDirRuntime ConfigDirRuntime;

typedef struct {
    ConfigDirRuntime *owner;
    const MbDeviceConfig *config;
    MbSession *session;
    GMutex session_lock;

    MbDuplexRuntime runtime;
    bool runtime_started;

    /*
     * Dataplane TX lock per device.
     *
     * Cada dispositivo BLE-MIDI em STREAMING tem seu próprio TX worker.
     * O lock é por dispositivo, não global, para permitir instrumentos
     * tocando simultaneamente sem serializar todos os GATT WriteValue.
     */
    GMutex gatt_write_lock;

    int alsa_port;
    snd_midi_event_t *alsa_midi_encoder;
    snd_midi_event_t *alsa_midi_decoder;

    guint device_sub_id;
    guint notify_sub_id;

    uint8_t running_status;
} ConfigDeviceRuntime;

typedef enum {
    MB_LIFECYCLE_CMD_DISCOVER,
    MB_LIFECYCLE_CMD_CONNECT,
    MB_LIFECYCLE_CMD_DISCONNECT,
    MB_LIFECYCLE_CMD_RECHECK,
} MbLifecycleCommandType;

typedef struct {
    MbLifecycleCommandType type;
    ConfigDeviceRuntime *dev;
    char *reason;
} MbLifecycleCommand;

struct _ConfigDirRuntime {
    MbConfig cfg;
    MbDaemon daemon;

    GDBusConnection *bus;
    GMainLoop *loop;
    snd_seq_t *seq;

    GPtrArray *devices;
    GHashTable *device_by_alsa_port;
    GHashTable *device_by_char_path;

    GMutex alsa_lock;

    /*
     * Lifecycle/control-plane monitor.
     *
     * Executa na thread principal do GLib main loop.
     * Tudo que altera descoberta, conexão, binding BlueZ/GATT,
     * health/recheck, reconnect e futuro ctl deve passar por esta fila.
     *
     * RX/TX de cada sessão só rodam em paralelo depois de STREAMING,
     * via MbDuplexRuntime por dispositivo.
     */
    GQueue *lifecycle_queue;
    bool lifecycle_busy;
    guint lifecycle_source_id;

    guint alsa_source_id;
    guint health_source_id;
    guint reconnect_source_id;
    guint sigint_source_id;
    guint sigterm_source_id;
};

static uint64_t runtime_now_ns(void) {
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static uint16_t runtime_ble_midi_timestamp_13bit(void) {
    gint64 ms = g_get_monotonic_time() / 1000;
    return (uint16_t)(ms & 0x1fff);
}

static gboolean quit_main_loop(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;
    if (rt && rt->loop)
        g_main_loop_quit(rt->loop);
    return G_SOURCE_REMOVE;
}

static bool session_has_bluez_device_path(const MbSession *session) {
    return session &&
           session->device_path &&
           *session->device_path &&
           !g_str_has_prefix(session->device_path, "config:");
}

static char *config_device_path(const MbDeviceConfig *device) {
    const char *id = printable_string(device->id, NULL);
    if (id)
        return g_strdup_printf("config:%s", id);

    return g_strdup_printf("config:%s", printable_string(device->address, "device"));
}

static const char *device_label(const ConfigDeviceRuntime *dev) {
    if (!dev || !dev->config)
        return "device";
    if (dev->config->name && *dev->config->name)
        return dev->config->name;
    if (dev->config->id && *dev->config->id)
        return dev->config->id;
    return printable_string(dev->config->address, "device");
}

static const char *device_io_alias(const ConfigDeviceRuntime *dev) {
    ConfigDirRuntime *rt = dev->owner;

    if (rt->cfg.io_uuid_alias && *rt->cfg.io_uuid_alias)
        return rt->cfg.io_uuid_alias;

    if (dev->config && g_strcmp0(dev->config->profile, "roland_gokeys") == 0)
        return MB_BLE_MIDI_ROLAND_IO_UUID_ALIAS;

    return "";
}

static char *device_dup_device_path(ConfigDeviceRuntime *dev) {
    char *path = NULL;
    g_mutex_lock(&dev->session_lock);
    path = g_strdup(dev->session ? dev->session->device_path : NULL);
    g_mutex_unlock(&dev->session_lock);
    return path;
}

static char *device_dup_char_path(ConfigDeviceRuntime *dev) {
    char *path = NULL;
    g_mutex_lock(&dev->session_lock);
    path = g_strdup(dev->session ? dev->session->midi_char_path : NULL);
    g_mutex_unlock(&dev->session_lock);
    return path;
}

static MbSessionState device_session_state(ConfigDeviceRuntime *dev) {
    g_mutex_lock(&dev->session_lock);
    MbSessionState state = dev->session ? dev->session->state : MB_SESSION_ERROR;
    g_mutex_unlock(&dev->session_lock);
    return state;
}

static bool device_has_bluez_device_path(ConfigDeviceRuntime *dev) {
    bool ok = false;
    g_mutex_lock(&dev->session_lock);
    ok = session_has_bluez_device_path(dev->session);
    g_mutex_unlock(&dev->session_lock);
    return ok;
}

static bool device_session_event(ConfigDeviceRuntime *dev, MbEventType event) {
    g_mutex_lock(&dev->session_lock);
    MbSessionState before = dev->session->state;
    MbErrorCode before_error = dev->session->error;
    bool ok = mb_session_handle_event(dev->session, event);
    MbSessionState after = dev->session->state;
    MbErrorCode after_error = dev->session->error;
    g_mutex_unlock(&dev->session_lock);

    if (!ok || before != after || before_error != after_error) {
        g_print("Session[%s]: %s --%s--> %s",
                printable_string(dev->config->id, device_label(dev)),
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

static bool device_session_is(ConfigDeviceRuntime *dev, MbSessionState expected) {
    return dev && dev->session && device_session_state(dev) == expected;
}

static bool device_can_mark_disconnected(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->session)
        return false;

    MbSessionState state = device_session_state(dev);
    return state != MB_SESSION_IDLE &&
           state != MB_SESSION_RECONNECTING &&
           state != MB_SESSION_ERROR;
}

static void runtime_build_sessions(ConfigDirRuntime *rt) {
    mb_daemon_init(&rt->daemon);

    for (unsigned i = 0; i < mb_config_device_count(&rt->cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(&rt->cfg, i);
        if (!device)
            continue;

        char *path = config_device_path(device);
        MbSession *session = mb_daemon_ensure_session(&rt->daemon,
                                                      path,
                                                      device->address,
                                                      device->name);
        if (session)
            session->auto_reconnect = device->reconnect_on_link_loss;
        g_free(path);
    }
}

static void runtime_resolve_bluez_devices(ConfigDirRuntime *rt) {
    for (unsigned i = 0; i < mb_config_device_count(&rt->cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(&rt->cfg, i);
        if (!device || !device->address || !*device->address)
            continue;

        char *bluez_path = mb_bluez_find_device_path_by_address(rt->bus, device->address);
        if (!bluez_path)
            continue;

        MbSession *session = mb_daemon_ensure_session(&rt->daemon,
                                                      bluez_path,
                                                      device->address,
                                                      device->name);
        if (session)
            session->auto_reconnect = device->reconnect_on_link_loss;
        g_free(bluez_path);
    }
}

static void device_runtime_free(gpointer data) {
    ConfigDeviceRuntime *dev = data;
    if (!dev)
        return;

    if (dev->notify_sub_id && dev->owner && dev->owner->bus)
        g_dbus_connection_signal_unsubscribe(dev->owner->bus, dev->notify_sub_id);
    if (dev->device_sub_id && dev->owner && dev->owner->bus)
        g_dbus_connection_signal_unsubscribe(dev->owner->bus, dev->device_sub_id);

    if (dev->runtime_started) {
        mb_duplex_runtime_stop(&dev->runtime);
        dev->runtime_started = false;
    }
    mb_duplex_runtime_clear(&dev->runtime);
    mb_alsa_midi_event_pair_free(&dev->alsa_midi_encoder,
                                  &dev->alsa_midi_decoder);
    g_mutex_clear(&dev->gatt_write_lock);
    g_mutex_clear(&dev->session_lock);
    g_free(dev);
}

static void runtime_create_devices(ConfigDirRuntime *rt) {
    rt->devices = g_ptr_array_new_with_free_func(device_runtime_free);

    for (unsigned i = 0; i < mb_config_device_count(&rt->cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(&rt->cfg, i);
        if (!device)
            continue;

        MbSession *session = mb_daemon_find_session_by_address(&rt->daemon,
                                                               device->address);
        if (!session)
            continue;

        ConfigDeviceRuntime *dev = g_new0(ConfigDeviceRuntime, 1);
        dev->owner = rt;
        dev->config = device;
        dev->session = session;
        dev->alsa_port = -1;
        g_mutex_init(&dev->session_lock);
        g_mutex_init(&dev->gatt_write_lock);
        g_ptr_array_add(rt->devices, dev);
    }
}

static void runtime_remove_char_route(ConfigDeviceRuntime *dev) {
    char *char_path = device_dup_char_path(dev);
    if (char_path && dev->owner && dev->owner->device_by_char_path)
        g_hash_table_remove(dev->owner->device_by_char_path, char_path);
    g_free(char_path);
}

static void device_stop_notify(ConfigDeviceRuntime *dev) {
    runtime_remove_char_route(dev);
    if (dev->notify_sub_id && dev->owner && dev->owner->bus) {
        g_dbus_connection_signal_unsubscribe(dev->owner->bus, dev->notify_sub_id);
        dev->notify_sub_id = 0;
    }
}

static void device_mark_disconnected(ConfigDeviceRuntime *dev, const char *reason) {
    if (!device_can_mark_disconnected(dev))
        return;

    g_print("BlueZ device disconnected for %s%s%s.\n",
            device_label(dev),
            reason && *reason ? ": " : "",
            reason && *reason ? reason : "");
    device_stop_notify(dev);
    device_session_event(dev, MB_EV_BLUEZ_DISCONNECTED);
}

static bool runtime_open_alsa_client(ConfigDirRuntime *rt) {
    if (rt->seq)
        return true;

    if (!mb_alsa_client_open(&rt->seq, rt->cfg.alsa_client_name))
        return false;

    g_print("ALSA Sequencer client created: %s\n",
            printable_string(rt->cfg.alsa_client_name, "midi-ble-rt"));
    return true;
}

static bool device_ensure_alsa_port(ConfigDeviceRuntime *dev) {
    ConfigDirRuntime *rt = dev->owner;

    if (dev->alsa_port >= 0)
        return true;

    if (!runtime_open_alsa_client(rt))
        return false;

    dev->alsa_port = mb_alsa_port_create_duplex(rt->seq,
                                                printable_string(dev->config->alsa_port_name,
                                                                 device_label(dev)));
    if (dev->alsa_port < 0) {
        dev->alsa_port = -1;
        return false;
    }

    if (!mb_alsa_midi_event_pair_new(&dev->alsa_midi_encoder,
                                     &dev->alsa_midi_decoder))
        return false;

    g_mutex_lock(&dev->session_lock);
    mb_session_set_alsa_port(dev->session, dev->alsa_port);
    g_mutex_unlock(&dev->session_lock);

    g_hash_table_replace(rt->device_by_alsa_port,
                         GINT_TO_POINTER(dev->alsa_port),
                         dev);

    g_print("ALSA port created for %s: %s:%d\n",
            device_label(dev),
            printable_string(rt->cfg.alsa_client_name, "midi-ble-rt"),
            dev->alsa_port);
    return true;
}

static void device_alsa_emit_midi_byte(ConfigDeviceRuntime *dev, uint8_t byte) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);

    int r = snd_midi_event_encode_byte(dev->alsa_midi_encoder, byte, &ev);
    if (r < 0) {
        g_printerr("ALSA MIDI encode failed for %s byte 0x%02x: %s\n",
                   device_label(dev), byte, snd_strerror(r));
        snd_midi_event_reset_encode(dev->alsa_midi_encoder);
        return;
    }

    if (r > 0) {
        snd_seq_ev_set_source(&ev, dev->alsa_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);

        int out = snd_seq_event_output_direct(dev->owner->seq, &ev);
        if (out < 0)
            g_printerr("snd_seq_event_output_direct failed for %s: %s\n",
                       device_label(dev), snd_strerror(out));
    }
}

static void device_ble_midi_decode_packet(ConfigDeviceRuntime *dev,
                                          const uint8_t *p,
                                          size_t len) {
    if (!p || len < 3)
        return;

    if (dev->owner->cfg.print_ble_packets) {
        g_print("BLE packet[%s]:", device_label(dev));
        for (size_t k = 0; k < len; k++)
            g_print(" %02x", p[k]);
        g_print("\n");
    }

    if (!mb_ble_midi_packet_has_valid_header(p, len)) {
        g_printerr("Invalid BLE-MIDI header byte from %s: 0x%02x\n",
                   device_label(dev), p[0]);
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
            if (dev->running_status >= 0x80) {
                int n = mb_ble_midi_status_data_len(dev->running_status);
                if (n > 0 && i + (size_t)n <= len) {
                    device_alsa_emit_midi_byte(dev, dev->running_status);
                    for (int j = 0; j < n; j++)
                        device_alsa_emit_midi_byte(dev, p[i++]);
                    continue;
                }
            }
            i++;
            continue;
        }

        i++;
        device_alsa_emit_midi_byte(dev, status);

        if (status < 0xF0)
            dev->running_status = status;
        else if (status != 0xF7)
            dev->running_status = 0;

        int data_len = mb_ble_midi_status_data_len(status);
        if (data_len >= 0) {
            for (int j = 0; j < data_len && i < len; j++) {
                if (p[i] & 0x80)
                    break;
                device_alsa_emit_midi_byte(dev, p[i++]);
            }
            continue;
        }

        if (data_len == -2) {
            while (i < len) {
                device_alsa_emit_midi_byte(dev, p[i]);
                if (p[i] == 0xF7) {
                    i++;
                    break;
                }
                i++;
            }
        }
    }
}

static bool device_ble_midi_write_packet(ConfigDeviceRuntime *dev,
                                         const uint8_t *midi,
                                         size_t len) {
    if (!midi || len == 0)
        return true;

    char *char_path = device_dup_char_path(dev);
    if (!char_path) {
        g_printerr("No GATT characteristic path for %s.\n", device_label(dev));
        return false;
    }

    const size_t max_midi_per_packet = 18;
    size_t off = 0;
    bool ok = true;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > max_midi_per_packet)
            chunk = max_midi_per_packet;

        uint8_t packet[20];
        size_t packet_len = 0;
        if (!mb_ble_midi_make_packet(runtime_ble_midi_timestamp_13bit(),
                                     &midi[off], chunk,
                                     packet, sizeof(packet), &packet_len)) {
            g_printerr("BLE-MIDI packet construction failed for %s.\n",
                       device_label(dev));
            ok = false;
            break;
        }

        if (dev->owner->cfg.print_ble_packets) {
            g_print("BLE write[%s]:", device_label(dev));
            for (size_t i = 0; i < packet_len; i++)
                g_print(" %02x", packet[i]);
            g_print("\n");
        }

        if (!mb_gatt_midi_write_value_command(dev->owner->bus,
                                               char_path,
                                               packet,
                                               packet_len,
                                               5000)) {
            device_session_event(dev, MB_EV_GATT_WRITE_FAILED);
            ok = false;
            break;
        }

        off += chunk;
    }

    g_free(char_path);
    return ok;
}

static void device_rx_consume(MbRuntimeFlow *flow,
                              const MbSliceRingItem *item,
                              const uint8_t *data,
                              size_t len,
                              void *user_data) {
    (void)flow;
    (void)item;
    ConfigDeviceRuntime *dev = user_data;

    g_mutex_lock(&dev->owner->alsa_lock);
    device_ble_midi_decode_packet(dev, data, len);
    g_mutex_unlock(&dev->owner->alsa_lock);
}

static void device_tx_consume(MbRuntimeFlow *flow,
                              const MbSliceRingItem *item,
                              const uint8_t *data,
                              size_t len,
                              void *user_data) {
    (void)flow;
    (void)item;
    ConfigDeviceRuntime *dev = user_data;

    g_mutex_lock(&dev->gatt_write_lock);
    device_ble_midi_write_packet(dev, data, len);
    g_mutex_unlock(&dev->gatt_write_lock);
}

static bool device_start_workers(ConfigDeviceRuntime *dev) {
    if (dev->runtime_started)
        return true;

    mb_duplex_runtime_init(&dev->runtime,
                           device_rx_consume, dev,
                           device_tx_consume, dev);
    if (!mb_duplex_runtime_start(&dev->runtime)) {
        g_printerr("Could not start duplex runtime workers for %s.\n",
                   device_label(dev));
        device_session_event(dev, MB_EV_RUNTIME_FAILED);
        return false;
    }

    dev->runtime_started = true;
    return true;
}

static void device_on_properties_changed(GDBusConnection *connection,
                                         const gchar *sender_name,
                                         const gchar *object_path,
                                         const gchar *interface_name,
                                         const gchar *signal_name,
                                         GVariant *parameters,
                                         gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)interface_name;
    (void)signal_name;

    ConfigDeviceRuntime *dev = user_data;
    const char *iface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, &invalidated);

    char *device_path = device_dup_device_path(dev);
    bool device_match = device_path && g_strcmp0(object_path, device_path) == 0;
    g_free(device_path);

    if (g_strcmp0(iface, DEVICE_IFACE) == 0 && device_match) {
        GVariant *v_connected = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN);
        if (v_connected) {
            gboolean connected = g_variant_get_boolean(v_connected);
            if (!connected)
                device_mark_disconnected(dev, "Device1.Connected=false");
            g_variant_unref(v_connected);
        }
    }

    char *char_path = device_dup_char_path(dev);
    bool char_match = char_path && g_strcmp0(object_path, char_path) == 0;
    g_free(char_path);

    if (g_strcmp0(iface, GATT_CHRC_IFACE) == 0 && char_match) {
        GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (value) {
            gsize len = 0;
            const guint8 *bytes = g_variant_get_fixed_array(value, &len, sizeof(guint8));
            if (!mb_duplex_runtime_push_rx(&dev->runtime, bytes, len, runtime_now_ns())) {
                g_printerr("RX runtime queue push failed for %s; BLE notification dropped.\n",
                           device_label(dev));
                device_session_event(dev, MB_EV_RUNTIME_FAILED);
            }
            g_variant_unref(value);
        }
    }

    if (changed) g_variant_unref(changed);
    if (invalidated) g_variant_unref(invalidated);
}

static bool device_start_device_watch(ConfigDeviceRuntime *dev) {
    char *device_path = device_dup_device_path(dev);
    if (!device_path)
        return false;

    if (dev->device_sub_id) {
        g_free(device_path);
        return true;
    }

    dev->device_sub_id = mb_bluez_subscribe_properties_changed(dev->owner->bus,
                                                               device_path,
                                                               device_on_properties_changed,
                                                               dev);
    g_free(device_path);

    if (!dev->device_sub_id) {
        g_printerr("Failed to subscribe to Device1 PropertiesChanged for %s.\n",
                   device_label(dev));
        return false;
    }

    return true;
}

static bool device_start_notify(ConfigDeviceRuntime *dev) {
    GError *error = NULL;

    device_stop_notify(dev);

    char *char_path = device_dup_char_path(dev);
    if (!char_path)
        return false;

    dev->notify_sub_id = mb_bluez_subscribe_properties_changed(dev->owner->bus,
                                                               char_path,
                                                               device_on_properties_changed,
                                                               dev);
    if (!dev->notify_sub_id) {
        g_free(char_path);
        g_printerr("Failed to subscribe to GattCharacteristic1 PropertiesChanged for %s.\n",
                   device_label(dev));
        return false;
    }

    if (!mb_gatt_midi_start_notify(dev->owner->bus,
                                   char_path,
                                   15000,
                                   &error)) {
        g_printerr("StartNotify failed for %s: %s\n",
                   device_label(dev), error ? error->message : "unknown error");
        g_clear_error(&error);
        g_free(char_path);
        return false;
    }

    g_free(char_path);
    g_print("StartNotify ok for %s.\n", device_label(dev));
    return true;
}

static bool device_try_start_streaming(ConfigDeviceRuntime *dev) {
    ConfigDirRuntime *rt = dev->owner;
    MbSessionState state = device_session_state(dev);

    if (state == MB_SESSION_STREAMING)
        return true;

    char *device_path = device_dup_device_path(dev);
    if (!device_path || g_str_has_prefix(device_path, "config:")) {
        g_print("Connect skipped for %s: BlueZ Device1 not found.\n", device_label(dev));
        g_free(device_path);
        return false;
    }

    if (state == MB_SESSION_RECONNECTING)
        device_session_event(dev, MB_EV_RECONNECT_TIMER);
    else if (state == MB_SESSION_IDLE || state == MB_SESSION_SCANNING)
        device_session_event(dev, MB_EV_CMD_CONNECT);
    else if (state != MB_SESSION_CONNECTING) {
        g_free(device_path);
        return false;
    }

    if (dev->config->pair && !mb_bluez_pair_device(rt->bus, device_path)) {
        device_session_event(dev, MB_EV_BLUEZ_CONNECT_FAILED);
        g_free(device_path);
        return false;
    }

    if (dev->config->trust)
        mb_bluez_set_device_trusted(rt->bus, device_path);

    if (!mb_bluez_connect_device(rt->bus, device_path)) {
        device_session_event(dev, MB_EV_BLUEZ_CONNECT_FAILED);
        g_free(device_path);
        return false;
    }
    device_session_event(dev, MB_EV_BLUEZ_CONNECTED);

    if (!mb_bluez_wait_services_resolved(rt->bus, device_path, 15000)) {
        device_session_event(dev, MB_EV_TIMEOUT);
        g_free(device_path);
        return false;
    }
    device_session_event(dev, MB_EV_BLUEZ_SERVICES_RESOLVED);

    char *service_path = mb_gatt_midi_find_service(rt->bus,
                                                   device_path,
                                                   rt->cfg.service_uuid);
    if (!service_path) {
        g_printerr("BLE-MIDI service not found for %s.\n", device_label(dev));
        device_session_event(dev, MB_EV_MIDI_SERVICE_NOT_FOUND);
        g_free(device_path);
        return false;
    }

    char *char_path = mb_gatt_midi_find_characteristic(rt->bus,
                                                       device_path,
                                                       service_path,
                                                       rt->cfg.io_uuid,
                                                       device_io_alias(dev));
    g_free(device_path);

    if (!char_path) {
        g_printerr("BLE-MIDI I/O characteristic not found for %s.\n", device_label(dev));
        g_free(service_path);
        device_session_event(dev, MB_EV_MIDI_CHAR_NOT_FOUND);
        return false;
    }

    g_mutex_lock(&dev->session_lock);
    mb_session_set_midi_binding(dev->session,
                                service_path,
                                char_path,
                                rt->cfg.io_uuid);
    g_mutex_unlock(&dev->session_lock);

    g_hash_table_replace(rt->device_by_char_path, g_strdup(char_path), dev);
    g_free(service_path);
    g_free(char_path);
    device_session_event(dev, MB_EV_MIDI_CHAR_FOUND);

    if (!device_ensure_alsa_port(dev)) {
        device_session_event(dev, MB_EV_ALSA_FAILED);
        return false;
    }
    device_session_event(dev, MB_EV_ALSA_READY);

    if (!device_start_workers(dev))
        return false;

    if (!device_start_device_watch(dev)) {
        device_session_event(dev, MB_EV_FATAL);
        return false;
    }

    if (!device_start_notify(dev)) {
        device_session_event(dev, MB_EV_NOTIFY_FAILED);
        return false;
    }
    device_session_event(dev, MB_EV_NOTIFY_OK);

    return device_session_is(dev, MB_SESSION_STREAMING);
}

static const char *lifecycle_command_name(MbLifecycleCommandType type) {
    switch (type) {
    case MB_LIFECYCLE_CMD_DISCOVER: return "DISCOVER";
    case MB_LIFECYCLE_CMD_CONNECT: return "CONNECT";
    case MB_LIFECYCLE_CMD_DISCONNECT: return "DISCONNECT";
    case MB_LIFECYCLE_CMD_RECHECK: return "RECHECK";
    default: return "UNKNOWN";
    }
}

static void lifecycle_command_free(MbLifecycleCommand *cmd) {
    if (!cmd)
        return;

    g_free(cmd->reason);
    g_free(cmd);
}

static gboolean lifecycle_process_next_cb(gpointer user_data);

static bool lifecycle_has_pending(ConfigDirRuntime *rt,
                                  ConfigDeviceRuntime *dev,
                                  MbLifecycleCommandType type) {
    if (!rt || !rt->lifecycle_queue)
        return false;

    for (GList *l = rt->lifecycle_queue->head; l; l = l->next) {
        MbLifecycleCommand *cmd = l->data;
        if (cmd && cmd->dev == dev && cmd->type == type)
            return true;
    }

    return false;
}

static void lifecycle_schedule(ConfigDirRuntime *rt) {
    if (!rt || rt->lifecycle_source_id)
        return;

    rt->lifecycle_source_id = g_idle_add(lifecycle_process_next_cb, rt);
}

static void lifecycle_enqueue(ConfigDirRuntime *rt,
                              ConfigDeviceRuntime *dev,
                              MbLifecycleCommandType type,
                              const char *reason) {
    if (!rt || !dev || !rt->lifecycle_queue)
        return;

    if (lifecycle_has_pending(rt, dev, type))
        return;

    MbLifecycleCommand *cmd = g_new0(MbLifecycleCommand, 1);
    cmd->type = type;
    cmd->dev = dev;
    cmd->reason = g_strdup(reason);

    g_queue_push_tail(rt->lifecycle_queue, cmd);

    g_print("Lifecycle queued %s for %s%s%s.\n",
            lifecycle_command_name(type),
            device_label(dev),
            reason && *reason ? ": " : "",
            reason && *reason ? reason : "");

    lifecycle_schedule(rt);
}

static bool device_lifecycle_discover(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner || !dev->config ||
        !dev->config->address || !*dev->config->address)
        return false;

    ConfigDirRuntime *rt = dev->owner;
    char *bluez_path = mb_bluez_find_device_path_by_address(rt->bus,
                                                            dev->config->address);
    if (!bluez_path) {
        g_print("BlueZ Device1 not found for %s (%s).\n",
                device_label(dev),
                dev->config->address);
        return false;
    }

    g_mutex_lock(&dev->session_lock);
    mb_session_set_identity(dev->session,
                            bluez_path,
                            dev->config->address,
                            dev->config->name);
    g_mutex_unlock(&dev->session_lock);

    g_print("BlueZ Device1 resolved for %s: %s\n",
            device_label(dev),
            bluez_path);

    g_free(bluez_path);
    return true;
}

static void device_lifecycle_disconnect(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner)
        return;

    char *device_path = device_dup_device_path(dev);

    device_stop_notify(dev);

    if (device_path && !g_str_has_prefix(device_path, "config:")) {
        if (!mb_bluez_disconnect_device(dev->owner->bus, device_path))
            g_printerr("BlueZ disconnect failed for %s.\n", device_label(dev));
    }

    device_session_event(dev, MB_EV_CMD_DISCONNECT);

    g_free(device_path);
}

static void device_lifecycle_recheck(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner)
        return;

    char *device_path = device_dup_device_path(dev);

    if (!device_path || g_str_has_prefix(device_path, "config:")) {
        g_free(device_path);
        device_lifecycle_discover(dev);
        return;
    }

    bool connected = false;
    if (!mb_bluez_get_device_bool_property(dev->owner->bus,
                                           device_path,
                                           "Connected",
                                           &connected) ||
        !connected) {
        device_mark_disconnected(dev, "health check");
    }

    g_free(device_path);
}

static gboolean lifecycle_process_next_cb(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;
    if (!rt)
        return G_SOURCE_REMOVE;

    rt->lifecycle_source_id = 0;

    if (rt->lifecycle_busy)
        return G_SOURCE_REMOVE;

    MbLifecycleCommand *cmd = rt->lifecycle_queue
        ? g_queue_pop_head(rt->lifecycle_queue)
        : NULL;

    if (!cmd)
        return G_SOURCE_REMOVE;

    rt->lifecycle_busy = true;

    g_print("Lifecycle processing %s for %s%s%s.\n",
            lifecycle_command_name(cmd->type),
            device_label(cmd->dev),
            cmd->reason && *cmd->reason ? ": " : "",
            cmd->reason && *cmd->reason ? cmd->reason : "");

    switch (cmd->type) {
    case MB_LIFECYCLE_CMD_DISCOVER:
        device_lifecycle_discover(cmd->dev);
        break;

    case MB_LIFECYCLE_CMD_CONNECT:
        if (!device_has_bluez_device_path(cmd->dev))
            device_lifecycle_discover(cmd->dev);

        if (!device_try_start_streaming(cmd->dev))
            g_print("Device %s did not reach STREAMING.\n",
                    device_label(cmd->dev));
        break;

    case MB_LIFECYCLE_CMD_DISCONNECT:
        device_lifecycle_disconnect(cmd->dev);
        break;

    case MB_LIFECYCLE_CMD_RECHECK:
        device_lifecycle_recheck(cmd->dev);
        break;
    }

    lifecycle_command_free(cmd);
    rt->lifecycle_busy = false;

    if (rt->lifecycle_queue && !g_queue_is_empty(rt->lifecycle_queue))
        lifecycle_schedule(rt);

    return G_SOURCE_REMOVE;
}

static gboolean runtime_alsa_rx_poll_cb(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;

    if (!rt->cfg.enable_tx || !rt->seq)
        return G_SOURCE_CONTINUE;

    for (;;) {
        g_mutex_lock(&rt->alsa_lock);
        int pending = snd_seq_event_input_pending(rt->seq, 1);
        if (pending <= 0) {
            g_mutex_unlock(&rt->alsa_lock);
            break;
        }

        snd_seq_event_t *ev = NULL;
        int r = snd_seq_event_input(rt->seq, &ev);
        if (r < 0) {
            if (r != -EAGAIN)
                g_printerr("snd_seq_event_input failed: %s\n", snd_strerror(r));
            g_mutex_unlock(&rt->alsa_lock);
            break;
        }
        if (!ev) {
            g_mutex_unlock(&rt->alsa_lock);
            break;
        }

        ConfigDeviceRuntime *dev = g_hash_table_lookup(rt->device_by_alsa_port,
                                                       GINT_TO_POINTER(ev->dest.port));
        if (!dev) {
            g_mutex_unlock(&rt->alsa_lock);
            continue;
        }

        if (!dev->config->enable_tx || !mb_alsa_event_is_midi_payload(ev)) {
            g_mutex_unlock(&rt->alsa_lock);
            continue;
        }

        uint8_t midi[1024];
        long n = snd_midi_event_decode(dev->alsa_midi_decoder, midi, sizeof(midi), ev);
        if (n < 0) {
            g_printerr("ALSA MIDI decode failed for %s event type %d: %s\n",
                       device_label(dev), ev->type, snd_strerror((int)n));
            snd_midi_event_reset_decode(dev->alsa_midi_decoder);
            g_mutex_unlock(&rt->alsa_lock);
            device_session_event(dev, MB_EV_ALSA_DECODE_FAILED);
            continue;
        }
        g_mutex_unlock(&rt->alsa_lock);

        if (n == 0)
            continue;

        if (rt->cfg.print_midi_events) {
            g_print("ALSA->BLE MIDI[%s]:", device_label(dev));
            for (long i = 0; i < n; i++)
                g_print(" %02x", midi[i]);
            g_print("\n");
        }

        if (!mb_duplex_runtime_push_tx(&dev->runtime, midi, (size_t)n, runtime_now_ns())) {
            g_printerr("TX runtime queue push failed for %s; ALSA event dropped.\n",
                       device_label(dev));
            device_session_event(dev, MB_EV_RUNTIME_FAILED);
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean runtime_device_health_cb(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;

    if (rt->lifecycle_busy ||
        (rt->lifecycle_queue && !g_queue_is_empty(rt->lifecycle_queue)))
        return G_SOURCE_CONTINUE;

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!dev || !device_can_mark_disconnected(dev))
            continue;

        lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_RECHECK, "health");
    }

    return G_SOURCE_CONTINUE;
}

static gboolean runtime_reconnect_cb(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;

    if (rt->lifecycle_busy ||
        (rt->lifecycle_queue && !g_queue_is_empty(rt->lifecycle_queue)))
        return G_SOURCE_CONTINUE;

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (dev && device_session_is(dev, MB_SESSION_RECONNECTING))
            lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_CONNECT, "reconnect timer");
    }

    return G_SOURCE_CONTINUE;
}

static void runtime_start_configured_devices(ConfigDirRuntime *rt) {
    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!dev->config->connect_on_start)
            continue;

        lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_DISCOVER, "startup");
        lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_CONNECT, "startup");
    }
}

static bool runtime_has_streaming_device(ConfigDirRuntime *rt) {
    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (dev && device_session_is(dev, MB_SESSION_STREAMING))
            return true;
    }
    return false;
}

static void print_config_dir_devices(ConfigDirRuntime *rt, const char *config_dir) {
    g_print("midi-ble-rtd\n");
    g_print("Runtime: config-directory multi-session\n");
    g_print("Config: %s\n", config_dir);
    g_print("ALSA client: %s\n", printable_string(rt->cfg.alsa_client_name, "midi-ble-rt"));
    g_print("Service UUID: %s\n", printable_string(rt->cfg.service_uuid, ""));
    g_print("I/O UUID: %s\n", printable_string(rt->cfg.io_uuid, ""));
    g_print("Configured devices: %u\n", mb_config_device_count(&rt->cfg));
    g_print("Runtime devices: %u\n", rt->devices ? rt->devices->len : 0);

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        char *session_path = device_dup_device_path(dev);
        MbSessionState state = device_session_state(dev);
        g_print("\n");
        g_print("Device[%u]: %s\n", i, printable_string(dev->config->id, "(unnamed)"));
        g_print("  address:        %s\n", printable_string(dev->config->address, "(none)"));
        g_print("  name:           %s\n", printable_string(dev->config->name, "(none)"));
        g_print("  profile:        %s\n", printable_string(dev->config->profile, "standard_ble_midi"));
        g_print("  connect_on_start: %s\n", dev->config->connect_on_start ? "yes" : "no");
        g_print("  reconnect_on_link_loss: %s\n", dev->config->reconnect_on_link_loss ? "yes" : "no");
        g_print("  enable_tx:      %s\n", dev->config->enable_tx ? "yes" : "no");
        g_print("  bluez:          %s\n", session_path && !g_str_has_prefix(session_path, "config:") ? "found" : "not found");
        g_print("  session path:   %s\n", printable_string(session_path, "(none)"));
        g_print("  session state:  %s\n", mb_session_state_name(state));
        g_print("  ALSA port:      %d\n", dev->alsa_port);
        g_free(session_path);
    }
}

static void runtime_cleanup(ConfigDirRuntime *rt) {
    if (!rt)
        return;

    if (rt->sigint_source_id)
        g_source_remove(rt->sigint_source_id);
    if (rt->sigterm_source_id)
        g_source_remove(rt->sigterm_source_id);
    if (rt->reconnect_source_id)
        g_source_remove(rt->reconnect_source_id);
    if (rt->health_source_id)
        g_source_remove(rt->health_source_id);
    if (rt->alsa_source_id)
        g_source_remove(rt->alsa_source_id);
    if (rt->lifecycle_source_id)
        g_source_remove(rt->lifecycle_source_id);

    if (rt->lifecycle_queue) {
        while (!g_queue_is_empty(rt->lifecycle_queue))
            lifecycle_command_free(g_queue_pop_head(rt->lifecycle_queue));
        g_queue_free(rt->lifecycle_queue);
        rt->lifecycle_queue = NULL;
    }

    if (rt->devices)
        g_ptr_array_unref(rt->devices);
    if (rt->seq)
        snd_seq_close(rt->seq);
    if (rt->device_by_char_path)
        g_hash_table_unref(rt->device_by_char_path);
    if (rt->device_by_alsa_port)
        g_hash_table_unref(rt->device_by_alsa_port);
    mb_daemon_clear(&rt->daemon);
    mb_config_clear(&rt->cfg);
    if (rt->loop)
        g_main_loop_unref(rt->loop);
    if (rt->bus)
        g_object_unref(rt->bus);

    g_mutex_clear(&rt->alsa_lock);
}

static int run_config_directory_mode(const char *config_dir) {
    ConfigDirRuntime rt;
    memset(&rt, 0, sizeof(rt));
    g_mutex_init(&rt.alsa_lock);
    rt.lifecycle_queue = g_queue_new();
    rt.lifecycle_busy = false;
    rt.lifecycle_source_id = 0;
    rt.device_by_alsa_port = g_hash_table_new(g_direct_hash, g_direct_equal);
    rt.device_by_char_path = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (!mb_config_load_dir(&rt.cfg, config_dir)) {
        runtime_cleanup(&rt);
        return 1;
    }

    GError *error = NULL;
    rt.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!rt.bus) {
        g_printerr("Could not connect to system bus: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        runtime_cleanup(&rt);
        return 1;
    }

    runtime_build_sessions(&rt);
    runtime_resolve_bluez_devices(&rt);
    runtime_create_devices(&rt);

    rt.loop = g_main_loop_new(NULL, FALSE);
    rt.sigint_source_id = g_unix_signal_add(SIGINT, quit_main_loop, &rt);
    rt.sigterm_source_id = g_unix_signal_add(SIGTERM, quit_main_loop, &rt);
    rt.reconnect_source_id = g_timeout_add(10000, runtime_reconnect_cb, &rt);
    rt.health_source_id = g_timeout_add(1000, runtime_device_health_cb, &rt);
    rt.alsa_source_id = g_timeout_add(1, runtime_alsa_rx_poll_cb, &rt);

    print_config_dir_devices(&rt, config_dir);
    runtime_start_configured_devices(&rt);

    if (!runtime_has_streaming_device(&rt)) {
        if (rt.lifecycle_queue && !g_queue_is_empty(rt.lifecycle_queue))
            g_print("Startup lifecycle commands queued; daemon will attempt connection from the main loop.\n");
        else
            g_print("No configured device reached STREAMING yet; daemon stays alive for reconnect/manual future control.\n");
    }

    g_print("\nDaemon loop: running. Press Ctrl-C to exit.\n");
    g_main_loop_run(rt.loop);
    g_print("Daemon loop: stopping.\n");

    runtime_cleanup(&rt);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && g_strcmp0(argv[1], "--config") == 0) {
        if (g_file_test(argv[2], G_FILE_TEST_IS_DIR))
            return run_config_directory_mode(argv[2]);

        return mb_orchestrator_main(argc, argv);
    }

    if (argc == 3 && g_strcmp0(argv[1], "--config-dir") == 0)
        return run_config_directory_mode(argv[2]);

    return mb_orchestrator_main(argc, argv);
}
