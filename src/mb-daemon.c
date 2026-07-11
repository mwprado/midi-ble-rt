/*
 * midi-ble-rtd process entry point.
 *
 * The daemon now supports only configuration-directory mode: one MbDaemon,
 * one shared BlueZ bus, one shared ALSA client, one MbSession and one duplex
 * runtime per configured BLE-MIDI device.
 *
 * The old single-file/single-session compatibility path was removed so every
 * runtime execution uses the same multi-device session/dataplane model.
 */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <glib.h>
#include <poll.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "mb-alsa.h"
#include "mb-alsa-port.h"
#include "mb-ble-midi.h"
#include "mb-bluez.h"
#include "mb-config.h"
#include "mb-duplex-runtime.h"
#include "mb-gatt-midi.h"
#include "mb-session.h"
#include "mb-stats.h"
#include "mb-rtkit.h"
#include "mb-timeouts.h"

static const char *printable_string(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

typedef struct _ConfigDirRuntime ConfigDirRuntime;
static bool runtime_start_alsa_tx_thread(ConfigDirRuntime *rt);

typedef struct {
    ConfigDirRuntime *owner;
    const MbDeviceConfig *config;
    MbSession *session;
    GMutex session_lock;

    GMutex dataplane_lock;
    bool dataplane_streaming;
    uint64_t dataplane_epoch;
    uint64_t dataplane_closed_epoch;
    uint64_t dataplane_closed_at_ns;

    MbDuplexRuntime runtime;
    bool runtime_started;
    MbSessionStats stats;

    /*
     * Dataplane TX lock per device.
     *
     * Cada dispositivo BLE-MIDI em STREAMING tem seu próprio TX worker.
     * O lock é por dispositivo, não global, para permitir instrumentos
     * tocando simultaneamente sem serializar todos os GATT WriteValue.
     */
    GMutex gatt_write_lock;

    /*
     * Stable local ALSA endpoint for this configured device.
     *
     * The ALSA port lifetime is tied to the ConfigDeviceRuntime, not to the
     * momentary BLE/GATT link state.  A short radio drop, notification loss or
     * reconnect cycle must change the session state and guard/drop dataplane
     * I/O, but it must not destroy this ALSA port.
     *
     * This preserves user routing in ALSA/PipeWire-aware applications:
     *
     *   BLE link drops briefly  ->  port remains visible
     *   BLE link reconnects     ->  same port resumes streaming
     *   daemon exits            ->  ALSA client and all ports disappear
     *
     * Do not close/recreate per-device ALSA ports as part of normal BLE
     * disconnect/reconnect handling.  Close them only when tearing down the
     * device runtime or the whole daemon.
     */
    int alsa_port;
    bool removed;

    bool dbus_last_state_valid;
    MbSessionState dbus_last_state;
    snd_midi_event_t *alsa_midi_encoder;
    snd_midi_event_t *alsa_midi_decoder;

    guint device_sub_id;
    guint notify_sub_id;

    MbBleMidiDecoderState ble_midi_decoder;
} ConfigDeviceRuntime;

static void runtime_dbus_emit_device_changed(ConfigDeviceRuntime *dev);

typedef enum {
    MB_LIFECYCLE_CMD_DISCOVER,
    MB_LIFECYCLE_CMD_CONNECT,
    MB_LIFECYCLE_CMD_DISCONNECT,
    MB_LIFECYCLE_CMD_RECHECK,
    MB_LIFECYCLE_CMD_MARK_DISCONNECTED,
    MB_LIFECYCLE_CMD_SESSION_EVENT,
} MbLifecycleCommandType;

typedef struct {
    MbLifecycleCommandType type;
    ConfigDeviceRuntime *dev;
    MbEventType event;
    char *reason;
} MbLifecycleCommand;

struct _ConfigDirRuntime {
    MbConfig cfg;
    MbDaemon daemon;

    GDBusConnection *bus;
    GMainLoop *loop;
    char *config_dir;

    /*
     * User-session D-Bus API for GUI clients.
     *
     * BlueZ remains on the system bus through rt->bus.  This connection is a
     * user-session control/status API owned by midi-ble-rtd, so the GUI can be
     * a thin client instead of duplicating daemon/catalog/runtime logic.
     */
    GDBusConnection *dbus_bus;
    GDBusNodeInfo *dbus_introspection;
    guint dbus_name_id;
    guint dbus_object_id;

    /*
     * Shared ALSA Sequencer client for the whole daemon process.
     *
     * The daemon owns one ALSA client, and each configured BLE-MIDI device
     * gets its own ALSA port inside that client.  For example:
     *
     *   midi-ble-rt client 128
     *     128:0  first configured instrument/controller
     *     128:1  second configured instrument/controller
     *     128:2  third configured instrument/controller
     *
     * The client id is assigned dynamically by ALSA and may change between
     * daemon restarts.  Port numbers are local endpoints created by this
     * daemon for configured devices.
     */
    snd_seq_t *seq;

    GPtrArray *devices;
    GHashTable *device_by_alsa_port;
    GHashTable *device_by_char_path;

    GMutex alsa_lock;
    GMutex lifecycle_lock;
    GMutex stats_lock;

    MbStats stats;
    guint stats_source_id;

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

    /*
     * Local control socket.
     *
     * IPC endpoint for midi-ble-rtctl -> daemon communication.
     *
     * Read-only commands expose daemon state. Lifecycle-changing commands are
     * translated into lifecycle queue entries and executed by the main-loop
     * monitor, never directly from the control socket handler.
     */
    int control_socket_fd;
    guint control_socket_source_id;
    char *control_socket_path;

    GThread *alsa_tx_thread;
    gint alsa_tx_stop_requested;
    guint health_source_id;
    guint reconnect_source_id;
    guint sigint_source_id;
    guint sigterm_source_id;
};

static uint64_t runtime_now_ns(void) {
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static uint16_t runtime_ble_midi_timestamp_13bit(void) {
    gint64 ms = g_get_monotonic_time() / G_TIME_SPAN_MILLISECOND;
    return (uint16_t)(ms & 0x1fff);
}

static bool device_dataplane_fence_accept(ConfigDeviceRuntime *dev,
                                          uint64_t epoch,
                                          uint64_t timestamp_ns) {
    if (!dev)
        return false;

    g_mutex_lock(&dev->dataplane_lock);

    bool ok = dev->dataplane_streaming &&
              epoch == dev->dataplane_epoch;

    if (ok &&
        epoch == dev->dataplane_closed_epoch &&
        timestamp_ns <= dev->dataplane_closed_at_ns)
        ok = false;

    g_mutex_unlock(&dev->dataplane_lock);
    return ok;
}

static bool device_dataplane_stamp(ConfigDeviceRuntime *dev,
                                   uint64_t *epoch,
                                   uint64_t *timestamp_ns) {
    if (!dev || !epoch || !timestamp_ns)
        return false;

    uint64_t now_ns = runtime_now_ns();

    g_mutex_lock(&dev->dataplane_lock);

    bool ok = dev->dataplane_streaming;
    if (ok) {
        *epoch = dev->dataplane_epoch;
        *timestamp_ns = now_ns;
    }

    g_mutex_unlock(&dev->dataplane_lock);
    return ok;
}

static void device_dataplane_reset_partial_state(ConfigDeviceRuntime *dev) {
    if (!dev)
        return;

    if (dev->owner)
        g_mutex_lock(&dev->owner->alsa_lock);

    mb_ble_midi_decoder_reset(&dev->ble_midi_decoder);

    if (dev->alsa_midi_encoder)
        snd_midi_event_reset_encode(dev->alsa_midi_encoder);

    if (dev->alsa_midi_decoder)
        snd_midi_event_reset_decode(dev->alsa_midi_decoder);

    if (dev->owner)
        g_mutex_unlock(&dev->owner->alsa_lock);
}

static void device_dataplane_on_session_change(ConfigDeviceRuntime *dev,
                                               MbSessionState before,
                                               MbSessionState after) {
    if (!dev || before == after)
        return;

    uint64_t now_ns = runtime_now_ns();

    if (before != MB_SESSION_STREAMING && after == MB_SESSION_STREAMING) {
        g_mutex_lock(&dev->dataplane_lock);
        dev->dataplane_epoch++;
        if (dev->dataplane_epoch == 0)
            dev->dataplane_epoch = 1;
        dev->dataplane_streaming = true;
        g_mutex_unlock(&dev->dataplane_lock);
        return;
    }

    if (before == MB_SESSION_STREAMING && after != MB_SESSION_STREAMING) {
        g_mutex_lock(&dev->dataplane_lock);
        dev->dataplane_streaming = false;
        dev->dataplane_closed_epoch = dev->dataplane_epoch;
        dev->dataplane_closed_at_ns = now_ns;
        g_mutex_unlock(&dev->dataplane_lock);

        mb_duplex_runtime_drop_pending(&dev->runtime);
        device_dataplane_reset_partial_state(dev);
    }
}

static gboolean quit_main_loop(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;
    if (rt && rt->loop)
        g_main_loop_quit(rt->loop);

    /*
     * Keep the signal source registered until runtime_cleanup().
     * Returning G_SOURCE_REMOVE here makes GLib remove it immediately, while
     * runtime_cleanup() still has the old source id and tries to remove it
     * again, producing a harmless but noisy GLib-CRITICAL on Ctrl-C shutdown.
     */
    return G_SOURCE_CONTINUE;
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

static bool runtime_device_is_active(const ConfigDeviceRuntime *dev) {
    return dev && !dev->removed && dev->config;
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

    if (ok && before != after)
        device_dataplane_on_session_change(dev, before, after);

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

static void lifecycle_enqueue(ConfigDirRuntime *rt,
                              ConfigDeviceRuntime *dev,
                              MbLifecycleCommandType type,
                              const char *reason);

static void lifecycle_enqueue_event(ConfigDirRuntime *rt,
                                    ConfigDeviceRuntime *dev,
                                    MbEventType event,
                                    const char *reason);


static void runtime_device_stats_ensure_window(ConfigDeviceRuntime *dev,
                                               uint64_t now_ns);
static void runtime_device_stats_reset_window(ConfigDeviceRuntime *dev,
                                              uint64_t now_ns);

static uint64_t runtime_stats_v5_ns_to_ms(uint64_t ns) {
    return ns / 1000000ULL;
}

static uint64_t runtime_stats_v5_elapsed_ms(uint64_t now_ns, uint64_t then_ns) {
    if (then_ns == 0 || now_ns < then_ns)
        return 0;
    return runtime_stats_v5_ns_to_ms(now_ns - then_ns);
}

static double runtime_stats_v5_rate_per_sec(uint64_t count, uint64_t window_ns) {
    if (window_ns < 100000000ULL)
        return 0.0;
    return ((double)count * 1000000000.0) / (double)window_ns;
}

static char *runtime_stats_dup_tsv_field(const char *value) {
    char *out = g_strdup(value && *value ? value : "-");
    for (char *p = out; p && *p; p++) {
        if (*p == 9 || *p == 10 || *p == 13)
            *p = ' ';
    }
    return out;
}

static void runtime_stats_append_v5_row(GString *content,
                                        ConfigDirRuntime *rt,
                                        ConfigDeviceRuntime *dev,
                                        bool tx,
                                        int alsa_client_id,
                                        uint64_t now_ns,
                                        uint8_t queue_depth) {
    if (!content || !rt || !dev)
        return;

    MbSessionStats *st = &dev->stats;
    if (queue_depth > (tx ? st->tx_queue_high_watermark : st->rx_queue_high_watermark)) {
        if (tx)
            st->tx_queue_high_watermark = queue_depth;
        else
            st->rx_queue_high_watermark = queue_depth;
    }

    uint64_t window_ns = 0;
    if (st->window_started_ns != 0 && now_ns >= st->window_started_ns)
        window_ns = now_ns - st->window_started_ns;

    char *id = runtime_stats_dup_tsv_field(dev->config ? dev->config->id : "-");
    char *address = runtime_stats_dup_tsv_field(dev->config ? dev->config->address : "-");
    char *name = runtime_stats_dup_tsv_field(device_label(dev));
    char *state = runtime_stats_dup_tsv_field(mb_session_state_name(device_session_state(dev)));

    uint64_t packets = tx ? st->tx_packets : st->rx_packets;
    uint64_t bytes = tx ? st->tx_bytes : st->rx_bytes;
    uint64_t drops = tx ? st->tx_drops : st->rx_drops;
    uint64_t last_ms = tx
        ? runtime_stats_v5_elapsed_ms(now_ns, st->last_tx_ns)
        : runtime_stats_v5_elapsed_ms(now_ns, st->last_rx_ns);
    uint64_t gap_avg_ms = tx
        ? runtime_stats_v5_ns_to_ms(st->tx_gap_avg_ns)
        : runtime_stats_v5_ns_to_ms(st->rx_gap_avg_ns);
    uint64_t gap_max_ms = tx
        ? runtime_stats_v5_ns_to_ms(st->tx_gap_max_ns)
        : runtime_stats_v5_ns_to_ms(st->rx_gap_max_ns);
    unsigned high_watermark = tx
        ? st->tx_queue_high_watermark
        : st->rx_queue_high_watermark;

    g_string_append_printf(
        content,
        "%s\t%s\t%s\t%s\t%d\t%d\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%s\t"
        "%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t"
        "%.3f\t%.3f\t%.3f\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t"
        "%u\t%u\n",
        id,
        address,
        name,
        state,
        alsa_client_id,
        dev->alsa_port,
        runtime_stats_v5_elapsed_ms(now_ns, rt->stats.started_ns),
        runtime_stats_v5_ns_to_ms(window_ns),
        tx ? "TX" : "RX",
        packets,
        bytes,
        drops,
        runtime_stats_v5_rate_per_sec(packets, window_ns),
        runtime_stats_v5_rate_per_sec(bytes, window_ns),
        runtime_stats_v5_rate_per_sec(drops, window_ns),
        last_ms,
        gap_avg_ms,
        gap_max_ms,
        (unsigned)queue_depth,
        high_watermark);

    g_free(id);
    g_free(address);
    g_free(name);
    g_free(state);
}

static void runtime_stats_export_snapshot(ConfigDirRuntime *rt) {
    if (!rt || !rt->stats.enabled || !rt->stats.path)
        return;

    int alsa_client_id = rt->seq ? snd_seq_client_id(rt->seq) : -1;
    uint64_t now_ns = runtime_now_ns();

    char *dir = g_path_get_dirname(rt->stats.path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_printerr("Stats export failed: failed to create stats directory: %s\n", dir);
        g_free(dir);
        return;
    }
    g_free(dir);

    GString *content = g_string_new(
        "v5\n"
        "id\taddress\tname\tstate\talsa_client_id\talsa_port_id\tuptime_ms\twindow_ms\tdir\t"
        "packets\tbytes\tdrops\tpackets_per_sec\tbytes_per_sec\tdrops_per_sec\t"
        "last_ms\tgap_avg_ms\tgap_max_ms\tqueue_depth\tqueue_high_watermark\n");

    g_mutex_lock(&rt->stats_lock);

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!runtime_device_is_active(dev))
            continue;

        runtime_device_stats_ensure_window(dev, now_ns);

        uint8_t rx_depth = dev->runtime_started
            ? mb_duplex_runtime_rx_depth(&dev->runtime)
            : 0;
        uint8_t tx_depth = dev->runtime_started
            ? mb_duplex_runtime_tx_depth(&dev->runtime)
            : 0;

        runtime_stats_append_v5_row(content, rt, dev, false, alsa_client_id, now_ns, rx_depth);
        runtime_stats_append_v5_row(content, rt, dev, true, alsa_client_id, now_ns, tx_depth);
    }

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!runtime_device_is_active(dev))
            continue;

        runtime_device_stats_reset_window(dev, now_ns);
    }

    g_mutex_unlock(&rt->stats_lock);

    GError *error = NULL;
    if (!g_file_set_contents(rt->stats.path, content->str, -1, &error)) {
        g_printerr("Stats export failed: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
    }

    g_string_free(content, TRUE);
}

static gboolean runtime_stats_export_cb(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;
    runtime_stats_export_snapshot(rt);
    return G_SOURCE_CONTINUE;
}

static void runtime_stats_rx_packet(ConfigDirRuntime *rt, size_t bytes, uint64_t now_ns) {
    if (!rt)
        return;
    g_mutex_lock(&rt->stats_lock);
    mb_stats_rx_packet(&rt->stats, bytes, now_ns);
    g_mutex_unlock(&rt->stats_lock);
}

static void runtime_stats_tx_packet(ConfigDirRuntime *rt, size_t bytes, uint64_t now_ns) {
    if (!rt)
        return;
    g_mutex_lock(&rt->stats_lock);
    mb_stats_tx_packet(&rt->stats, bytes, now_ns);
    g_mutex_unlock(&rt->stats_lock);
}

static void runtime_stats_rx_drop(ConfigDirRuntime *rt) {
    if (!rt)
        return;
    g_mutex_lock(&rt->stats_lock);
    mb_stats_rx_drop(&rt->stats);
    g_mutex_unlock(&rt->stats_lock);
}

static void runtime_stats_tx_drop(ConfigDirRuntime *rt) {
    if (!rt)
        return;
    g_mutex_lock(&rt->stats_lock);
    mb_stats_tx_drop(&rt->stats);
    g_mutex_unlock(&rt->stats_lock);
}

static void runtime_stats_observe_queue_depth(ConfigDirRuntime *rt,
                                              bool tx,
                                              unsigned queue_depth) {
    if (!rt)
        return;

    g_mutex_lock(&rt->stats_lock);
    if (tx)
        mb_stats_queue_depth(&rt->stats, 0, queue_depth);
    else
        mb_stats_queue_depth(&rt->stats, queue_depth, 0);
    g_mutex_unlock(&rt->stats_lock);
}


static void runtime_stats_add_u64_saturating(uint64_t *value, uint64_t add) {
    if (!value)
        return;
    if (UINT64_MAX - *value < add)
        *value = UINT64_MAX;
    else
        *value += add;
}

static void runtime_device_stats_ensure_window(ConfigDeviceRuntime *dev, uint64_t now_ns) {
    if (dev && dev->stats.window_started_ns == 0)
        dev->stats.window_started_ns = now_ns;
}

static void runtime_device_stats_reset_window(ConfigDeviceRuntime *dev, uint64_t now_ns) {
    if (!dev)
        return;

    dev->stats.rx_packets = 0;
    dev->stats.tx_packets = 0;
    dev->stats.rx_bytes = 0;
    dev->stats.tx_bytes = 0;
    dev->stats.rx_drops = 0;
    dev->stats.tx_drops = 0;
    dev->stats.rx_queue_high_watermark = 0;
    dev->stats.tx_queue_high_watermark = 0;
    dev->stats.window_started_ns = now_ns;
}

static void runtime_device_stats_update_gap(uint64_t *last_ns,
                                            uint64_t *avg_ns,
                                            uint64_t *max_ns,
                                            uint64_t now_ns) {
    if (*last_ns != 0 && now_ns >= *last_ns) {
        uint64_t gap = now_ns - *last_ns;
        if (*avg_ns == 0)
            *avg_ns = gap;
        else
            *avg_ns = ((*avg_ns * 7ULL) + gap) / 8ULL;
        if (gap > *max_ns)
            *max_ns = gap;
    }

    *last_ns = now_ns;
}

static void runtime_device_stats_rx_packet(ConfigDeviceRuntime *dev,
                                           size_t bytes,
                                           uint64_t now_ns) {
    if (!dev || !dev->owner || !dev->owner->stats.enabled)
        return;

    g_mutex_lock(&dev->owner->stats_lock);
    runtime_device_stats_ensure_window(dev, now_ns);
    runtime_stats_add_u64_saturating(&dev->stats.rx_packets, 1);
    runtime_stats_add_u64_saturating(&dev->stats.rx_bytes, (uint64_t)bytes);
    runtime_device_stats_update_gap(&dev->stats.last_rx_ns,
                                    &dev->stats.rx_gap_avg_ns,
                                    &dev->stats.rx_gap_max_ns,
                                    now_ns);
    g_mutex_unlock(&dev->owner->stats_lock);
}

static void runtime_device_stats_tx_packet(ConfigDeviceRuntime *dev,
                                           size_t bytes,
                                           uint64_t now_ns) {
    if (!dev || !dev->owner || !dev->owner->stats.enabled)
        return;

    g_mutex_lock(&dev->owner->stats_lock);
    runtime_device_stats_ensure_window(dev, now_ns);
    runtime_stats_add_u64_saturating(&dev->stats.tx_packets, 1);
    runtime_stats_add_u64_saturating(&dev->stats.tx_bytes, (uint64_t)bytes);
    runtime_device_stats_update_gap(&dev->stats.last_tx_ns,
                                    &dev->stats.tx_gap_avg_ns,
                                    &dev->stats.tx_gap_max_ns,
                                    now_ns);
    g_mutex_unlock(&dev->owner->stats_lock);
}

static void runtime_device_stats_rx_drop(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner || !dev->owner->stats.enabled)
        return;

    g_mutex_lock(&dev->owner->stats_lock);
    runtime_device_stats_ensure_window(dev, runtime_now_ns());
    runtime_stats_add_u64_saturating(&dev->stats.rx_drops, 1);
    g_mutex_unlock(&dev->owner->stats_lock);
}

static void runtime_device_stats_tx_drop(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner || !dev->owner->stats.enabled)
        return;

    g_mutex_lock(&dev->owner->stats_lock);
    runtime_device_stats_ensure_window(dev, runtime_now_ns());
    runtime_stats_add_u64_saturating(&dev->stats.tx_drops, 1);
    g_mutex_unlock(&dev->owner->stats_lock);
}

static void runtime_device_stats_observe_queue_depth(ConfigDeviceRuntime *dev,
                                                     bool tx,
                                                     unsigned queue_depth) {
    if (!dev || !dev->owner || !dev->owner->stats.enabled)
        return;

    g_mutex_lock(&dev->owner->stats_lock);
    runtime_device_stats_ensure_window(dev, runtime_now_ns());
    if (tx) {
        if (queue_depth > dev->stats.tx_queue_high_watermark)
            dev->stats.tx_queue_high_watermark = queue_depth;
    } else {
        if (queue_depth > dev->stats.rx_queue_high_watermark)
            dev->stats.rx_queue_high_watermark = queue_depth;
    }
    g_mutex_unlock(&dev->owner->stats_lock);
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
    g_mutex_clear(&dev->dataplane_lock);
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
        dev->stats.window_started_ns = runtime_now_ns();
        g_mutex_init(&dev->session_lock);
        g_mutex_init(&dev->dataplane_lock);
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

    if (dev->alsa_port >= 0) {
        if (!runtime_start_alsa_tx_thread(rt))
            g_printerr("ALSA TX thread disabled; TX from ALSA will not run.\n");
        return true;
    }

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

    if (!runtime_start_alsa_tx_thread(rt))
        g_printerr("ALSA TX thread disabled; TX from ALSA will not run.\n");

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


static void device_alsa_emit_midi_panic(ConfigDeviceRuntime *dev, const char *reason) {
    if (!dev || !dev->alsa_midi_encoder)
        return;

    /*
     * RX resync policy: if BLE-MIDI framing becomes ambiguous, do not let an
     * old partial message be completed by future bytes.  Reset ALSA's byte
     * encoder and emit a conservative MIDI panic to downstream ALSA clients so
     * a lost Note Off, sustain-off, or controller reset does not leave sound
     * stuck after we drop the suspect fragment.
     */
    g_printerr("MIDI RX resync for %s%s%s; emitting panic to ALSA.\n",
               device_label(dev),
               reason && *reason ? ": " : "",
               reason && *reason ? reason : "");

    snd_midi_event_reset_encode(dev->alsa_midi_encoder);

    for (uint8_t ch = 0; ch < 16; ch++) {
        uint8_t cc = 0xB0 | ch;
        uint8_t bend = 0xE0 | ch;

        device_alsa_emit_midi_byte(dev, cc);
        device_alsa_emit_midi_byte(dev, 64);
        device_alsa_emit_midi_byte(dev, 0);

        device_alsa_emit_midi_byte(dev, cc);
        device_alsa_emit_midi_byte(dev, 120);
        device_alsa_emit_midi_byte(dev, 0);

        device_alsa_emit_midi_byte(dev, cc);
        device_alsa_emit_midi_byte(dev, 123);
        device_alsa_emit_midi_byte(dev, 0);

        device_alsa_emit_midi_byte(dev, bend);
        device_alsa_emit_midi_byte(dev, 0x00);
        device_alsa_emit_midi_byte(dev, 0x40);
    }
}

static void device_ble_midi_emit_byte(uint8_t byte, void *user_data) {
    device_alsa_emit_midi_byte((ConfigDeviceRuntime *)user_data, byte);
}

/*
 * Per-device realtime dataplane.
 *
 * The daemon exposes one stable ALSA port per configured BLE-MIDI device, but
 * the BLE/GATT streaming session is transient.  RX and TX are independent
 * asynchronous flows:
 *
 *   instrument/controller -> BLE -> buffer RX -> ALSA
 *   ALSA -> buffer TX -> BLE -> instrument/controller
 *
 * Buffer items are jitter absorbers inside a live STREAMING session.  They are
 * not replay queues across BLE session loss.  If the device leaves STREAMING,
 * pending RX/TX data for that device should be dropped/invalidated and the next
 * STREAMING session should start clean, while the ALSA port remains visible.
 *
 * Runtime flow consumption is item based: each consumer callback receives one
 * ring item per call.  The current frame-model payload capacity is
 * MB_FRAME_MODEL_CAPACITY bytes per item.  TX items may be split later into
 * multiple BLE-MIDI packets; MB_BLE_MIDI_MAX_MIDI_BYTES_PER_PACKET controls the
 * per-WriteValue MIDI payload size.
 */
static void device_ble_midi_decode_packet(ConfigDeviceRuntime *dev,
                                          const uint8_t *p,
                                          size_t len) {
    if (!dev || !p || len < 3)
        return;

    if (dev->owner->cfg.print_ble_packets) {
        g_print("BLE packet[%s]:", device_label(dev));
        for (size_t k = 0; k < len; k++)
            g_print(" %02x", p[k]);
        g_print("\n");
    }

    MbBleMidiDecodeResult result =
        mb_ble_midi_decode_packet(&dev->ble_midi_decoder,
                                  p,
                                  len,
                                  device_ble_midi_emit_byte,
                                  dev);

    if (result == MB_BLE_MIDI_DECODE_INVALID_HEADER) {
        g_printerr("Invalid BLE-MIDI header byte from %s: 0x%02x\n",
                   device_label(dev), p[0]);
        mb_ble_midi_decoder_reset(&dev->ble_midi_decoder);
        device_alsa_emit_midi_panic(dev, "invalid BLE-MIDI header");
        runtime_stats_rx_drop(dev->owner);
        runtime_device_stats_rx_drop(dev);
    } else if (result == MB_BLE_MIDI_DECODE_RESYNC) {
        device_alsa_emit_midi_panic(dev, "ambiguous or orphaned MIDI fragment");
        runtime_stats_rx_drop(dev->owner);
        runtime_device_stats_rx_drop(dev);
    }
}

static bool device_ble_midi_write_packet(ConfigDeviceRuntime *dev,
                                         const uint8_t *midi,
                                         size_t len,
                                         uint64_t epoch,
                                         uint64_t timestamp_ns) {
    if (!midi || len == 0)
        return true;

    if (!device_dataplane_fence_accept(dev, epoch, timestamp_ns)) {
        runtime_stats_tx_drop(dev->owner);
        runtime_device_stats_tx_drop(dev);
        return false;
    }

    char *char_path = device_dup_char_path(dev);
    if (!char_path) {
        g_printerr("No GATT characteristic path for %s.\n", device_label(dev));
        runtime_stats_tx_drop(dev->owner);
        runtime_device_stats_tx_drop(dev);
        return false;
    }

    const size_t max_midi_per_packet = MB_BLE_MIDI_MAX_MIDI_BYTES_PER_PACKET;
    size_t off = 0;
    bool ok = true;

    while (off < len) {
        if (!device_dataplane_fence_accept(dev, epoch, timestamp_ns)) {
            runtime_stats_tx_drop(dev->owner);
            runtime_device_stats_tx_drop(dev);
            ok = false;
            break;
        }

        size_t chunk = len - off;
        if (chunk > max_midi_per_packet)
            chunk = max_midi_per_packet;

        uint8_t packet[MB_BLE_MIDI_PACKET_MAX_BYTES];
        size_t packet_len = 0;
        if (!mb_ble_midi_make_packet(runtime_ble_midi_timestamp_13bit(),
                                     &midi[off], chunk,
                                     packet, sizeof(packet), &packet_len)) {
            g_printerr("BLE-MIDI packet construction failed for %s.\n",
                       device_label(dev));
            runtime_stats_tx_drop(dev->owner);
        runtime_device_stats_tx_drop(dev);
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
                                               MB_GATT_WRITE_VALUE_TIMEOUT_MS)) {
            runtime_stats_tx_drop(dev->owner);
        runtime_device_stats_tx_drop(dev);
            lifecycle_enqueue_event(dev->owner,
                                    dev,
                                    MB_EV_GATT_WRITE_FAILED,
                                    "GATT write failed");
            ok = false;
            break;
        }

        uint64_t now_ns = runtime_now_ns();
        runtime_stats_tx_packet(dev->owner, chunk, now_ns);
        runtime_device_stats_tx_packet(dev, chunk, now_ns);
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
    ConfigDeviceRuntime *dev = user_data;
    if (!dev)
        return;

    if (!item || !device_dataplane_fence_accept(dev, item->epoch, item->timestamp_ns)) {
        runtime_stats_rx_drop(dev->owner);
        runtime_device_stats_rx_drop(dev);
        return;
    }

    unsigned depth = (unsigned)mb_runtime_flow_depth(flow) + 1u;
    runtime_stats_observe_queue_depth(dev->owner, false, depth);
    runtime_device_stats_observe_queue_depth(dev, false, depth);

    g_mutex_lock(&dev->owner->alsa_lock);
    device_ble_midi_decode_packet(dev, data, len);
    g_mutex_unlock(&dev->owner->alsa_lock);
}

static void device_tx_consume(MbRuntimeFlow *flow,
                              const MbSliceRingItem *item,
                              const uint8_t *data,
                              size_t len,
                              void *user_data) {
    ConfigDeviceRuntime *dev = user_data;
    if (!dev)
        return;

    if (!item || !device_dataplane_fence_accept(dev, item->epoch, item->timestamp_ns)) {
        runtime_stats_tx_drop(dev->owner);
        runtime_device_stats_tx_drop(dev);
        return;
    }

    unsigned depth = (unsigned)mb_runtime_flow_depth(flow) + 1u;
    runtime_stats_observe_queue_depth(dev->owner, true, depth);
    runtime_device_stats_observe_queue_depth(dev, true, depth);

    g_mutex_lock(&dev->gatt_write_lock);
    device_ble_midi_write_packet(dev,
                                 data,
                                 len,
                                 item->epoch,
                                 item->timestamp_ns);
    g_mutex_unlock(&dev->gatt_write_lock);
}

static void device_rx_observe_depth(MbRuntimeFlow *flow,
                                    uint8_t queue_depth,
                                    void *user_data) {
    (void)flow;
    ConfigDeviceRuntime *dev = user_data;
    if (dev)
        runtime_stats_observe_queue_depth(dev->owner, false, queue_depth);
}

static void device_tx_observe_depth(MbRuntimeFlow *flow,
                                    uint8_t queue_depth,
                                    void *user_data) {
    (void)flow;
    ConfigDeviceRuntime *dev = user_data;
    if (dev)
        runtime_stats_observe_queue_depth(dev->owner, true, queue_depth);
}

static bool device_start_workers(ConfigDeviceRuntime *dev) {
    if (dev->runtime_started)
        return true;

    mb_duplex_runtime_init(&dev->runtime,
                           device_rx_consume, dev,
                           device_tx_consume, dev);
    mb_runtime_flow_set_depth_observer(&dev->runtime.rx,
                                       device_rx_observe_depth,
                                       dev);
    mb_runtime_flow_set_depth_observer(&dev->runtime.tx,
                                       device_tx_observe_depth,
                                       dev);

    if (!mb_duplex_runtime_start(&dev->runtime)) {
        g_printerr("Could not start duplex runtime workers for %s.\n",
                   device_label(dev));
        lifecycle_enqueue_event(dev->owner,
                                dev,
                                MB_EV_RUNTIME_FAILED,
                                "duplex runtime start failed");
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
                lifecycle_enqueue(dev->owner,
                                  dev,
                                  MB_LIFECYCLE_CMD_MARK_DISCONNECTED,
                                  "Device1.Connected=false");
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
            uint64_t epoch = 0;
            uint64_t now_ns = 0;
            if (!device_dataplane_stamp(dev, &epoch, &now_ns)) {
                runtime_stats_rx_drop(dev->owner);
                runtime_device_stats_rx_drop(dev);
            } else if (!mb_duplex_runtime_push_rx_with_epoch(&dev->runtime,
                                                             bytes,
                                                             len,
                                                             now_ns,
                                                             epoch)) {
                g_printerr("RX runtime queue push failed for %s; BLE notification dropped.\n",
                           device_label(dev));
                runtime_stats_rx_drop(dev->owner);
                runtime_device_stats_rx_drop(dev);
                lifecycle_enqueue_event(dev->owner,
                                        dev,
                                        MB_EV_RUNTIME_FAILED,
                                        "RX runtime queue push failed");
            } else {
                runtime_stats_rx_packet(dev->owner, len, now_ns);
                runtime_device_stats_rx_packet(dev, len, now_ns);
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
                                   MB_GATT_START_NOTIFY_TIMEOUT_MS,
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

    if (!mb_bluez_wait_services_resolved(rt->bus, device_path, MB_GATT_SERVICES_RESOLVED_TIMEOUT_MS)) {
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
    case MB_LIFECYCLE_CMD_MARK_DISCONNECTED: return "MARK_DISCONNECTED";
    case MB_LIFECYCLE_CMD_SESSION_EVENT: return "SESSION_EVENT";
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
    if (!rt)
        return;

    g_mutex_lock(&rt->lifecycle_lock);
    if (!rt->lifecycle_source_id)
        rt->lifecycle_source_id = g_idle_add(lifecycle_process_next_cb, rt);
    g_mutex_unlock(&rt->lifecycle_lock);
}

static bool lifecycle_command_should_log(MbLifecycleCommandType type, const char *reason) {
    if (type == MB_LIFECYCLE_CMD_RECHECK && g_strcmp0(reason, "health") == 0)
        return false;

    return true;
}

static void lifecycle_enqueue(ConfigDirRuntime *rt,
                              ConfigDeviceRuntime *dev,
                              MbLifecycleCommandType type,
                              const char *reason) {
    if (!rt || !dev)
        return;

    g_mutex_lock(&rt->lifecycle_lock);

    if (!rt->lifecycle_queue) {
        g_mutex_unlock(&rt->lifecycle_lock);
        return;
    }

    if (lifecycle_has_pending(rt, dev, type)) {
        g_mutex_unlock(&rt->lifecycle_lock);
        return;
    }

    MbLifecycleCommand *cmd = g_new0(MbLifecycleCommand, 1);
    cmd->type = type;
    cmd->dev = dev;
    cmd->reason = g_strdup(reason);

    g_queue_push_tail(rt->lifecycle_queue, cmd);

    g_mutex_unlock(&rt->lifecycle_lock);

    if (lifecycle_command_should_log(type, reason)) {
        g_print("Lifecycle queued %s for %s%s%s.\n",
                lifecycle_command_name(type),
                device_label(dev),
                reason && *reason ? ": " : "",
                reason && *reason ? reason : "");
    }

    lifecycle_schedule(rt);
}

static void lifecycle_enqueue_event(ConfigDirRuntime *rt,
                                    ConfigDeviceRuntime *dev,
                                    MbEventType event,
                                    const char *reason) {
    if (!rt || !dev)
        return;

    g_mutex_lock(&rt->lifecycle_lock);

    if (!rt->lifecycle_queue) {
        g_mutex_unlock(&rt->lifecycle_lock);
        return;
    }

    MbLifecycleCommand *cmd = g_new0(MbLifecycleCommand, 1);
    cmd->type = MB_LIFECYCLE_CMD_SESSION_EVENT;
    cmd->dev = dev;
    cmd->event = event;
    cmd->reason = g_strdup(reason);

    g_queue_push_tail(rt->lifecycle_queue, cmd);

    g_mutex_unlock(&rt->lifecycle_lock);

    g_print("Lifecycle queued SESSION_EVENT/%s for %s%s%s.\n",
            mb_event_name(event),
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

    g_mutex_lock(&rt->lifecycle_lock);

    rt->lifecycle_source_id = 0;

    if (rt->lifecycle_busy) {
        g_mutex_unlock(&rt->lifecycle_lock);
        return G_SOURCE_REMOVE;
    }

    MbLifecycleCommand *cmd = rt->lifecycle_queue
        ? g_queue_pop_head(rt->lifecycle_queue)
        : NULL;

    if (!cmd) {
        g_mutex_unlock(&rt->lifecycle_lock);
        return G_SOURCE_REMOVE;
    }

    rt->lifecycle_busy = true;

    g_mutex_unlock(&rt->lifecycle_lock);

    if (lifecycle_command_should_log(cmd->type, cmd->reason)) {
        g_print("Lifecycle processing %s for %s%s%s.\n",
                lifecycle_command_name(cmd->type),
                device_label(cmd->dev),
                cmd->reason && *cmd->reason ? ": " : "",
                cmd->reason && *cmd->reason ? cmd->reason : "");
    }

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

    case MB_LIFECYCLE_CMD_MARK_DISCONNECTED:
        device_mark_disconnected(cmd->dev, cmd->reason);
        break;

    case MB_LIFECYCLE_CMD_SESSION_EVENT:
        device_session_event(cmd->dev, cmd->event);
        break;
    }

    runtime_dbus_emit_device_changed(cmd->dev);

    lifecycle_command_free(cmd);

    g_mutex_lock(&rt->lifecycle_lock);
    rt->lifecycle_busy = false;
    bool has_more = rt->lifecycle_queue && !g_queue_is_empty(rt->lifecycle_queue);
    g_mutex_unlock(&rt->lifecycle_lock);

    if (has_more)
        lifecycle_schedule(rt);

    return G_SOURCE_REMOVE;
}

static bool lifecycle_is_idle(ConfigDirRuntime *rt) {
    if (!rt)
        return true;

    g_mutex_lock(&rt->lifecycle_lock);
    bool idle = !rt->lifecycle_busy &&
                (!rt->lifecycle_queue || g_queue_is_empty(rt->lifecycle_queue));
    g_mutex_unlock(&rt->lifecycle_lock);

    return idle;
}

static bool lifecycle_has_queued(ConfigDirRuntime *rt) {
    if (!rt)
        return false;

    g_mutex_lock(&rt->lifecycle_lock);
    bool queued = rt->lifecycle_queue && !g_queue_is_empty(rt->lifecycle_queue);
    g_mutex_unlock(&rt->lifecycle_lock);

    return queued;
}

static char *runtime_control_socket_path(void) {
    const char *runtime_dir = g_get_user_runtime_dir();
    if (!runtime_dir || !*runtime_dir)
        runtime_dir = g_get_tmp_dir();

    return g_build_filename(runtime_dir, "midi-ble-rt", "control.sock", NULL);
}

static unsigned runtime_lifecycle_queue_depth(ConfigDirRuntime *rt) {
    if (!rt)
        return 0;

    g_mutex_lock(&rt->lifecycle_lock);
    unsigned depth = rt->lifecycle_queue ? (unsigned)g_queue_get_length(rt->lifecycle_queue) : 0;
    g_mutex_unlock(&rt->lifecycle_lock);

    return depth;
}

static void runtime_control_reply(int fd, const char *text) {
    if (!text)
        return;

    size_t len = strlen(text);
    while (len > 0) {
        ssize_t n = write(fd, text, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }

        text += n;
        len -= (size_t)n;
    }
}

static ConfigDeviceRuntime *runtime_find_device_by_id(ConfigDirRuntime *rt,
                                                      const char *id) {
    if (!rt || !id || !*id)
        return NULL;

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!dev || dev->removed || !dev->config)
            continue;

        if (g_strcmp0(dev->config->id, id) == 0 ||
            g_strcmp0(dev->config->address, id) == 0)
            return dev;
    }

    return NULL;
}

static bool runtime_control_enqueue_lifecycle(ConfigDirRuntime *rt,
                                              int client_fd,
                                              const char *id,
                                              MbLifecycleCommandType type,
                                              const char *reason) {
    if (!rt || !id || !*id)
        return false;

    ConfigDeviceRuntime *dev = runtime_find_device_by_id(rt, id);
    if (!dev) {
        runtime_control_reply(client_fd, "ERR device not found\n");
        return false;
    }

    lifecycle_enqueue(rt, dev, type, reason);

    char *reply = g_strdup_printf("OK QUEUED %s id=%s state=%s\n",
                                  lifecycle_command_name(type),
                                  printable_string(dev->config->id, "-"),
                                  mb_session_state_name(device_session_state(dev)));
    runtime_control_reply(client_fd, reply);
    g_free(reply);
    return true;
}


static const char runtime_dbus_introspection_xml[] =
    "<node>"
    "  <interface name='org.midi_ble_rt.Daemon1'>"
    "    <method name='ListDevices'>"
    "      <arg name='devices' type='aa{sv}' direction='out'/>"
    "    </method>"
    "    <method name='ScanDevices'>"
    "      <arg name='timeout_seconds' type='u' direction='in'/>"
    "      <arg name='devices' type='aa{sv}' direction='out'/>"
    "    </method>"
    "    <method name='PairAndImportDevice'>"
    "      <arg name='address' type='s' direction='in'/>"
    "      <arg name='device' type='a{sv}' direction='out'/>"
    "    </method>"
    "    <method name='ForgetDevice'>"
    "      <arg name='id' type='s' direction='in'/>"
    "      <arg name='remove_bluez' type='b' direction='in'/>"
    "    </method>"
    "    <method name='ConnectDevice'>"
    "      <arg name='id' type='s' direction='in'/>"
    "    </method>"
    "    <method name='DisconnectDevice'>"
    "      <arg name='id' type='s' direction='in'/>"
    "    </method>"
    "    <method name='RecheckDevice'>"
    "      <arg name='id' type='s' direction='in'/>"
    "    </method>"
    "    <signal name='DeviceChanged'>"
    "      <arg name='device' type='a{sv}'/>"
    "    </signal>"
    "    <signal name='DeviceStateChanged'>"
    "      <arg name='id' type='s'/>"
    "      <arg name='state' type='s'/>"
    "    </signal>"
    "    <signal name='DeviceRemoved'>"
    "      <arg name='id' type='s'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static void runtime_dbus_dict_add_string(GVariantBuilder *dict,
                                         const char *key,
                                         const char *value) {
    g_variant_builder_add(dict,
                          "{sv}",
                          key,
                          g_variant_new_string(printable_string(value, "")));
}

static void runtime_dbus_dict_add_bool(GVariantBuilder *dict,
                                       const char *key,
                                       bool value) {
    g_variant_builder_add(dict, "{sv}", key, g_variant_new_boolean(value));
}

static void runtime_dbus_dict_add_int(GVariantBuilder *dict,
                                      const char *key,
                                      int value) {
    g_variant_builder_add(dict, "{sv}", key, g_variant_new_int32(value));
}

static GVariant *runtime_dbus_device_to_variant(ConfigDeviceRuntime *dev) {
    GVariantBuilder dict;
    g_variant_builder_init(&dict, G_VARIANT_TYPE("a{sv}"));

    if (!dev || !dev->config) {
        return g_variant_builder_end(&dict);
    }

    MbSessionState state = device_session_state(dev);

    runtime_dbus_dict_add_string(&dict, "Id", dev->config->id);
    runtime_dbus_dict_add_string(&dict, "Address", dev->config->address);
    runtime_dbus_dict_add_string(&dict, "Name", dev->config->name);
    runtime_dbus_dict_add_string(&dict,
                                 "Profile",
                                 printable_string(dev->config->profile,
                                                  "standard_ble_midi"));
    runtime_dbus_dict_add_bool(&dict, "Enabled", dev->config->enabled);
    runtime_dbus_dict_add_bool(&dict, "ConnectOnStart", dev->config->connect_on_start);
    runtime_dbus_dict_add_bool(&dict,
                               "ReconnectOnLinkLoss",
                               dev->config->reconnect_on_link_loss);
    runtime_dbus_dict_add_bool(&dict, "EnableTx", dev->config->enable_tx);
    runtime_dbus_dict_add_string(&dict, "AlsaPortName", dev->config->alsa_port_name);
    runtime_dbus_dict_add_string(&dict, "RuntimeState", mb_session_state_name(state));
    runtime_dbus_dict_add_bool(&dict, "Streaming", state == MB_SESSION_STREAMING);
    runtime_dbus_dict_add_int(&dict, "AlsaPort", dev->alsa_port);

    return g_variant_builder_end(&dict);
}


#define RUNTIME_DBUS_ADAPTER_IFACE "org.bluez.Adapter1"
#define RUNTIME_DBUS_BLE_MIDI_SERVICE_UUID "03b80e5a-ede8-4b33-a751-6ce34ec4c700"

static char *runtime_dbus_bluez_dup_string(GVariant *dict,
                                           const char *key) {
    if (!dict || !key)
        return NULL;

    GVariant *value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
    if (!value)
        return NULL;

    char *out = g_strdup(g_variant_get_string(value, NULL));
    g_variant_unref(value);
    return out;
}

static bool runtime_dbus_bluez_get_bool(GVariant *dict,
                                        const char *key,
                                        bool fallback) {
    if (!dict || !key)
        return fallback;

    GVariant *value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_BOOLEAN);
    if (!value)
        return fallback;

    bool out = g_variant_get_boolean(value);
    g_variant_unref(value);
    return out;
}

static bool runtime_dbus_bluez_get_int16(GVariant *dict,
                                         const char *key,
                                         int *out) {
    if (!dict || !key || !out)
        return false;

    GVariant *value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_INT16);
    if (!value)
        return false;

    *out = (int)g_variant_get_int16(value);
    g_variant_unref(value);
    return true;
}

static bool runtime_dbus_contains_casefold(const char *haystack,
                                           const char *needle) {
    if (!haystack || !needle || !*needle)
        return false;

    char *h = g_ascii_strdown(haystack, -1);
    char *n = g_ascii_strdown(needle, -1);
    bool ok = h && n && strstr(h, n) != NULL;

    g_free(n);
    g_free(h);
    return ok;
}

static bool runtime_dbus_uuid_array_contains(GVariant *uuids,
                                             const char *uuid) {
    if (!uuids || !uuid || !*uuid)
        return false;

    GVariantIter iter;
    const char *candidate = NULL;

    g_variant_iter_init(&iter, uuids);
    while (g_variant_iter_next(&iter, "&s", &candidate)) {
        if (candidate && g_ascii_strcasecmp(candidate, uuid) == 0)
            return true;
    }

    return false;
}

static bool runtime_dbus_scan_has_midi_hint(const char *name,
                                            const char *alias,
                                            GVariant *uuids) {
    return runtime_dbus_uuid_array_contains(uuids, RUNTIME_DBUS_BLE_MIDI_SERVICE_UUID) ||
           runtime_dbus_contains_casefold(name, "midi") ||
           runtime_dbus_contains_casefold(alias, "midi") ||
           runtime_dbus_contains_casefold(name, "go:keys") ||
           runtime_dbus_contains_casefold(alias, "go:keys") ||
           runtime_dbus_contains_casefold(name, "go keys") ||
           runtime_dbus_contains_casefold(alias, "go keys");
}

static const char *runtime_dbus_scan_guess_profile(const char *name,
                                                   const char *alias,
                                                   GVariant *uuids) {
    if (runtime_dbus_contains_casefold(name, "go:keys") ||
        runtime_dbus_contains_casefold(alias, "go:keys") ||
        runtime_dbus_contains_casefold(name, "go keys") ||
        runtime_dbus_contains_casefold(alias, "go keys"))
        return "roland_gokeys";

    if (runtime_dbus_uuid_array_contains(uuids, RUNTIME_DBUS_BLE_MIDI_SERVICE_UUID))
        return "standard_ble_midi";

    if (runtime_dbus_contains_casefold(name, "midi") ||
        runtime_dbus_contains_casefold(alias, "midi"))
        return "unknown_midi";

    return "-";
}

static char *runtime_dbus_scan_id_from_name_address(const char *name,
                                                    const char *address) {
    const char *base = name && *name ? name : address;
    if (!base || !*base)
        base = "ble-midi-device";

    char *tmp = g_ascii_strdown(base, -1);

    for (char *p = tmp; p && *p; p++) {
        if (!g_ascii_isalnum(*p))
            *p = '-';
    }

    char **parts = g_strsplit(tmp, "-", -1);
    GString *out = g_string_new(NULL);

    for (guint i = 0; parts && parts[i]; i++) {
        if (!*parts[i])
            continue;
        if (out->len > 0)
            g_string_append_c(out, '-');
        g_string_append(out, parts[i]);
    }

    if (out->len == 0)
        g_string_append(out, "ble-midi-device");

    g_strfreev(parts);
    g_free(tmp);

    return g_string_free(out, FALSE);
}

static bool runtime_dbus_address_is_imported(ConfigDirRuntime *rt,
                                             const char *address) {
    if (!rt || !address || !*address)
        return false;

    for (guint i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);

        if (!dev || !dev->config || !dev->config->address)
            continue;

        if (g_ascii_strcasecmp(dev->config->address, address) == 0)
            return true;
    }

    return false;
}

static char *runtime_dbus_first_bluez_adapter_path(ConfigDirRuntime *rt) {
    if (!rt || !rt->bus)
        return NULL;

    GVariant *objects = mb_bluez_get_managed_objects(rt->bus);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    char *adapter_path = NULL;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces,
                                                  RUNTIME_DBUS_ADAPTER_IFACE,
                                                  G_VARIANT_TYPE("a{sv}"));
        if (props) {
            adapter_path = g_strdup(path);
            g_variant_unref(props);
        }

        g_variant_unref(ifaces);

        if (adapter_path)
            break;
    }

    g_variant_unref(objects);
    return adapter_path;
}

static bool runtime_dbus_call_bluez_adapter_void(ConfigDirRuntime *rt,
                                                 const char *adapter_path,
                                                 const char *method,
                                                 GError **error) {
    if (!rt || !rt->bus || !adapter_path || !*adapter_path || !method || !*method) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid BlueZ adapter call");
        return false;
    }

    GVariant *reply = g_dbus_connection_call_sync(rt->bus,
                                                  BLUEZ_BUS,
                                                  adapter_path,
                                                  RUNTIME_DBUS_ADAPTER_IFACE,
                                                  method,
                                                  NULL,
                                                  NULL,
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  MB_DBUS_GET_MANAGED_OBJECTS_TIMEOUT_MS,
                                                  NULL,
                                                  error);
    if (!reply)
        return false;

    g_variant_unref(reply);
    return true;
}

static GVariant *runtime_dbus_scan_candidate_to_variant(ConfigDirRuntime *rt,
                                                        const char *path,
                                                        GVariant *props) {
    char *address = runtime_dbus_bluez_dup_string(props, "Address");
    char *name = runtime_dbus_bluez_dup_string(props, "Name");
    char *alias = runtime_dbus_bluez_dup_string(props, "Alias");
    GVariant *uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    char *id = runtime_dbus_scan_id_from_name_address(name && *name ? name : alias,
                                                      address);
    const char *profile = runtime_dbus_scan_guess_profile(name, alias, uuids);
    bool imported = runtime_dbus_address_is_imported(rt, address);

    bool paired = runtime_dbus_bluez_get_bool(props, "Paired", false);
    bool trusted = runtime_dbus_bluez_get_bool(props, "Trusted", false);
    bool connected = runtime_dbus_bluez_get_bool(props, "Connected", false);

    int rssi = 0;
    bool rssi_known = runtime_dbus_bluez_get_int16(props, "RSSI", &rssi);

    GVariantBuilder dict;
    g_variant_builder_init(&dict, G_VARIANT_TYPE("a{sv}"));

    runtime_dbus_dict_add_string(&dict, "Id", id);
    runtime_dbus_dict_add_string(&dict, "Address", address);
    runtime_dbus_dict_add_string(&dict, "Name", name && *name ? name : alias);
    runtime_dbus_dict_add_string(&dict, "Alias", alias);
    runtime_dbus_dict_add_string(&dict, "BluezPath", path);
    runtime_dbus_dict_add_string(&dict, "Profile", profile);
    runtime_dbus_dict_add_bool(&dict, "Imported", imported);
    runtime_dbus_dict_add_bool(&dict, "Paired", paired);
    runtime_dbus_dict_add_bool(&dict, "Trusted", trusted);
    runtime_dbus_dict_add_bool(&dict, "Connected", connected);
    runtime_dbus_dict_add_bool(&dict, "RSSIKnown", rssi_known);
    runtime_dbus_dict_add_int(&dict, "RSSI", rssi);
    runtime_dbus_dict_add_string(&dict,
                                 "RuntimeState",
                                 imported ? "IMPORTED" : "DISCOVERED");

    if (uuids)
        g_variant_unref(uuids);
    g_free(id);
    g_free(alias);
    g_free(name);
    g_free(address);

    return g_variant_builder_end(&dict);
}

static GVariant *runtime_dbus_scan_devices(ConfigDirRuntime *rt,
                                           guint timeout_seconds,
                                           GError **error) {
    if (!rt || !rt->bus) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "BlueZ system bus is unavailable");
        return NULL;
    }

    if (timeout_seconds < 1)
        timeout_seconds = 1;
    if (timeout_seconds > 30)
        timeout_seconds = 30;

    char *adapter_path = runtime_dbus_first_bluez_adapter_path(rt);
    if (!adapter_path) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "no BlueZ adapter found");
        return NULL;
    }

    g_print("D-Bus ScanDevices: StartDiscovery adapter=%s timeout=%u\n",
            adapter_path,
            timeout_seconds);

    if (!runtime_dbus_call_bluez_adapter_void(rt,
                                              adapter_path,
                                              "StartDiscovery",
                                              error)) {
        g_free(adapter_path);
        return NULL;
    }

    g_usleep((gulong)timeout_seconds * G_USEC_PER_SEC);

    GError *stop_error = NULL;
    if (!runtime_dbus_call_bluez_adapter_void(rt,
                                              adapter_path,
                                              "StopDiscovery",
                                              &stop_error)) {
        g_printerr("D-Bus ScanDevices: StopDiscovery failed: %s\n",
                   stop_error && stop_error->message ? stop_error->message : "unknown error");
        g_clear_error(&stop_error);
    }

    g_free(adapter_path);

    GVariant *objects = mb_bluez_get_managed_objects(rt->bus);
    if (!objects) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "BlueZ GetManagedObjects failed after discovery");
        return NULL;
    }

    GVariantBuilder devices;
    g_variant_builder_init(&devices, G_VARIANT_TYPE("aa{sv}"));

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces,
                                                  DEVICE_IFACE,
                                                  G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        char *address = runtime_dbus_bluez_dup_string(props, "Address");
        char *name = runtime_dbus_bluez_dup_string(props, "Name");
        char *alias = runtime_dbus_bluez_dup_string(props, "Alias");
        GVariant *uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

        bool keep = address && *address &&
                    runtime_dbus_scan_has_midi_hint(name, alias, uuids);

        if (keep) {
            g_variant_builder_add_value(&devices,
                                        runtime_dbus_scan_candidate_to_variant(rt,
                                                                              path,
                                                                              props));
        }

        if (uuids)
            g_variant_unref(uuids);
        g_free(alias);
        g_free(name);
        g_free(address);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    return g_variant_builder_end(&devices);
}



static GVariant *runtime_dbus_get_bluez_device_props(ConfigDirRuntime *rt,
                                                     const char *device_path,
                                                     GError **error) {
    if (!rt || !rt->bus || !device_path || !*device_path) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid BlueZ device path");
        return NULL;
    }

    GVariant *reply = g_dbus_connection_call_sync(rt->bus,
                                                  BLUEZ_BUS,
                                                  device_path,
                                                  PROPERTIES_IFACE,
                                                  "GetAll",
                                                  g_variant_new("(s)", DEVICE_IFACE),
                                                  G_VARIANT_TYPE("(a{sv})"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  MB_DBUS_DEVICE_PROPERTY_GET_TIMEOUT_MS,
                                                  NULL,
                                                  error);
    if (!reply)
        return NULL;

    GVariant *props = g_variant_get_child_value(reply, 0);
    g_variant_unref(reply);
    return props;
}

static const char *runtime_dbus_import_config_profile(const char *profile) {
    /*
     * devices.d currently supports concrete daemon profiles.
     * A fuzzy discovery hint like unknown_midi must not be persisted as a
     * runtime profile.
     */
    if (profile && g_strcmp0(profile, "roland_gokeys") == 0)
        return "roland_gokeys";

    return "standard_ble_midi";
}

static char *runtime_dbus_unique_import_id(ConfigDirRuntime *rt,
                                           const char *base_id,
                                           const char *devices_dir) {
    const char *base = base_id && *base_id ? base_id : "ble-midi-device";

    for (unsigned i = 0; i < 1000; i++) {
        char *candidate = i == 0
            ? g_strdup(base)
            : g_strdup_printf("%s-%u", base, i + 1);

        char *filename = g_strdup_printf("%s.ini", candidate);
        char *path = g_build_filename(devices_dir, filename, NULL);

        bool exists = g_file_test(path, G_FILE_TEST_EXISTS) ||
                      runtime_find_device_by_id(rt, candidate) != NULL;

        g_free(path);
        g_free(filename);

        if (!exists)
            return candidate;

        g_free(candidate);
    }

    return g_strdup_printf("%s-%" G_GINT64_FORMAT,
                           base,
                           g_get_real_time());
}

static char *runtime_dbus_sanitize_port_name(const char *name,
                                             const char *id) {
    if (name && *name)
        return g_strdup(name);

    if (id && *id)
        return g_strdup(id);

    return g_strdup("BLE-MIDI");
}

static bool runtime_dbus_write_import_config(ConfigDirRuntime *rt,
                                             const char *address,
                                             const char *name,
                                             const char *profile,
                                             char **out_id,
                                             char **out_path,
                                             GError **error) {
    if (!rt || !rt->config_dir || !*rt->config_dir) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "daemon config directory is unavailable");
        return false;
    }

    if (!address || !*address) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "device address is required");
        return false;
    }

    char *devices_dir = g_build_filename(rt->config_dir, "devices.d", NULL);
    if (g_mkdir_with_parents(devices_dir, 0700) != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "could not create devices directory %s: %s",
                    devices_dir,
                    g_strerror(errno));
        g_free(devices_dir);
        return false;
    }

    char *base_id = runtime_dbus_scan_id_from_name_address(name, address);
    char *id = runtime_dbus_unique_import_id(rt, base_id, devices_dir);
    char *filename = g_strdup_printf("%s.ini", id);
    char *path = g_build_filename(devices_dir, filename, NULL);
    char *alsa_port_name = runtime_dbus_sanitize_port_name(name, id);

    const char *persisted_profile = runtime_dbus_import_config_profile(profile);

    char *content = g_strdup_printf(
        "[device]\n"
        "id = %s\n"
        "address = %s\n"
        "name = %s\n"
        "profile = %s\n"
        "alsa_port_name = %s\n"
        "enabled = true\n"
        "connect_on_start = true\n"
        "\n"
        "[policy]\n"
        "pair = false\n"
        "trust = true\n"
        "reconnect_on_link_loss = true\n"
        "\n"
        "[midi]\n"
        "enable_tx = true\n",
        id,
        address,
        name && *name ? name : id,
        persisted_profile,
        alsa_port_name);

    bool ok = g_file_set_contents(path, content, -1, error);

    if (ok) {
        if (out_id)
            *out_id = g_strdup(id);
        if (out_path)
            *out_path = g_strdup(path);
    }

    g_free(content);
    g_free(alsa_port_name);
    g_free(path);
    g_free(filename);
    g_free(id);
    g_free(base_id);
    g_free(devices_dir);

    return ok;
}

static GVariant *runtime_dbus_pair_import_result_to_variant(const char *id,
                                                            const char *address,
                                                            const char *name,
                                                            const char *device_path,
                                                            const char *profile,
                                                            bool paired,
                                                            bool trusted,
                                                            bool connected,
                                                            const char *config_path,
                                                            const char *import_status) {
    GVariantBuilder dict;
    g_variant_builder_init(&dict, G_VARIANT_TYPE("a{sv}"));

    runtime_dbus_dict_add_string(&dict, "Id", id);
    runtime_dbus_dict_add_string(&dict, "Address", address);
    runtime_dbus_dict_add_string(&dict, "Name", name);
    runtime_dbus_dict_add_string(&dict, "BluezPath", device_path);
    runtime_dbus_dict_add_string(&dict, "Profile", runtime_dbus_import_config_profile(profile));
    runtime_dbus_dict_add_bool(&dict, "Imported", true);
    runtime_dbus_dict_add_bool(&dict, "Paired", paired);
    runtime_dbus_dict_add_bool(&dict, "Trusted", trusted);
    runtime_dbus_dict_add_bool(&dict, "Connected", connected);
    runtime_dbus_dict_add_string(&dict, "ConfigPath", config_path);
    runtime_dbus_dict_add_string(&dict, "ImportStatus", import_status);
    runtime_dbus_dict_add_string(&dict, "RuntimeState", "IMPORTED");

    return g_variant_builder_end(&dict);
}

static GVariant *runtime_dbus_pair_and_import_device(ConfigDirRuntime *rt,
                                                     const char *address,
                                                     GError **error) {
    if (!rt || !rt->bus) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "BlueZ system bus is unavailable");
        return NULL;
    }

    if (!address || !*address) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "device address is required");
        return NULL;
    }

    char *device_path = mb_bluez_find_device_path_by_address(rt->bus, address);
    if (!device_path) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "BlueZ device not found for address: %s",
                    address);
        return NULL;
    }

    GVariant *props = runtime_dbus_get_bluez_device_props(rt, device_path, error);
    if (!props) {
        g_free(device_path);
        return NULL;
    }

    char *name = runtime_dbus_bluez_dup_string(props, "Name");
    char *alias = runtime_dbus_bluez_dup_string(props, "Alias");
    GVariant *uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    const char *visible_name = name && *name ? name : alias;
    const char *guessed_profile = runtime_dbus_scan_guess_profile(name, alias, uuids);
    const char *profile = runtime_dbus_import_config_profile(guessed_profile);

    bool paired = runtime_dbus_bluez_get_bool(props, "Paired", false);
    bool trusted = runtime_dbus_bluez_get_bool(props, "Trusted", false);
    bool connected = runtime_dbus_bluez_get_bool(props, "Connected", false);

    if (runtime_dbus_address_is_imported(rt, address)) {
        char *id = runtime_dbus_scan_id_from_name_address(visible_name, address);

        GVariant *result =
            runtime_dbus_pair_import_result_to_variant(id,
                                                       address,
                                                       visible_name,
                                                       device_path,
                                                       profile,
                                                       paired,
                                                       trusted,
                                                       connected,
                                                       "",
                                                       "already_imported");

        if (uuids)
            g_variant_unref(uuids);
        g_free(id);
        g_free(alias);
        g_free(name);
        g_variant_unref(props);
        g_free(device_path);
        return result;
    }

    if (!paired) {
        g_print("D-Bus PairAndImportDevice: pairing %s\n", address);
        if (!mb_bluez_pair_device(rt->bus, device_path)) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "BlueZ Pair() failed for %s",
                        address);
            if (uuids)
                g_variant_unref(uuids);
            g_free(alias);
            g_free(name);
            g_variant_unref(props);
            g_free(device_path);
            return NULL;
        }
        paired = true;
    }

    if (!trusted) {
        g_print("D-Bus PairAndImportDevice: setting Trusted=true for %s\n", address);
        if (!mb_bluez_set_device_trusted(rt->bus, device_path)) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "BlueZ Trusted=true failed for %s",
                        address);
            if (uuids)
                g_variant_unref(uuids);
            g_free(alias);
            g_free(name);
            g_variant_unref(props);
            g_free(device_path);
            return NULL;
        }
        trusted = true;
    }

    char *import_id = NULL;
    char *config_path = NULL;
    if (!runtime_dbus_write_import_config(rt,
                                          address,
                                          visible_name,
                                          profile,
                                          &import_id,
                                          &config_path,
                                          error)) {
        if (uuids)
            g_variant_unref(uuids);
        g_free(alias);
        g_free(name);
        g_variant_unref(props);
        g_free(device_path);
        return NULL;
    }

    g_print("D-Bus PairAndImportDevice: imported %s as %s (%s)\n",
            address,
            import_id,
            config_path);

    GVariant *result =
        runtime_dbus_pair_import_result_to_variant(import_id,
                                                   address,
                                                   visible_name,
                                                   device_path,
                                                   profile,
                                                   paired,
                                                   trusted,
                                                   connected,
                                                   config_path,
                                                   "imported_restart_required");

    /*
     * Full dynamic runtime insertion will come in a later patch.  For now the
     * daemon tells clients that the config exists but this running process has
     * not created the ALSA/session runtime for it yet.
     */
    g_dbus_connection_emit_signal(rt->dbus_bus,
                                  NULL,
                                  "/org/midi_ble_rt/Daemon",
                                  "org.midi_ble_rt.Daemon1",
                                  "DeviceChanged",
                                  g_variant_new("(@a{sv})", g_variant_ref(result)),
                                  NULL);

    if (uuids)
        g_variant_unref(uuids);
    g_free(config_path);
    g_free(import_id);
    g_free(alias);
    g_free(name);
    g_variant_unref(props);
    g_free(device_path);

    return result;
}


static void runtime_dbus_emit_device_changed(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner || !dev->owner->dbus_bus || !dev->config)
        return;

    ConfigDirRuntime *rt = dev->owner;
    MbSessionState state = device_session_state(dev);

    if (dev->dbus_last_state_valid && dev->dbus_last_state == state)
        return;

    dev->dbus_last_state_valid = true;
    dev->dbus_last_state = state;

    const char *id = printable_string(dev->config->id, "");
    const char *state_name = mb_session_state_name(state);

    g_dbus_connection_emit_signal(rt->dbus_bus,
                                  NULL,
                                  "/org/midi_ble_rt/Daemon",
                                  "org.midi_ble_rt.Daemon1",
                                  "DeviceChanged",
                                  g_variant_new("(@a{sv})",
                                                runtime_dbus_device_to_variant(dev)),
                                  NULL);

    g_dbus_connection_emit_signal(rt->dbus_bus,
                                  NULL,
                                  "/org/midi_ble_rt/Daemon",
                                  "org.midi_ble_rt.Daemon1",
                                  "DeviceStateChanged",
                                  g_variant_new("(ss)", id, state_name),
                                  NULL);
}


static void runtime_dbus_return_device_not_found(GDBusMethodInvocation *invocation,
                                                 const char *id) {
    g_dbus_method_invocation_return_error(invocation,
                                          G_IO_ERROR,
                                          G_IO_ERROR_NOT_FOUND,
                                          "device not found: %s",
                                          id && *id ? id : "-");
}

static bool runtime_dbus_enqueue_device_command(ConfigDirRuntime *rt,
                                                GDBusMethodInvocation *invocation,
                                                const char *id,
                                                MbLifecycleCommandType type,
                                                const char *reason) {
    ConfigDeviceRuntime *dev = runtime_find_device_by_id(rt, id);

    if (!dev) {
        runtime_dbus_return_device_not_found(invocation, id);
        return false;
    }

    lifecycle_enqueue(rt, dev, type, reason);
    g_dbus_method_invocation_return_value(invocation, NULL);
    return true;
}


static void runtime_dbus_emit_device_removed(ConfigDirRuntime *rt,
                                             const char *id) {
    if (!rt || !rt->dbus_bus)
        return;

    g_dbus_connection_emit_signal(rt->dbus_bus,
                                  NULL,
                                  "/org/midi_ble_rt/Daemon",
                                  "org.midi_ble_rt.Daemon1",
                                  "DeviceRemoved",
                                  g_variant_new("(s)", printable_string(id, "")),
                                  NULL);
}

static bool runtime_dbus_config_file_matches_device(const char *path,
                                                    const char *id,
                                                    const char *address) {
    if (!path || !*path)
        return false;

    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_key_file_unref(kf);
        return false;
    }

    char *file_id = g_key_file_get_string(kf, "device", "id", NULL);
    char *file_address = g_key_file_get_string(kf, "device", "address", NULL);

    bool match = false;

    if (id && *id) {
        if (file_id && g_strcmp0(file_id, id) == 0)
            match = true;
        if (file_address && g_ascii_strcasecmp(file_address, id) == 0)
            match = true;
    }

    if (!match && address && *address && file_address)
        match = g_ascii_strcasecmp(file_address, address) == 0;

    g_free(file_address);
    g_free(file_id);
    g_key_file_unref(kf);
    return match;
}

static char *runtime_dbus_find_device_config_path(ConfigDirRuntime *rt,
                                                  const char *id,
                                                  const char *address) {
    if (!rt || !rt->config_dir || !*rt->config_dir)
        return NULL;

    char *devices_dir = g_build_filename(rt->config_dir, "devices.d", NULL);
    GError *error = NULL;
    GDir *dir = g_dir_open(devices_dir, 0, &error);

    if (!dir) {
        g_clear_error(&error);
        g_free(devices_dir);
        return NULL;
    }

    const char *name = NULL;
    char *match = NULL;

    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".ini"))
            continue;

        char *path = g_build_filename(devices_dir, name, NULL);

        if (runtime_dbus_config_file_matches_device(path, id, address)) {
            match = path;
            break;
        }

        g_free(path);
    }

    g_dir_close(dir);
    g_free(devices_dir);
    return match;
}

static bool runtime_dbus_bluez_remove_device(ConfigDirRuntime *rt,
                                             const char *device_path,
                                             GError **error) {
    if (!rt || !rt->bus || !device_path || !*device_path) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid BlueZ RemoveDevice request");
        return false;
    }

    char *adapter_path = g_path_get_dirname(device_path);

    GVariant *reply = g_dbus_connection_call_sync(rt->bus,
                                                  BLUEZ_BUS,
                                                  adapter_path,
                                                  RUNTIME_DBUS_ADAPTER_IFACE,
                                                  "RemoveDevice",
                                                  g_variant_new("(o)", device_path),
                                                  NULL,
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  MB_DBUS_DEVICE_PROPERTY_GET_TIMEOUT_MS,
                                                  NULL,
                                                  error);

    g_free(adapter_path);

    if (!reply)
        return false;

    g_variant_unref(reply);
    return true;
}


static void runtime_lifecycle_drop_device_commands(ConfigDirRuntime *rt,
                                                   ConfigDeviceRuntime *dev) {
    if (!rt || !dev || !rt->lifecycle_queue)
        return;

    GQueue kept = G_QUEUE_INIT;

    g_mutex_lock(&rt->lifecycle_lock);

    while (!g_queue_is_empty(rt->lifecycle_queue)) {
        MbLifecycleCommand *cmd = g_queue_pop_head(rt->lifecycle_queue);

        if (cmd && cmd->dev == dev) {
            lifecycle_command_free(cmd);
            continue;
        }

        g_queue_push_tail(&kept, cmd);
    }

    while (!g_queue_is_empty(&kept))
        g_queue_push_tail(rt->lifecycle_queue, g_queue_pop_head(&kept));

    g_mutex_unlock(&rt->lifecycle_lock);
}

static void runtime_device_remove_alsa_port(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner)
        return;

    ConfigDirRuntime *rt = dev->owner;

    if (dev->alsa_port < 0)
        return;

    if (rt->device_by_alsa_port)
        g_hash_table_remove(rt->device_by_alsa_port,
                            GINT_TO_POINTER(dev->alsa_port));

    g_mutex_lock(&rt->alsa_lock);

    if (dev->session)
        mb_session_set_alsa_port(dev->session, -1);

    if (rt->seq) {
        int r = snd_seq_delete_simple_port(rt->seq, dev->alsa_port);
        if (r < 0) {
            g_printerr("Could not delete ALSA port for %s: %s\n",
                       device_label(dev),
                       snd_strerror(r));
        } else {
            g_print("ALSA port removed for %s: %d\n",
                    device_label(dev),
                    dev->alsa_port);
        }
    }

    g_mutex_unlock(&rt->alsa_lock);

    dev->alsa_port = -1;
}

static void runtime_device_disconnect_best_effort(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner || !dev->owner->bus)
        return;

    char *device_path = device_dup_device_path(dev);

    if (device_path &&
        *device_path &&
        !g_str_has_prefix(device_path, "config:")) {
        if (!mb_bluez_disconnect_device(dev->owner->bus, device_path)) {
            g_printerr("ForgetDevice: best-effort disconnect failed for %s\n",
                       device_label(dev));
        }
    }

    g_free(device_path);
}

static bool runtime_remove_device_from_memory(ConfigDirRuntime *rt,
                                              ConfigDeviceRuntime *dev,
                                              GError **error) {
    if (!rt || !dev) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid runtime device removal");
        return false;
    }

    guint dev_index = 0;
    bool found_dev = false;

    for (guint i = 0; rt->devices && i < rt->devices->len; i++) {
        if (g_ptr_array_index(rt->devices, i) == dev) {
            dev_index = i;
            found_dev = true;
            break;
        }
    }

    if (!found_dev) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "runtime device not found in device array");
        return false;
    }

    const MbDeviceConfig *config = dev->config;
    guint cfg_index = 0;
    bool found_cfg = false;

    for (guint i = 0; rt->cfg.devices && i < rt->cfg.devices->len; i++) {
        if (g_ptr_array_index(rt->cfg.devices, i) == config) {
            cfg_index = i;
            found_cfg = true;
            break;
        }
    }

    /*
     * Stop new work for this device before freeing the object.
     */
    runtime_lifecycle_drop_device_commands(rt, dev);

    /*
     * Prevent future RX/TX callbacks from accepting data.  The normal session
     * transition path also drops pending flow items; do the same here because
     * removal is administrative, not a radio state transition.
     */
    g_mutex_lock(&dev->dataplane_lock);
    dev->dataplane_streaming = false;
    dev->dataplane_closed_epoch = dev->dataplane_epoch;
    dev->dataplane_closed_at_ns = runtime_now_ns();
    g_mutex_unlock(&dev->dataplane_lock);

    if (dev->runtime_started)
        mb_duplex_runtime_drop_pending(&dev->runtime);

    device_stop_notify(dev);
    runtime_remove_char_route(dev);
    runtime_device_disconnect_best_effort(dev);
    runtime_device_remove_alsa_port(dev);

    /*
     * g_ptr_array_remove_index() calls device_runtime_free() for rt->devices.
     * That frees runtime-owned resources but not dev->config.
     */
    g_ptr_array_remove_index(rt->devices, dev_index);

    /*
     * Now it is safe to remove the config object.  rt->cfg.devices owns it.
     */
    if (found_cfg)
        g_ptr_array_remove_index(rt->cfg.devices, cfg_index);

    runtime_stats_export_snapshot(rt);
    return true;
}



static void runtime_dbus_deactivate_forgotten_device(ConfigDeviceRuntime *dev) {
    if (!dev || !dev->owner)
        return;

    ConfigDirRuntime *rt = dev->owner;

    /*
     * Administrative removal:
     * keep the ConfigDeviceRuntime object allocated, but stop exposing it to
     * ALSA/GATT routing.  This avoids use-after-free risks from pending GLib,
     * BlueZ, ALSA or lifecycle callbacks.
     */
    device_stop_notify(dev);
    runtime_remove_char_route(dev);

    if (dev->runtime_started)
        mb_duplex_runtime_drop_pending(&dev->runtime);

    g_mutex_lock(&dev->dataplane_lock);
    dev->dataplane_streaming = false;
    dev->dataplane_closed_epoch = dev->dataplane_epoch;
    dev->dataplane_closed_at_ns = runtime_now_ns();
    g_mutex_unlock(&dev->dataplane_lock);

    if (dev->alsa_port >= 0) {
        int old_port = dev->alsa_port;

        if (rt->device_by_alsa_port)
            g_hash_table_remove(rt->device_by_alsa_port,
                                GINT_TO_POINTER(dev->alsa_port));

        g_mutex_lock(&rt->alsa_lock);

        if (dev->session)
            mb_session_set_alsa_port(dev->session, -1);

        if (rt->seq) {
            int r = snd_seq_delete_simple_port(rt->seq, dev->alsa_port);
            if (r < 0) {
                g_printerr("ForgetDevice: could not delete ALSA port for %s: %s\n",
                           device_label(dev),
                           snd_strerror(r));
            } else {
                g_print("ForgetDevice: ALSA port removed for %s: %d\n",
                        device_label(dev),
                        old_port);
            }
        }

        dev->alsa_port = -1;

        g_mutex_unlock(&rt->alsa_lock);
    }

    runtime_stats_export_snapshot(rt);
}


static bool runtime_dbus_forget_device(ConfigDirRuntime *rt,
                                       const char *id,
                                       bool remove_bluez,
                                       GError **error) {
    if (!rt || !id || !*id) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "device id or address is required");
        return false;
    }

    ConfigDeviceRuntime *dev = runtime_find_device_by_id(rt, id);

    char *resolved_id = dev && dev->config
        ? g_strdup(printable_string(dev->config->id, id))
        : g_strdup(id);

    char *resolved_address = dev && dev->config && dev->config->address
        ? g_strdup(dev->config->address)
        : NULL;

    /*
     * Remove the persistent config first.  If unlink fails, keep runtime state
     * intact to avoid an in-memory/ondisk split-brain.
     */
    char *config_path = runtime_dbus_find_device_config_path(rt,
                                                            id,
                                                            resolved_address);
    if (config_path) {
        if (g_unlink(config_path) != 0) {
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "could not remove config %s: %s",
                        config_path,
                        g_strerror(errno));
            g_free(config_path);
            g_free(resolved_address);
            g_free(resolved_id);
            return false;
        }

        g_print("D-Bus ForgetDevice: removed config %s\n", config_path);
        g_free(config_path);
    } else {
        g_print("D-Bus ForgetDevice: no config file found for %s\n", id);
    }

    if (dev) {
        GError *remove_error = NULL;

        if (!runtime_remove_device_from_memory(rt, dev, &remove_error)) {
            g_printerr("D-Bus ForgetDevice: runtime removal failed for %s: %s\n",
                       id,
                       remove_error && remove_error->message
                           ? remove_error->message
                           : "unknown error");
            g_clear_error(&remove_error);
        } else {
            g_print("D-Bus ForgetDevice: removed runtime device %s\n",
                    printable_string(resolved_id, id));
        }
    }

    if (remove_bluez) {
        char *device_path = NULL;

        if (resolved_address && *resolved_address)
            device_path = mb_bluez_find_device_path_by_address(rt->bus,
                                                               resolved_address);
        if (!device_path && id && *id)
            device_path = mb_bluez_find_device_path_by_address(rt->bus, id);

        if (device_path) {
            GError *remove_error = NULL;

            if (!runtime_dbus_bluez_remove_device(rt, device_path, &remove_error)) {
                g_printerr("D-Bus ForgetDevice: BlueZ RemoveDevice failed for %s: %s\n",
                           id,
                           remove_error && remove_error->message
                               ? remove_error->message
                               : "unknown error");
                g_clear_error(&remove_error);
            } else {
                g_print("D-Bus ForgetDevice: removed BlueZ device %s\n", device_path);
            }

            g_free(device_path);
        }
    }

    if (dev && !dev->removed) {
        dev->removed = true;
        lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_DISCONNECT, "dbus forget");
        runtime_dbus_deactivate_forgotten_device(dev);
        runtime_dbus_emit_device_changed(dev);
    }

    runtime_dbus_emit_device_removed(rt, resolved_id);

    g_free(resolved_address);
    g_free(resolved_id);
    return true;
}




static void runtime_dbus_handle_method_call(GDBusConnection *connection,
                                            const char *sender,
                                            const char *object_path,
                                            const char *interface_name,
                                            const char *method_name,
                                            GVariant *parameters,
                                            GDBusMethodInvocation *invocation,
                                            gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;

    ConfigDirRuntime *rt = user_data;

    if (!rt) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_IO_ERROR,
                                              G_IO_ERROR_FAILED,
                                              "daemon runtime is unavailable");
        return;
    }

    if (g_strcmp0(method_name, "ListDevices") == 0) {
        GVariantBuilder devices;
        g_variant_builder_init(&devices, G_VARIANT_TYPE("aa{sv}"));

        for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
            ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
            if (!dev || dev->removed)
                continue;

            g_variant_builder_add_value(&devices,
                                        runtime_dbus_device_to_variant(dev));
        }

        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(@aa{sv})",
                                                            g_variant_builder_end(&devices)));
        return;
    }

    if (g_strcmp0(method_name, "ScanDevices") == 0) {
        guint timeout_seconds = 0;
        g_variant_get(parameters, "(u)", &timeout_seconds);

        GError *error = NULL;
        GVariant *devices = runtime_dbus_scan_devices(rt, timeout_seconds, &error);
        if (!devices) {
            g_dbus_method_invocation_return_error(invocation,
                                                  error ? error->domain : G_IO_ERROR,
                                                  error ? error->code : G_IO_ERROR_FAILED,
                                                  "%s",
                                                  error && error->message
                                                      ? error->message
                                                      : "ScanDevices failed");
            g_clear_error(&error);
            return;
        }

        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(@aa{sv})", devices));
        return;
    }

    if (g_strcmp0(method_name, "PairAndImportDevice") == 0) {
        const char *address = NULL;
        g_variant_get(parameters, "(&s)", &address);

        GError *error = NULL;
        GVariant *device = runtime_dbus_pair_and_import_device(rt, address, &error);
        if (!device) {
            g_dbus_method_invocation_return_error(invocation,
                                                  error ? error->domain : G_IO_ERROR,
                                                  error ? error->code : G_IO_ERROR_FAILED,
                                                  "%s",
                                                  error && error->message
                                                      ? error->message
                                                      : "PairAndImportDevice failed");
            g_clear_error(&error);
            return;
        }

        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(@a{sv})", device));
        return;
    }

    if (g_strcmp0(method_name, "ForgetDevice") == 0) {
        const char *id = NULL;
        gboolean remove_bluez = FALSE;
        g_variant_get(parameters, "(&sb)", &id, &remove_bluez);

        GError *error = NULL;
        if (!runtime_dbus_forget_device(rt, id, remove_bluez, &error)) {
            g_dbus_method_invocation_return_error(invocation,
                                                  error ? error->domain : G_IO_ERROR,
                                                  error ? error->code : G_IO_ERROR_FAILED,
                                                  "%s",
                                                  error && error->message
                                                      ? error->message
                                                      : "ForgetDevice failed");
            g_clear_error(&error);
            return;
        }

        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "ConnectDevice") == 0) {
        const char *id = NULL;
        g_variant_get(parameters, "(&s)", &id);
        runtime_dbus_enqueue_device_command(rt,
                                            invocation,
                                            id,
                                            MB_LIFECYCLE_CMD_CONNECT,
                                            "dbus connect");
        return;
    }

    if (g_strcmp0(method_name, "DisconnectDevice") == 0) {
        const char *id = NULL;
        g_variant_get(parameters, "(&s)", &id);
        runtime_dbus_enqueue_device_command(rt,
                                            invocation,
                                            id,
                                            MB_LIFECYCLE_CMD_DISCONNECT,
                                            "dbus disconnect");
        return;
    }

    if (g_strcmp0(method_name, "RecheckDevice") == 0) {
        const char *id = NULL;
        g_variant_get(parameters, "(&s)", &id);
        runtime_dbus_enqueue_device_command(rt,
                                            invocation,
                                            id,
                                            MB_LIFECYCLE_CMD_RECHECK,
                                            "dbus recheck");
        return;
    }

    g_dbus_method_invocation_return_error(invocation,
                                          G_DBUS_ERROR,
                                          G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "unknown method: %s",
                                          method_name ? method_name : "-");
}

static const GDBusInterfaceVTable runtime_dbus_vtable = {
    .method_call = runtime_dbus_handle_method_call,
};

static void runtime_dbus_name_acquired(GDBusConnection *connection,
                                       const char *name,
                                       gpointer user_data) {
    (void)connection;
    (void)user_data;

    g_print("D-Bus API name acquired: %s\n", printable_string(name, "-"));
}

static void runtime_dbus_name_lost(GDBusConnection *connection,
                                   const char *name,
                                   gpointer user_data) {
    (void)connection;
    (void)user_data;

    g_printerr("D-Bus API name lost: %s\n", printable_string(name, "-"));
}

static bool runtime_start_dbus_service(ConfigDirRuntime *rt) {
    if (!rt)
        return false;

    GError *error = NULL;

    rt->dbus_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!rt->dbus_bus) {
        g_printerr("D-Bus API unavailable: could not connect to session bus: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    rt->dbus_introspection =
        g_dbus_node_info_new_for_xml(runtime_dbus_introspection_xml, &error);
    if (!rt->dbus_introspection) {
        g_printerr("D-Bus API unavailable: invalid introspection XML: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    rt->dbus_object_id =
        g_dbus_connection_register_object(rt->dbus_bus,
                                          "/org/midi_ble_rt/Daemon",
                                          rt->dbus_introspection->interfaces[0],
                                          &runtime_dbus_vtable,
                                          rt,
                                          NULL,
                                          &error);

    if (!rt->dbus_object_id) {
        g_printerr("D-Bus API unavailable: could not register object: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    rt->dbus_name_id =
        g_bus_own_name_on_connection(rt->dbus_bus,
                                     "org.midi_ble_rt.Daemon",
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     runtime_dbus_name_acquired,
                                     runtime_dbus_name_lost,
                                     rt,
                                     NULL);

    g_print("D-Bus API object: /org/midi_ble_rt/Daemon\n");
    return true;
}

static void runtime_stop_dbus_service(ConfigDirRuntime *rt) {
    if (!rt)
        return;

    if (rt->dbus_name_id) {
        g_bus_unown_name(rt->dbus_name_id);
        rt->dbus_name_id = 0;
    }

    if (rt->dbus_object_id && rt->dbus_bus) {
        g_dbus_connection_unregister_object(rt->dbus_bus, rt->dbus_object_id);
        rt->dbus_object_id = 0;
    }

    if (rt->dbus_introspection) {
        g_dbus_node_info_unref(rt->dbus_introspection);
        rt->dbus_introspection = NULL;
    }

    if (rt->dbus_bus) {
        g_object_unref(rt->dbus_bus);
        rt->dbus_bus = NULL;
    }
}


static void runtime_control_handle_request(ConfigDirRuntime *rt,
                                           int client_fd,
                                           const char *request) {
    if (!rt || !request)
        return;

    char *line = g_strdup(request);
    g_strstrip(line);

    if (g_ascii_strcasecmp(line, "PING") == 0) {
        runtime_control_reply(client_fd, "OK PONG\n");
        g_free(line);
        return;
    }

    if (g_ascii_strcasecmp(line, "STATUS") == 0) {
        unsigned devices = rt->devices ? rt->devices->len : 0;
        unsigned streaming = 0;
        unsigned rx_workers = 0;
        unsigned tx_workers = 0;

        for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
            ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
            if (!runtime_device_is_active(dev))
                continue;

            if (device_session_is(dev, MB_SESSION_STREAMING))
                streaming++;

            if (dev->runtime_started && dev->runtime.rx.consumer_thread)
                rx_workers++;

            if (dev->runtime_started && dev->runtime.tx.consumer_thread)
                tx_workers++;
        }

        char *reply = g_strdup_printf(
            "OK STATUS devices=%u streaming=%u alsa_tx_thread=%s rx_workers=%u tx_workers=%u rtkit=%s rtkit_priority=%u rtkit_rx=%s rtkit_tx=%s lifecycle_busy=%s lifecycle_queue=%u\n",
            devices,
            streaming,
            rt->alsa_tx_thread ? "running" : "stopped",
            rx_workers,
            tx_workers,
            mb_rtkit_enabled() ? "on" : "off",
            mb_rtkit_priority(),
            mb_rtkit_realtime_rx_enabled() ? "on" : "off",
            mb_rtkit_realtime_tx_enabled() ? "on" : "off",
            rt->lifecycle_busy ? "yes" : "no",
            runtime_lifecycle_queue_depth(rt));

        runtime_control_reply(client_fd, reply);
        g_free(reply);
        g_free(line);
        return;
    }

    if (g_ascii_strcasecmp(line, "LIST") == 0) {
        runtime_control_reply(client_fd, "OK LIST\n");
        runtime_control_reply(client_fd, "id\taddress\tname\tstate\talsa_port\n");

        for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
            ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
            if (!runtime_device_is_active(dev))
                continue;

            char *row = g_strdup_printf("%s\t%s\t%s\t%s\t%d\n",
                                        printable_string(dev->config->id, "-"),
                                        printable_string(dev->config->address, "-"),
                                        printable_string(dev->config->name, "-"),
                                        mb_session_state_name(device_session_state(dev)),
                                        dev->alsa_port);
            runtime_control_reply(client_fd, row);
            g_free(row);
        }

        runtime_control_reply(client_fd, ".\n");
        g_free(line);
        return;
    }

    if (g_str_has_prefix(line, "FIND ")) {
        const char *id = line + 5;
        g_strstrip((char *)id);

        ConfigDeviceRuntime *dev = runtime_find_device_by_id(rt, id);
        if (!dev) {
            runtime_control_reply(client_fd, "ERR device not found\n");
            g_free(line);
            return;
        }

        char *reply = g_strdup_printf("OK DEVICE id=%s address=%s state=%s alsa_port=%d\n",
                                      printable_string(dev->config->id, "-"),
                                      printable_string(dev->config->address, "-"),
                                      mb_session_state_name(device_session_state(dev)),
                                      dev->alsa_port);
        runtime_control_reply(client_fd, reply);
        g_free(reply);
        g_free(line);
        return;
    }

    if (g_str_has_prefix(line, "CONNECT ")) {
        const char *id = line + 8;
        g_strstrip((char *)id);
        runtime_control_enqueue_lifecycle(rt,
                                          client_fd,
                                          id,
                                          MB_LIFECYCLE_CMD_CONNECT,
                                          "ctl connect");
        g_free(line);
        return;
    }

    if (g_str_has_prefix(line, "DISCONNECT ")) {
        const char *id = line + 11;
        g_strstrip((char *)id);
        runtime_control_enqueue_lifecycle(rt,
                                          client_fd,
                                          id,
                                          MB_LIFECYCLE_CMD_DISCONNECT,
                                          "ctl disconnect");
        g_free(line);
        return;
    }

    if (g_str_has_prefix(line, "RECHECK ")) {
        const char *id = line + 8;
        g_strstrip((char *)id);
        runtime_control_enqueue_lifecycle(rt,
                                          client_fd,
                                          id,
                                          MB_LIFECYCLE_CMD_RECHECK,
                                          "ctl recheck");
        g_free(line);
        return;
    }

    runtime_control_reply(client_fd, "ERR unknown command\n");
    g_free(line);
}

static gboolean runtime_control_socket_cb(gint fd,
                                          GIOCondition condition,
                                          gpointer user_data) {
    ConfigDirRuntime *rt = user_data;

    if (!rt || condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
        return G_SOURCE_CONTINUE;

    struct sockaddr_un peer;
    socklen_t peer_len = sizeof(peer);
    int client_fd = accept(fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            g_printerr("control socket accept failed: %s\n", g_strerror(errno));
        return G_SOURCE_CONTINUE;
    }

    char buf[512];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        runtime_control_handle_request(rt, client_fd, buf);
    } else {
        runtime_control_reply(client_fd, "ERR empty request\n");
    }

    close(client_fd);
    return G_SOURCE_CONTINUE;
}

static bool runtime_start_control_socket(ConfigDirRuntime *rt) {
    if (!rt)
        return false;

    rt->control_socket_path = runtime_control_socket_path();
    char *dir = g_path_get_dirname(rt->control_socket_path);

    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_printerr("Could not create runtime control directory %s: %s\n",
                   dir, g_strerror(errno));
        g_free(dir);
        return false;
    }
    g_free(dir);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_printerr("Could not create control socket: %s\n", g_strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(rt->control_socket_path) >= sizeof(addr.sun_path)) {
        g_printerr("Control socket path too long: %s\n", rt->control_socket_path);
        close(fd);
        return false;
    }

    strncpy(addr.sun_path, rt->control_socket_path, sizeof(addr.sun_path) - 1);
    unlink(rt->control_socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        g_printerr("Could not bind control socket %s: %s\n",
                   rt->control_socket_path, g_strerror(errno));
        close(fd);
        return false;
    }

    if (listen(fd, 8) != 0) {
        g_printerr("Could not listen on control socket %s: %s\n",
                   rt->control_socket_path, g_strerror(errno));
        close(fd);
        unlink(rt->control_socket_path);
        return false;
    }

    rt->control_socket_fd = fd;
    rt->control_socket_source_id = g_unix_fd_add(fd,
                                                 G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                                 runtime_control_socket_cb,
                                                 rt);

    g_print("Control socket: %s\n", rt->control_socket_path);
    return rt->control_socket_source_id != 0;
}

static void runtime_drain_alsa_tx_queue(ConfigDirRuntime *rt) {
    if (!rt->cfg.enable_tx || !rt->seq)
        return;

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
            runtime_stats_tx_drop(rt);
            runtime_device_stats_tx_drop(dev);
            lifecycle_enqueue_event(rt,
                                    dev,
                                    MB_EV_ALSA_DECODE_FAILED,
                                    "ALSA MIDI decode failed");
            continue;
        }
        g_mutex_unlock(&rt->alsa_lock);

        if (n == 0)
            continue;

        uint64_t epoch = 0;
        uint64_t now_ns = 0;
        if (!device_dataplane_stamp(dev, &epoch, &now_ns)) {
            runtime_stats_tx_drop(rt);
            runtime_device_stats_tx_drop(dev);
            continue;
        }

        if (rt->cfg.print_midi_events) {
            g_print("ALSA->BLE MIDI[%s]:", device_label(dev));
            for (long i = 0; i < n; i++)
                g_print(" %02x", midi[i]);
            g_print("\n");
        }

        if (!mb_duplex_runtime_push_tx_with_epoch(&dev->runtime,
                                                  midi,
                                                  (size_t)n,
                                                  now_ns,
                                                  epoch)) {
            g_printerr("TX runtime queue push failed for %s; ALSA event dropped.\n",
                       device_label(dev));
            runtime_stats_tx_drop(rt);
            runtime_device_stats_tx_drop(dev);
            lifecycle_enqueue_event(rt,
                                    dev,
                                    MB_EV_RUNTIME_FAILED,
                                    "TX runtime queue push failed");
        }
    }
}

static gpointer runtime_alsa_tx_thread_main(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;

    if (!rt->cfg.enable_tx || !rt->seq)
        return NULL;

    int nfds = snd_seq_poll_descriptors_count(rt->seq, POLLIN);
    if (nfds <= 0) {
        g_printerr("ALSA TX thread could not get poll descriptor count.\n");
        return NULL;
    }

    struct pollfd *pfds = g_new0(struct pollfd, (guint)nfds);

    g_mutex_lock(&rt->alsa_lock);
    int got = snd_seq_poll_descriptors(rt->seq, pfds, (unsigned int)nfds, POLLIN);
    g_mutex_unlock(&rt->alsa_lock);

    if (got < 0) {
        g_printerr("ALSA TX thread could not get poll descriptors: %s\n",
                   snd_strerror(got));
        g_free(pfds);
        return NULL;
    }

    g_print("ALSA TX thread started.\n");

    while (!g_atomic_int_get(&rt->alsa_tx_stop_requested)) {
        int pr = poll(pfds, (nfds_t)nfds, MB_ALSA_TX_POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            g_printerr("ALSA TX poll failed: %s\n", g_strerror(errno));
            break;
        }

        if (pr == 0)
            continue;

        runtime_drain_alsa_tx_queue(rt);
    }

    g_free(pfds);
    g_print("ALSA TX thread stopped.\n");
    return NULL;
}

static bool runtime_start_alsa_tx_thread(ConfigDirRuntime *rt) {
    if (!rt->cfg.enable_tx)
        return true;

    if (!rt->seq)
        return true;

    if (rt->alsa_tx_thread)
        return true;

    g_atomic_int_set(&rt->alsa_tx_stop_requested, 0);
    rt->alsa_tx_thread = g_thread_new("midi-ble-rt-alsa-tx",
                                      runtime_alsa_tx_thread_main,
                                      rt);
    if (!rt->alsa_tx_thread) {
        g_printerr("Could not start ALSA TX thread.\n");
        return false;
    }

    return true;
}

static void runtime_stop_alsa_tx_thread(ConfigDirRuntime *rt) {
    if (!rt || !rt->alsa_tx_thread)
        return;

    g_atomic_int_set(&rt->alsa_tx_stop_requested, 1);
    g_thread_join(rt->alsa_tx_thread);
    rt->alsa_tx_thread = NULL;
}

static void runtime_close_alsa_client(ConfigDirRuntime *rt) {
    if (!rt || !rt->seq)
        return;

    /*
     * Closing the shared ALSA client removes every per-device ALSA port.
     * This is a daemon shutdown/runtime teardown operation, not a normal
     * response to BLE link loss for one instrument/controller.
     */
    snd_seq_close(rt->seq);
    rt->seq = NULL;
}

static gboolean runtime_device_health_cb(gpointer user_data) {
    ConfigDirRuntime *rt = user_data;

    if (!lifecycle_is_idle(rt))
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

    if (!lifecycle_is_idle(rt))
        return G_SOURCE_CONTINUE;

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (runtime_device_is_active(dev) &&
            device_session_is(dev, MB_SESSION_RECONNECTING))
            lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_CONNECT, "reconnect timer");
    }

    return G_SOURCE_CONTINUE;
}

static void runtime_start_configured_devices(ConfigDirRuntime *rt) {
    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!runtime_device_is_active(dev) || !dev->config->connect_on_start)
            continue;

        lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_DISCOVER, "startup");
        lifecycle_enqueue(rt, dev, MB_LIFECYCLE_CMD_CONNECT, "startup");
    }
}

static bool runtime_has_streaming_device(ConfigDirRuntime *rt) {
    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (runtime_device_is_active(dev) &&
            device_session_is(dev, MB_SESSION_STREAMING))
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
    g_print("Stats: %s\n", rt->cfg.stats_enabled ? "enabled" : "disabled");
    if (rt->cfg.stats_enabled && rt->stats.path)
        g_print("Stats path: %s\n", rt->stats.path);

    for (unsigned i = 0; rt->devices && i < rt->devices->len; i++) {
        ConfigDeviceRuntime *dev = g_ptr_array_index(rt->devices, i);
        if (!runtime_device_is_active(dev))
            continue;

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

    runtime_stats_export_snapshot(rt);

    if (rt->stats_source_id) {
        g_source_remove(rt->stats_source_id);
        rt->stats_source_id = 0;
    }

    runtime_stop_dbus_service(rt);

    if (rt->sigint_source_id)
        g_source_remove(rt->sigint_source_id);
    if (rt->sigterm_source_id)
        g_source_remove(rt->sigterm_source_id);
    if (rt->reconnect_source_id)
        g_source_remove(rt->reconnect_source_id);
    if (rt->health_source_id)
        g_source_remove(rt->health_source_id);
    runtime_stop_alsa_tx_thread(rt);
    if (rt->control_socket_source_id)
        g_source_remove(rt->control_socket_source_id);
    if (rt->lifecycle_source_id)
        g_source_remove(rt->lifecycle_source_id);

    if (rt->control_socket_fd >= 0) {
        close(rt->control_socket_fd);
        rt->control_socket_fd = -1;
    }

    if (rt->control_socket_path) {
        unlink(rt->control_socket_path);
        g_clear_pointer(&rt->control_socket_path, g_free);
    }

    if (rt->devices)
        g_ptr_array_unref(rt->devices);

    if (rt->lifecycle_queue) {
        g_mutex_lock(&rt->lifecycle_lock);
        while (!g_queue_is_empty(rt->lifecycle_queue))
            lifecycle_command_free(g_queue_pop_head(rt->lifecycle_queue));
        g_queue_free(rt->lifecycle_queue);
        rt->lifecycle_queue = NULL;
        g_mutex_unlock(&rt->lifecycle_lock);
    }
    runtime_close_alsa_client(rt);
    if (rt->device_by_char_path)
        g_hash_table_unref(rt->device_by_char_path);
    if (rt->device_by_alsa_port)
        g_hash_table_unref(rt->device_by_alsa_port);
    mb_daemon_clear(&rt->daemon);
    mb_config_clear(&rt->cfg);
    if (rt->loop)
        g_main_loop_unref(rt->loop);
    g_clear_pointer(&rt->config_dir, g_free);
    if (rt->bus)
        g_object_unref(rt->bus);

    g_mutex_lock(&rt->stats_lock);
    mb_stats_clear(&rt->stats);
    g_mutex_unlock(&rt->stats_lock);

    g_mutex_clear(&rt->alsa_lock);
    g_mutex_clear(&rt->lifecycle_lock);
    g_mutex_clear(&rt->stats_lock);
}

static int run_config_directory_mode(const char *config_dir) {
    ConfigDirRuntime rt;
    memset(&rt, 0, sizeof(rt));
    rt.config_dir = g_strdup(config_dir);
    rt.control_socket_fd = -1;
    g_mutex_init(&rt.alsa_lock);
    g_mutex_init(&rt.lifecycle_lock);
    g_mutex_init(&rt.stats_lock);
    rt.lifecycle_queue = g_queue_new();
    rt.lifecycle_busy = false;
    rt.lifecycle_source_id = 0;
    rt.device_by_alsa_port = g_hash_table_new(g_direct_hash, g_direct_equal);
    rt.device_by_char_path = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (!mb_config_load_dir(&rt.cfg, config_dir)) {
        runtime_cleanup(&rt);
        return 1;
    }

    mb_stats_init(&rt.stats, rt.cfg.stats_enabled, rt.cfg.stats_interval_ms);

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
    runtime_stats_export_snapshot(&rt);

    rt.loop = g_main_loop_new(NULL, FALSE);
    rt.sigint_source_id = g_unix_signal_add(SIGINT, quit_main_loop, &rt);
    rt.sigterm_source_id = g_unix_signal_add(SIGTERM, quit_main_loop, &rt);
    rt.reconnect_source_id = g_timeout_add(MB_RECONNECT_INTERVAL_MS, runtime_reconnect_cb, &rt);
    rt.health_source_id = g_timeout_add(MB_DEVICE_HEALTH_INTERVAL_MS, runtime_device_health_cb, &rt);
    if (rt.cfg.stats_enabled)
        rt.stats_source_id = g_timeout_add(rt.cfg.stats_interval_ms,
                                           runtime_stats_export_cb,
                                           &rt);
    runtime_start_control_socket(&rt);

    if (!runtime_start_dbus_service(&rt))
        g_printerr("D-Bus API disabled; continuing with legacy control socket.\n");

    print_config_dir_devices(&rt, config_dir);
    runtime_start_configured_devices(&rt);

    if (!runtime_has_streaming_device(&rt)) {
        if (lifecycle_has_queued(&rt))
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
    if (argc == 3 &&
        (g_strcmp0(argv[1], "--config") == 0 ||
         g_strcmp0(argv[1], "--config-dir") == 0)) {
        if (!g_file_test(argv[2], G_FILE_TEST_IS_DIR)) {
            g_printerr("Config path must be a directory: %s\n", argv[2]);
            return 1;
        }

        return run_config_directory_mode(argv[2]);
    }

    g_printerr("Usage: %s --config <config-dir>\n", argv[0]);
    return 1;
}
