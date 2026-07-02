#include "mb-config.h"

#include "mb-ble-midi.h"
#include "mb-timeouts.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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
    char *raw = g_key_file_get_string(kf, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return fallback;
    }

    char *value = g_strstrip(raw);
    bool parsed = fallback;

    if (g_ascii_strcasecmp(value, "true") == 0 ||
        g_ascii_strcasecmp(value, "yes") == 0 ||
        g_ascii_strcasecmp(value, "on") == 0 ||
        g_strcmp0(value, "1") == 0) {
        parsed = true;
    } else if (g_ascii_strcasecmp(value, "false") == 0 ||
               g_ascii_strcasecmp(value, "no") == 0 ||
               g_ascii_strcasecmp(value, "off") == 0 ||
               g_strcmp0(value, "0") == 0) {
        parsed = false;
    } else {
        g_printerr("Invalid boolean value for [%s].%s: '%s'; using default %s.\n",
                   group,
                   key,
                   value,
                   fallback ? "true" : "false");
    }

    g_free(raw);
    return parsed;
}

static unsigned keyfile_get_uint_default(GKeyFile *kf, const char *group, const char *key, unsigned fallback) {
    GError *error = NULL;
    guint64 value = g_key_file_get_uint64(kf, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return fallback;
    }
    if (value == 0 || value > MB_CONFIG_MAX_INTERVAL_MS)
        return fallback;
    return (unsigned)value;
}

static const char *str_nonempty_or(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

static gint compare_filename_strings(gconstpointer a, gconstpointer b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return g_strcmp0(sa, sb);
}

static void mb_device_config_free(gpointer data) {
    MbDeviceConfig *device = data;
    if (!device)
        return;

    g_free(device->id);
    g_free(device->address);
    g_free(device->name);
    g_free(device->profile);
    g_free(device->alsa_port_name);
    g_free(device);
}

static GPtrArray *mb_config_new_device_array(void) {
    return g_ptr_array_new_with_free_func(mb_device_config_free);
}

static void mb_config_load_daemon_from_key_file(MbConfig *cfg, GKeyFile *kf) {
    cfg->address = g_strdup("");
    cfg->name = g_strdup("");
    cfg->pair = keyfile_get_bool_default(kf, "defaults", "pair", false);
    cfg->trust = keyfile_get_bool_default(kf, "defaults", "trust", true);
    cfg->reconnect_on_link_loss = keyfile_get_bool_default(kf, "defaults", "reconnect_on_link_loss", true);

    cfg->service_uuid = keyfile_get_string_default(kf, "gatt", "service_uuid",
        MB_BLE_MIDI_SERVICE_UUID);
    cfg->io_uuid = keyfile_get_string_default(kf, "gatt", "io_uuid",
        MB_BLE_MIDI_IO_UUID);
    cfg->io_uuid_alias = keyfile_get_string_default(kf, "gatt", "io_uuid_alias", "");
    cfg->require_notify = keyfile_get_bool_default(kf, "gatt", "require_notify", true);
    cfg->require_write_without_response = keyfile_get_bool_default(kf, "gatt", "require_write_without_response", true);

    cfg->alsa_client_name = keyfile_get_string_default(kf, "daemon", "client_name", "midi-ble-rt");
    cfg->alsa_port_name = keyfile_get_string_default(kf, "alsa", "port_name", "BLE-MIDI");

    cfg->print_ble_packets = keyfile_get_bool_default(kf, "debug", "print_ble_packets", false);
    cfg->print_midi_events = keyfile_get_bool_default(kf, "debug", "print_midi_events", false);
    cfg->enable_tx = keyfile_get_bool_default(kf, "defaults", "enable_tx", true);
    cfg->stats_enabled = keyfile_get_bool_default(kf, "stats", "enabled", true);
    cfg->stats_latency_diagnostics = keyfile_get_bool_default(kf, "stats", "latency_diagnostics", false);
    cfg->stats_interval_ms = keyfile_get_uint_default(kf, "stats", "interval_ms", MB_STATS_DEFAULT_INTERVAL_MS);
}

static MbDeviceConfig *mb_device_config_from_key_file(GKeyFile *kf,
                                                      const MbConfig *defaults,
                                                      const char *fallback_id) {
    MbDeviceConfig *device = g_new0(MbDeviceConfig, 1);

    device->id = keyfile_get_string_default(kf, "device", "id", fallback_id ? fallback_id : "device");
    device->address = keyfile_get_string_default(kf, "device", "address", "");
    device->name = keyfile_get_string_default(kf, "device", "name", "");
    device->profile = keyfile_get_string_default(kf, "device", "profile", "standard_ble_midi");
    device->alsa_port_name = keyfile_get_string_default(kf, "device", "alsa_port_name",
        str_nonempty_or(defaults ? defaults->alsa_port_name : NULL, "BLE-MIDI"));
    device->enabled = keyfile_get_bool_default(kf, "device", "enabled", true);
    device->connect_on_start = keyfile_get_bool_default(kf, "device", "connect_on_start", false);
    device->pair = keyfile_get_bool_default(kf, "policy", "pair", defaults ? defaults->pair : false);
    device->trust = keyfile_get_bool_default(kf, "policy", "trust", defaults ? defaults->trust : true);
    device->reconnect_on_link_loss = keyfile_get_bool_default(kf, "policy", "reconnect_on_link_loss",
        defaults ? defaults->reconnect_on_link_loss : true);
    device->enable_tx = keyfile_get_bool_default(kf, "midi", "enable_tx", defaults ? defaults->enable_tx : true);

    return device;
}

static void mb_config_add_device_if_valid(MbConfig *cfg,
                                          GHashTable *seen_ids,
                                          MbDeviceConfig *device,
                                          const char *path) {
    if (!device)
        return;

    if (!device->enabled || !device->address || !*device->address) {
        mb_device_config_free(device);
        return;
    }

    if (device->id && *device->id && g_hash_table_contains(seen_ids, device->id)) {
        g_printerr("Config %s: id '%s' already configured; ignoring duplicate configuration.\n",
                   path,
                   device->id);
        mb_device_config_free(device);
        return;
    }

    if (device->id && *device->id)
        g_hash_table_add(seen_ids, g_strdup(device->id));

    g_ptr_array_add(cfg->devices, device);
}

bool mb_config_load_dir(MbConfig *cfg, const char *dir_path) {
    if (!cfg || !dir_path)
        return false;

    char *daemon_path = g_build_filename(dir_path, "daemon.ini", NULL);
    GKeyFile *daemon_kf = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(daemon_kf, daemon_path, G_KEY_FILE_NONE, &error)) {
        g_printerr("Failed to load daemon config %s: %s\n", daemon_path, error->message);
        g_clear_error(&error);
        g_key_file_unref(daemon_kf);
        g_free(daemon_path);
        return false;
    }

    MbConfig tmp;
    memset(&tmp, 0, sizeof(tmp));
    mb_config_load_daemon_from_key_file(&tmp, daemon_kf);
    tmp.devices = mb_config_new_device_array();
    g_key_file_unref(daemon_kf);
    g_free(daemon_path);

    char *devices_dir = g_build_filename(dir_path, "devices.d", NULL);
    GDir *dir = g_dir_open(devices_dir, 0, &error);
    if (!dir) {
        g_printerr("Failed to open devices directory %s: %s\n", devices_dir, error->message);
        g_clear_error(&error);
        g_free(devices_dir);
        mb_config_clear(&tmp);
        return false;
    }

    GPtrArray *filenames = g_ptr_array_new_with_free_func(g_free);
    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_suffix(name, ".ini"))
            g_ptr_array_add(filenames, g_strdup(name));
    }
    g_dir_close(dir);
    g_ptr_array_sort(filenames, compare_filename_strings);

    GHashTable *seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (unsigned i = 0; i < filenames->len; i++) {
        name = g_ptr_array_index(filenames, i);
        char *path = g_build_filename(devices_dir, name, NULL);
        GKeyFile *device_kf = g_key_file_new();
        error = NULL;

        if (!g_key_file_load_from_file(device_kf, path, G_KEY_FILE_NONE, &error)) {
            g_printerr("Skipping device config %s: %s\n", path, error->message);
            g_clear_error(&error);
            g_key_file_unref(device_kf);
            g_free(path);
            continue;
        }

        char *fallback_id = g_strndup(name, strlen(name) - 4);
        MbDeviceConfig *device = mb_device_config_from_key_file(device_kf, &tmp, fallback_id);
        g_free(fallback_id);
        g_key_file_unref(device_kf);

        mb_config_add_device_if_valid(&tmp, seen_ids, device, path);
        g_free(path);
    }

    g_hash_table_unref(seen_ids);
    g_ptr_array_unref(filenames);
    g_free(devices_dir);

    mb_config_clear(cfg);
    *cfg = tmp;
    return true;
}

void mb_config_clear(MbConfig *cfg) {
    if (!cfg)
        return;

    g_free(cfg->address);
    g_free(cfg->name);
    g_free(cfg->service_uuid);
    g_free(cfg->io_uuid);
    g_free(cfg->io_uuid_alias);
    g_free(cfg->alsa_client_name);
    g_free(cfg->alsa_port_name);
    if (cfg->devices)
        g_ptr_array_unref(cfg->devices);

    memset(cfg, 0, sizeof(*cfg));
}

unsigned mb_config_device_count(const MbConfig *cfg) {
    return cfg && cfg->devices ? cfg->devices->len : 0;
}

const MbDeviceConfig *mb_config_get_device(const MbConfig *cfg, unsigned index) {
    if (!cfg || !cfg->devices || index >= cfg->devices->len)
        return NULL;
    return g_ptr_array_index(cfg->devices, index);
}
