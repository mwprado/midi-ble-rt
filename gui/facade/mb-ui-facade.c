#include "mb-ui-facade.h"

#include <gio/gio.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct _MbUiFacade {
    char *ctl_path;
    GPtrArray *scan_devices;
};

void mb_ui_device_config_defaults(MbUiDeviceConfig *config) {
    if (!config)
        return;

    config->connect_on_start = true;
    config->reconnect_on_link_loss = true;
    config->enable_tx = true;
    config->pair = false;
    config->trust = true;
}


static void ui_log_backend_error(const char *context, const GError *error) {
    g_printerr("[midi-ble-rt-gui] %s failed: %s\n",
               context ? context : "backend command",
               error && error->message ? error->message : "unknown error");
}

static void ui_log_backend_step(const char *context) {
    g_printerr("[midi-ble-rt-gui] %s\n", context ? context : "backend step");
}

static const char *safe_str(const char *s, const char *fallback) {
    return s && *s ? s : fallback;
}

static bool facade_load_scan_devices_from_daemon_dbus(MbUiFacade *facade,
                                                      unsigned timeout_seconds,
                                                      GError **error);


static char *scan_device_id_from_name_address(const char *name, const char *address) {
    const char *base = name && *name ? name : address;
    if (!base || !*base)
        base = "ble-midi-device";

    char *tmp = g_ascii_strdown(base, -1);

    for (char *q = tmp; *q; q++) {
        if (!g_ascii_isalnum(*q))
            *q = '-';
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

    g_strfreev(parts);
    g_free(tmp);

    if (out->len == 0)
        g_string_append(out, "ble-midi-device");

    return g_string_free(out, FALSE);
}

static bool snapshot_has_device_address_or_id(MbUiSnapshot *snapshot,
                                              const char *id,
                                              const char *address) {
    if (!snapshot || !snapshot->devices)
        return false;

    for (guint i = 0; i < snapshot->devices->len; i++) {
        const MbUiDevice *device = g_ptr_array_index(snapshot->devices, i);
        if (!device)
            continue;

        /*
         * Catalog identity is the Bluetooth address.
         *
         * The UI id is a derived label/cache key only.  It must not merge two
         * different instruments that happen to share the same generated id.
         */
        if (address && *address && device->address && *device->address)
            if (g_ascii_strcasecmp(address, device->address) == 0)
                return true;

        /*
         * Fallback only for legacy/incomplete entries with no address.
         */
        if ((!address || !*address || !device->address || !*device->address) &&
            id && *id && device->id && *device->id &&
            g_ascii_strcasecmp(id, device->id) == 0)
            return true;
    }

    return false;
}

static void merge_scan_cache_into_snapshot(MbUiFacade *facade, MbUiSnapshot *snapshot) {
    if (!facade || !facade->scan_devices || !snapshot || !snapshot->devices)
        return;

    for (guint i = 0; i < facade->scan_devices->len; i++) {
        const MbUiDevice *scan_device = g_ptr_array_index(facade->scan_devices, i);

        if (snapshot_has_device_address_or_id(snapshot, scan_device->id, scan_device->address))
            continue;

        MbUiDevice *copy = mb_ui_device_new(safe_str(scan_device->id, "-"),
                                            safe_str(scan_device->address, "-"),
                                            safe_str(scan_device->name, "Dispositivo BLE-MIDI"),
                                            "IDLE",
                                            -1);
        copy->paired = scan_device->paired;
        copy->trusted = scan_device->trusted;
        copy->connected = scan_device->connected;
        copy->gatt_resolved = scan_device->gatt_resolved;
        copy->profile = g_strdup(scan_device->profile);
        g_ptr_array_add(snapshot->devices, copy);
    }
}

static char *devices_config_dir(void) {
    return g_build_filename(g_get_user_config_dir(),
                            "midi-ble-rt",
                            "devices.d",
                            NULL);
}

static char *key_file_get_optional_string(GKeyFile *key,
                                          const char *group,
                                          const char *name) {
    if (!key || !group || !name)
        return NULL;

    GError *error = NULL;
    char *value = g_key_file_get_string(key, group, name, &error);

    if (error) {
        g_clear_error(&error);
        return NULL;
    }

    if (value)
        g_strstrip(value);

    if (!value || !*value) {
        g_free(value);
        return NULL;
    }

    return value;
}

static char *key_file_get_first_optional_string(GKeyFile *key,
                                                const char *group1,
                                                const char *name1,
                                                const char *group2,
                                                const char *name2) {
    char *value = key_file_get_optional_string(key, group1, name1);

    if (value)
        return value;

    return key_file_get_optional_string(key, group2, name2);
}


static char *run_ctl(MbUiFacade *facade,
                     const char *arg1,
                     const char *arg2,
                     const char *arg3,
                     GError **error) {
    if (!facade || !facade->ctl_path || !arg1) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid UI façade command");
        return NULL;
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, facade->ctl_path);
    g_ptr_array_add(argv, (char *)arg1);
    if (arg2)
        g_ptr_array_add(argv, (char *)arg2);
    if (arg3)
        g_ptr_array_add(argv, (char *)arg3);
    g_ptr_array_add(argv, NULL);

    char *stdout_text = NULL;
    char *stderr_text = NULL;
    int wait_status = 0;

    gboolean ok = g_spawn_sync(NULL,
                               (char **)argv->pdata,
                               NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL,
                               NULL,
                               &stdout_text,
                               &stderr_text,
                               &wait_status,
                               error);

    g_ptr_array_free(argv, TRUE);

    if (!ok) {
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    if (!g_spawn_check_wait_status(wait_status, error)) {
        if (stderr_text && *stderr_text)
            g_prefix_error(error, "%s: ", g_strstrip(stderr_text));
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    g_free(stderr_text);
    return stdout_text;
}

static char *sanitize_device_id(const char *device_id) {
    if (!device_id || !*device_id)
        return g_strdup("ble-midi-device");

    GString *out = g_string_new(NULL);
    for (const char *p = device_id; *p; p++) {
        char c = *p;
        if (g_ascii_isalnum(c))
            g_string_append_c(out, g_ascii_tolower(c));
        else if (c == '-' || c == '_')
            g_string_append_c(out, c);
        else
            g_string_append_c(out, '-');
    }

    while (strstr(out->str, "--")) {
        char *dup = g_strdup(out->str);
        char *pos = strstr(dup, "--");
        if (!pos) {
            g_free(dup);
            break;
        }
        memmove(pos, pos + 1, strlen(pos));
        g_string_assign(out, dup);
        g_free(dup);
    }

    if (out->len == 0)
        g_string_assign(out, "ble-midi-device");

    return g_string_free(out, FALSE);
}


static bool facade_variant_dict_lookup_bool(GVariant *dict,
                                            const char *key,
                                            bool *out) {
    if (!dict || !key || !out)
        return false;

    GVariant *value = g_variant_lookup_value(dict, key, NULL);
    if (!value)
        return false;

    bool ok = false;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        *out = g_variant_get_boolean(value);
        ok = true;
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        if (g_variant_is_of_type(inner, G_VARIANT_TYPE_BOOLEAN)) {
            *out = g_variant_get_boolean(inner);
            ok = true;
        }
        g_variant_unref(inner);
    }

    g_variant_unref(value);
    return ok;
}

static char *facade_variant_dict_dup_string(GVariant *dict,
                                            const char *key) {
    if (!dict || !key)
        return NULL;

    GVariant *value = g_variant_lookup_value(dict, key, NULL);
    if (!value)
        return NULL;

    char *out = NULL;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        out = g_strdup(g_variant_get_string(value, NULL));
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        if (g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING))
            out = g_strdup(g_variant_get_string(inner, NULL));
        g_variant_unref(inner);
    }

    g_variant_unref(value);
    return out;
}


static int facade_variant_dict_get_int32(GVariant *dict,
                                         const char *key,
                                         int fallback) {
    if (!dict || !key)
        return fallback;

    GVariant *value = g_variant_lookup_value(dict, key, NULL);
    if (!value)
        return fallback;

    int out = fallback;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
        out = g_variant_get_int32(value);
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        if (g_variant_is_of_type(inner, G_VARIANT_TYPE_INT32))
            out = g_variant_get_int32(inner);
        g_variant_unref(inner);
    }

    g_variant_unref(value);
    return out;
}

static MbUiDevice *facade_device_from_daemon_dbus_dict(GVariant *dict) {
    if (!dict)
        return NULL;

    char *id = facade_variant_dict_dup_string(dict, "Id");
    char *address = facade_variant_dict_dup_string(dict, "Address");
    char *name = facade_variant_dict_dup_string(dict, "Name");
    char *state = facade_variant_dict_dup_string(dict, "RuntimeState");
    char *profile = facade_variant_dict_dup_string(dict, "Profile");
    char *alsa_port_name = facade_variant_dict_dup_string(dict, "AlsaPortName");

    bool enabled = false;
    bool connect_on_start = false;
    bool reconnect_on_link_loss = false;
    bool enable_tx = false;
    bool streaming = false;

    facade_variant_dict_lookup_bool(dict, "Enabled", &enabled);
    facade_variant_dict_lookup_bool(dict, "ConnectOnStart", &connect_on_start);
    facade_variant_dict_lookup_bool(dict, "ReconnectOnLinkLoss", &reconnect_on_link_loss);
    facade_variant_dict_lookup_bool(dict, "EnableTx", &enable_tx);
    facade_variant_dict_lookup_bool(dict, "Streaming", &streaming);

    int alsa_port = facade_variant_dict_get_int32(dict, "AlsaPort", -1);

    MbUiDevice *device = mb_ui_device_new(id,
                                          address,
                                          name,
                                          state && *state ? state : "UNKNOWN",
                                          alsa_port);

    device->imported = true;
    device->profile = g_strdup(profile);
    device->alsa_port_name = g_strdup(alsa_port_name);

    device->enabled = g_strdup(enabled ? "yes" : "no");
    device->connect_on_start = g_strdup(connect_on_start ? "yes" : "no");
    device->policy_reconnect_on_link_loss = g_strdup(reconnect_on_link_loss ? "yes" : "no");
    device->midi_enable_tx = g_strdup(enable_tx ? "yes" : "no");

    /*
     * These fields are runtime-facing.  The daemon D-Bus API is now the source
     * of truth for the main GUI; BlueZ is no longer queried directly here.
     */
    device->connected = streaming;
    device->gatt_resolved = streaming;

    g_free(alsa_port_name);
    g_free(profile);
    g_free(state);
    g_free(name);
    g_free(address);
    g_free(id);

    return device;
}

static GVariant *facade_daemon_dbus_call_sync(const char *method,
                                              GVariant *parameters,
                                              const GVariantType *reply_type,
                                              GError **error) {
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!connection)
        return NULL;

    GVariant *reply = g_dbus_connection_call_sync(connection,
                                                  "org.midi_ble_rt.Daemon",
                                                  "/org/midi_ble_rt/Daemon",
                                                  "org.midi_ble_rt.Daemon1",
                                                  method,
                                                  parameters,
                                                  reply_type,
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  error);
    g_object_unref(connection);
    return reply;
}

static bool facade_daemon_dbus_device_command(const char *method,
                                              const char *device_id,
                                              GError **error) {
    if (!device_id || !*device_id) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "no device selected");
        return false;
    }

    GVariant *reply = facade_daemon_dbus_call_sync(method,
                                                   g_variant_new("(s)", device_id),
                                                   NULL,
                                                   error);
    if (!reply)
        return false;

    g_variant_unref(reply);
    return true;
}

static bool facade_load_snapshot_from_daemon_dbus(MbUiSnapshot *snapshot,
                                                  GError **error) {
    if (!snapshot || !snapshot->devices) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid UI snapshot");
        return false;
    }

    GVariant *reply = facade_daemon_dbus_call_sync("ListDevices",
                                                   NULL,
                                                   G_VARIANT_TYPE("(aa{sv})"),
                                                   error);
    if (!reply)
        return false;

    GVariant *devices = NULL;
    g_variant_get(reply, "(@aa{sv})", &devices);

    GVariantIter iter;
    GVariant *dict = NULL;

    g_variant_iter_init(&iter, devices);
    while (g_variant_iter_next(&iter, "@a{sv}", &dict)) {
        MbUiDevice *device = facade_device_from_daemon_dbus_dict(dict);
        if (device)
            g_ptr_array_add(snapshot->devices, device);
        g_variant_unref(dict);
    }

    g_variant_unref(devices);
    g_variant_unref(reply);

    snapshot->status.online = true;
    snapshot->status.devices = snapshot->devices ? snapshot->devices->len : 0;

    for (guint i = 0; snapshot->devices && i < snapshot->devices->len; i++) {
        const MbUiDevice *device = g_ptr_array_index(snapshot->devices, i);
        if (device && device->state && g_strcmp0(device->state, "STREAMING") == 0)
            snapshot->status.streaming++;
    }

    return true;
}


static gint compare_config_filename_ptrs(gconstpointer a, gconstpointer b) {
    const char * const *sa = a;
    const char * const *sb = b;
    return g_strcmp0(sa ? *sa : NULL, sb ? *sb : NULL);
}

static bool bt_address_is_valid_for_catalog(const char *address) {
    if (!address || strlen(address) != 17)
        return false;

    for (guint i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (address[i] != ':')
                return false;
        } else if (!g_ascii_isxdigit(address[i])) {
            return false;
        }
    }

    return true;
}

static void load_imported_device_configs(MbUiSnapshot *snapshot) {
    if (!snapshot || !snapshot->devices)
        return;

    char *dir = devices_config_dir();

    g_printerr("[midi-ble-rt-gui] catalog: loading dir=%s\n",
               dir ? dir : "-");

    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        g_printerr("[midi-ble-rt-gui] catalog: directory does not exist\n");
        g_free(dir);
        return;
    }

    GError *error = NULL;
    GDir *gdir = g_dir_open(dir, 0, &error);
    if (!gdir) {
        g_printerr("[midi-ble-rt-gui] catalog: cannot open dir: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        g_free(dir);
        return;
    }

    GPtrArray *filenames = g_ptr_array_new_with_free_func(g_free);
    const char *entry = NULL;

    while ((entry = g_dir_read_name(gdir)) != NULL) {
        if (g_str_has_suffix(entry, ".ini"))
            g_ptr_array_add(filenames, g_strdup(entry));
    }

    g_dir_close(gdir);
    g_ptr_array_sort(filenames, compare_config_filename_ptrs);

    g_printerr("[midi-ble-rt-gui] catalog: ini files=%u\n",
               filenames->len);

    for (guint i = 0; i < filenames->len; i++) {
        const char *filename = g_ptr_array_index(filenames, i);
        char *path = g_build_filename(dir, filename, NULL);

        g_printerr("[midi-ble-rt-gui] catalog: reading %s\n",
                   path ? path : "-");

        GKeyFile *key = g_key_file_new();

        error = NULL;
        if (!g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, &error)) {
            g_printerr("[midi-ble-rt-gui] catalog: invalid ini %s: %s\n",
                       filename ? filename : "-",
                       error && error->message ? error->message : "unknown error");
            g_clear_error(&error);
            g_key_file_unref(key);
            g_free(path);
            continue;
        }

        char *id = key_file_get_optional_string(key, "device", "id");
        char *address = key_file_get_optional_string(key, "device", "address");
        char *name = key_file_get_optional_string(key, "device", "name");
        char *profile = key_file_get_optional_string(key, "device", "profile");

        char *enabled = key_file_get_optional_string(key, "device", "enabled");
        char *connect_on_start = key_file_get_optional_string(key, "device", "connect_on_start");
        char *alsa_port_name = key_file_get_first_optional_string(key,
                                                                  "device",
                                                                  "alsa_port_name",
                                                                  "alsa",
                                                                  "port_name");
        char *policy_pair = key_file_get_first_optional_string(key,
                                                               "policy",
                                                               "pair",
                                                               "device",
                                                               "pair");
        char *policy_trust = key_file_get_first_optional_string(key,
                                                                "policy",
                                                                "trust",
                                                                "device",
                                                                "trust");
        char *policy_reconnect = key_file_get_first_optional_string(key,
                                                                    "policy",
                                                                    "reconnect_on_link_loss",
                                                                    "device",
                                                                    "auto_reconnect");
        char *midi_enable_tx = key_file_get_optional_string(key, "midi", "enable_tx");
        char *gatt_service_uuid = key_file_get_optional_string(key, "gatt", "service_uuid");
        char *gatt_io_uuid = key_file_get_optional_string(key, "gatt", "io_uuid");
        char *gatt_io_uuid_alias = key_file_get_optional_string(key, "gatt", "io_uuid_alias");

        if (!bt_address_is_valid_for_catalog(address)) {
            g_printerr("[midi-ble-rt-gui] catalog: ignored %s: invalid address=%s\n",
                       filename ? filename : "-",
                       address ? address : "-");

            g_free(gatt_io_uuid_alias);
            g_free(gatt_io_uuid);
            g_free(gatt_service_uuid);
            g_free(midi_enable_tx);
            g_free(policy_reconnect);
            g_free(policy_trust);
            g_free(policy_pair);
            g_free(alsa_port_name);
            g_free(connect_on_start);
            g_free(enabled);
            g_free(profile);
            g_free(name);
            g_free(address);
            g_free(id);
            g_key_file_unref(key);
            g_free(path);
            continue;
        }

        char *generated_id = NULL;
        const char *effective_id = id && *id ? id : NULL;
        const char *effective_name = name && *name ? name : NULL;

        if (!effective_id) {
            generated_id = scan_device_id_from_name_address(effective_name, address);
            effective_id = generated_id;
        }

        if (!effective_name)
            effective_name = effective_id && *effective_id ? effective_id : address;

        if (!snapshot_has_device_address_or_id(snapshot, NULL, address)) {
            MbUiDevice *device = mb_ui_device_new(effective_id,
                                                  address,
                                                  effective_name,
                                                  "DISCONNECTED",
                                                  -1);
            device->profile = g_strdup(profile);
            device->imported = true;
            device->config_file = g_strdup(filename);
            device->enabled = g_strdup(enabled);
            device->connect_on_start = g_strdup(connect_on_start);
            device->alsa_port_name = g_strdup(alsa_port_name);
            device->policy_pair = g_strdup(policy_pair);
            device->policy_trust = g_strdup(policy_trust);
            device->policy_reconnect_on_link_loss = g_strdup(policy_reconnect);
            device->midi_enable_tx = g_strdup(midi_enable_tx);
            device->gatt_service_uuid = g_strdup(gatt_service_uuid);
            device->gatt_io_uuid = g_strdup(gatt_io_uuid);
            device->gatt_io_uuid_alias = g_strdup(gatt_io_uuid_alias);
            g_ptr_array_add(snapshot->devices, device);

            g_printerr("[midi-ble-rt-gui] catalog: added file=%s id=%s address=%s name=%s profile=%s total=%u\n",
                       filename ? filename : "-",
                       effective_id ? effective_id : "-",
                       address ? address : "-",
                       effective_name ? effective_name : "-",
                       profile && *profile ? profile : "standard_ble_midi",
                       snapshot->devices->len);
        } else {
            g_printerr("[midi-ble-rt-gui] catalog: ignored duplicate file=%s address=%s\n",
                       filename ? filename : "-",
                       address ? address : "-");
        }

        g_free(generated_id);
        g_free(gatt_io_uuid_alias);
        g_free(gatt_io_uuid);
        g_free(gatt_service_uuid);
        g_free(midi_enable_tx);
        g_free(policy_reconnect);
        g_free(policy_trust);
        g_free(policy_pair);
        g_free(alsa_port_name);
        g_free(connect_on_start);
        g_free(enabled);
        g_free(profile);
        g_free(name);
        g_free(address);
        g_free(id);
        g_key_file_unref(key);
        g_free(path);
    }

    g_printerr("[midi-ble-rt-gui] catalog: final devices=%u\n",
               snapshot->devices->len);

    g_ptr_array_free(filenames, TRUE);
    g_free(dir);
}


static char *device_config_path(const MbUiDevice *device, GError **error) {
    char *safe_id = sanitize_device_id(device ? device->id : NULL);
    char *dir = devices_config_dir();

    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "failed to create config directory: %s",
                    dir);
        g_free(safe_id);
        g_free(dir);
        return NULL;
    }

    char *filename = g_strdup_printf("%s.ini", safe_id);
    char *path = g_build_filename(dir, filename, NULL);

    g_free(filename);
    g_free(safe_id);
    g_free(dir);
    return path;
}

MbUiFacade *mb_ui_facade_new(void) {
    MbUiFacade *facade = g_new0(MbUiFacade, 1);
    facade->ctl_path = g_strdup("midi-ble-rtctl");
    facade->scan_devices = g_ptr_array_new_with_free_func((GDestroyNotify)mb_ui_device_free);
    return facade;
}

void mb_ui_facade_free(MbUiFacade *facade) {
    if (!facade)
        return;

    g_free(facade->ctl_path);
    if (facade->scan_devices)
        g_ptr_array_unref(facade->scan_devices);
    g_free(facade);
}


bool mb_ui_facade_scan_devices(MbUiFacade *facade,
                               unsigned timeout_seconds,
                               GError **error) {
    return facade_load_scan_devices_from_daemon_dbus(facade,
                                                     timeout_seconds,
                                                     error);
}


MbUiSnapshot *mb_ui_facade_get_snapshot(MbUiFacade *facade) {
    (void)facade;

    MbUiSnapshot *snapshot = mb_ui_snapshot_new();
    GError *error = NULL;

    /*
     * Mature GUI architecture:
     *
     * The daemon is the authority for catalog, BlueZ and runtime state.
     * The GUI facade must not rebuild daemon state from .ini files, BlueZ or
     * the legacy local control socket.
     */
    if (!facade_load_snapshot_from_daemon_dbus(snapshot, &error)) {
        snapshot->status.online = false;
        snapshot->last_error = g_strdup(error && error->message
                                        ? error->message
                                        : "daemon D-Bus API unavailable");
        g_printerr("[midi-ble-rt-gui] daemon D-Bus snapshot failed: %s\n",
                   snapshot->last_error ? snapshot->last_error : "unknown error");
        g_clear_error(&error);
    }

    return snapshot;
}


static void mark_scan_devices_imported_from_catalog(MbUiSnapshot *scan_snapshot) {
    if (!scan_snapshot || !scan_snapshot->devices)
        return;

    MbUiSnapshot *catalog = mb_ui_snapshot_new();
    load_imported_device_configs(catalog);

    for (guint i = 0; i < scan_snapshot->devices->len; i++) {
        MbUiDevice *scan_device = g_ptr_array_index(scan_snapshot->devices, i);
        if (!scan_device)
            continue;

        for (guint j = 0; j < catalog->devices->len; j++) {
            const MbUiDevice *configured = g_ptr_array_index(catalog->devices, j);
            if (!configured)
                continue;

            bool same_address =
                scan_device->address && configured->address &&
                g_ascii_strcasecmp(scan_device->address, configured->address) == 0;

            bool same_id =
                scan_device->id && configured->id &&
                g_strcmp0(scan_device->id, configured->id) == 0;

            if (same_address || same_id) {
                scan_device->imported = true;
                break;
            }
        }
    }

    mb_ui_snapshot_free(catalog);
}


static MbUiDevice *facade_scan_device_from_daemon_dbus_dict(GVariant *dict) {
    if (!dict)
        return NULL;

    char *id = facade_variant_dict_dup_string(dict, "Id");
    char *address = facade_variant_dict_dup_string(dict, "Address");
    char *name = facade_variant_dict_dup_string(dict, "Name");
    char *state = facade_variant_dict_dup_string(dict, "RuntimeState");
    char *profile = facade_variant_dict_dup_string(dict, "Profile");

    bool imported = false;
    bool paired = false;
    bool trusted = false;
    bool connected = false;

    facade_variant_dict_lookup_bool(dict, "Imported", &imported);
    facade_variant_dict_lookup_bool(dict, "Paired", &paired);
    facade_variant_dict_lookup_bool(dict, "Trusted", &trusted);
    facade_variant_dict_lookup_bool(dict, "Connected", &connected);

    int rssi = facade_variant_dict_get_int32(dict, "RSSI", 0);

    MbUiDevice *device = mb_ui_device_new(id,
                                          address,
                                          name,
                                          state && *state ? state : "DISCOVERED",
                                          -1);

    device->imported = imported;
    device->paired = paired;
    device->trusted = trusted;
    device->connected = connected;
    device->profile = g_strdup(profile && *profile ? profile : "standard_ble_midi");

    /*
     * Reuse config_file as a diagnostic carrier for scan RSSI until the UI model
     * gets a dedicated discovery metadata struct.
     */
    device->config_file = g_strdup_printf("RSSI=%d", rssi);

    g_free(profile);
    g_free(state);
    g_free(name);
    g_free(address);
    g_free(id);

    return device;
}

static bool facade_load_scan_devices_from_daemon_dbus(MbUiFacade *facade,
                                                      unsigned timeout_seconds,
                                                      GError **error) {
    if (!facade) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid UI facade");
        return false;
    }

    if (timeout_seconds < 1)
        timeout_seconds = 1;
    if (timeout_seconds > 30)
        timeout_seconds = 30;

    GVariant *reply = facade_daemon_dbus_call_sync("ScanDevices",
                                                   g_variant_new("(u)", (guint)timeout_seconds),
                                                   G_VARIANT_TYPE("(aa{sv})"),
                                                   error);
    if (!reply)
        return false;

    if (!facade->scan_devices)
        facade->scan_devices = g_ptr_array_new_with_free_func((GDestroyNotify)mb_ui_device_free);
    else
        g_ptr_array_set_size(facade->scan_devices, 0);

    GVariant *devices = NULL;
    g_variant_get(reply, "(@aa{sv})", &devices);

    GVariantIter iter;
    GVariant *dict = NULL;

    g_variant_iter_init(&iter, devices);
    while (g_variant_iter_next(&iter, "@a{sv}", &dict)) {
        MbUiDevice *device = facade_scan_device_from_daemon_dbus_dict(dict);
        if (device)
            g_ptr_array_add(facade->scan_devices, device);
        g_variant_unref(dict);
    }

    g_variant_unref(devices);
    g_variant_unref(reply);
    return true;
}

MbUiSnapshot *mb_ui_facade_get_scan_snapshot(MbUiFacade *facade) {
    /*
     * Snapshot exclusivo do diálogo de pareamento.
     *
     * Não deve partir de mb_ui_facade_get_snapshot(), porque aquele snapshot
     * representa a janela principal: dispositivos cadastrados/configurados.
     *
     * O diálogo precisa ver apenas o resultado bruto do último scan.
     */
    MbUiSnapshot *snapshot = mb_ui_snapshot_new();

    snapshot->status.online = false;

    merge_scan_cache_into_snapshot(facade, snapshot);

    mark_scan_devices_imported_from_catalog(snapshot);
    return snapshot;
}


bool mb_ui_facade_scan(MbUiFacade *facade, GError **error) {
    return mb_ui_facade_scan_devices(facade, 8, error);
}


bool mb_ui_facade_save_device_config(MbUiFacade *facade,
                                     const MbUiDevice *device,
                                     GError **error) {
    (void)facade;

    if (!device || !device->id || !device->address) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid device for persistence");
        return false;
    }

    char *path = device_config_path(device, error);
    if (!path)
        return false;

    char *safe_id = sanitize_device_id(device->id);
    char *alsa_name = g_strdup_printf("%s BLE-MIDI", safe_str(device->name, safe_id));

    char *content = g_strdup_printf(
        "[device]\n"
        "id = %s\n"
        "enabled = yes\n"
        "address = %s\n"
        "name = %s\n"
        "profile = standard_ble_midi\n"
        "connect_on_start = yes\n"
        "alsa_port_name = %s\n"
        "\n"
        "[policy]\n"
        "pair = no\n"
        "trust = yes\n"
        "reconnect_on_link_loss = yes\n"
        "\n"
        "[midi]\n"
        "enable_tx = yes\n",
        safe_id,
        safe_str(device->address, ""),
        safe_str(device->name, safe_id),
        alsa_name);

    bool ok = g_file_set_contents(path, content, -1, error);

    g_free(content);
    g_free(alsa_name);
    g_free(safe_id);
    g_free(path);

    return ok;
}

static bool run_device_command(MbUiFacade *facade,
                               const char *command,
                               const char *device_id,
                               GError **error) {
    (void)facade;

    const char *method = NULL;

    if (g_strcmp0(command, "daemon-connect") == 0)
        method = "ConnectDevice";
    else if (g_strcmp0(command, "daemon-disconnect") == 0)
        method = "DisconnectDevice";
    else if (g_strcmp0(command, "daemon-recheck") == 0)
        method = "RecheckDevice";
    else {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "legacy GUI command removed: %s",
                    command ? command : "-");
        return false;
    }

    return facade_daemon_dbus_device_command(method, device_id, error);
}

bool mb_ui_facade_connect(MbUiFacade *facade,
                          const char *device_id,
                          GError **error) {
    return run_device_command(facade, "daemon-connect", device_id, error);
}

bool mb_ui_facade_disconnect(MbUiFacade *facade,
                             const char *device_id,
                             GError **error) {
    return run_device_command(facade, "daemon-disconnect", device_id, error);
}

bool mb_ui_facade_refresh_device(MbUiFacade *facade,
                                 const char *device_id,
                                 GError **error) {
    return run_device_command(facade, "daemon-recheck", device_id, error);
}

bool mb_ui_facade_connect_with_config(MbUiFacade *facade,
                                      const char *device_id,
                                      const MbUiDeviceConfig *config,
                                      GError **error) {
    (void)config;

    /*
     * The GUI no longer writes or infers daemon configuration here.
     * Connect is a daemon D-Bus command; profile, BlueZ and config policy live
     * in midi-ble-rtd.
     */
    return run_device_command(facade, "daemon-connect", device_id, error);
}


static bool try_bluez_forget_selector(MbUiFacade *facade,
                                      const char *selector,
                                      GError **last_error) {
    if (!selector || !*selector)
        return false;

    g_printerr("[midi-ble-rt-gui] forget: trying BlueZ RemoveDevice selector: %s\n",
               selector);

    GError *error = NULL;
    char *ignored = run_ctl(facade, "forget", selector, "--yes", &error);
    g_free(ignored);

    if (!error)
        return true;

    g_printerr("[midi-ble-rt-gui] forget: selector failed: %s: %s\n",
               selector,
               error->message ? error->message : "unknown error");

    if (last_error) {
        g_clear_error(last_error);
        *last_error = error;
    } else {
        g_clear_error(&error);
    }

    return false;
}

static bool bluez_forget_device(MbUiFacade *facade,
                                const MbUiDevice *device,
                                GError **error) {
    GError *last_error = NULL;

    /*
     * Prefer the Bluetooth address because midi-ble-rtctl documents address
     * as the stable selector. Fall back to session id and visible name because
     * the GUI snapshot may come from daemon state while BlueZ lookup uses its
     * own object cache.
     */
    if (try_bluez_forget_selector(facade, device->address, &last_error)) {
        g_clear_error(&last_error);
        return true;
    }

    if (try_bluez_forget_selector(facade, device->id, &last_error)) {
        g_clear_error(&last_error);
        return true;
    }

    if (try_bluez_forget_selector(facade, device->name, &last_error)) {
        g_clear_error(&last_error);
        return true;
    }

    if (last_error)
        g_propagate_prefixed_error(error, last_error, "BlueZ forget failed: ");
    else
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "BlueZ forget failed: no usable selector");

    return false;
}

static bool key_file_matches_device(const char *path, const MbUiDevice *device) {
    if (!path || !device)
        return false;

    GKeyFile *key_file = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_key_file_unref(key_file);
        return false;
    }

    char *id = g_key_file_get_string(key_file, "device", "id", NULL);
    char *address = g_key_file_get_string(key_file, "device", "address", NULL);

    bool match = false;
    if (id && device->id && g_ascii_strcasecmp(id, device->id) == 0)
        match = true;
    if (address && device->address && g_ascii_strcasecmp(address, device->address) == 0)
        match = true;

    g_free(id);
    g_free(address);
    g_key_file_unref(key_file);
    return match;
}

static bool remove_matching_device_configs(const MbUiDevice *device, GError **error) {
    char *dir = devices_config_dir();

    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        g_free(dir);
        return true;
    }

    GDir *gdir = g_dir_open(dir, 0, error);
    if (!gdir) {
        g_free(dir);
        return false;
    }

    bool ok = true;
    const char *name = NULL;

    while ((name = g_dir_read_name(gdir)) != NULL) {
        if (!g_str_has_suffix(name, ".ini"))
            continue;

        char *path = g_build_filename(dir, name, NULL);
        if (key_file_matches_device(path, device)) {
            if (g_unlink(path) != 0) {
                g_set_error(error,
                            G_IO_ERROR,
                            g_io_error_from_errno(errno),
                            "failed to remove device configuration: %s",
                            path);
                ok = false;
                g_free(path);
                break;
            }
        }
        g_free(path);
    }

    g_dir_close(gdir);
    g_free(dir);
    return ok;
}

bool mb_ui_facade_forget_device(MbUiFacade *facade,
                                const char *device_id,
                                GError **error) {
    if (!device_id || !*device_id) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "no device selected");
        return false;
    }

    MbUiSnapshot *snapshot = mb_ui_facade_get_snapshot(facade);
    const MbUiDevice *device = mb_ui_snapshot_find_device(snapshot, device_id);

    if (!device) {
        mb_ui_snapshot_free(snapshot);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "device is not visible in current service snapshot");
        return false;
    }

    char *id_copy = g_strdup(device->id);

    /*
     * Important order:
     *
     * 1. Ask BlueZ to forget while the object is still most likely present.
     *    RemoveDevice itself may disconnect the device.
     * 2. Ask the daemon to disconnect as best effort.
     * 3. Remove local midi-ble-rt device config.
     * 4. Ask daemon to recheck as best effort.
     *
     * The GUI must mirror backend/BlueZ state, not hide devices locally.
     */

    ui_log_backend_step("forget: BlueZ RemoveDevice via midi-ble-rtctl forget --yes");

    if (!bluez_forget_device(facade, device, error)) {
        g_free(id_copy);
        mb_ui_snapshot_free(snapshot);
        return false;
    }

    ui_log_backend_step("forget: daemon-disconnect");

    GError *disconnect_error = NULL;
    char *ignored = run_ctl(facade, "daemon-disconnect", id_copy, NULL, &disconnect_error);
    g_free(ignored);
    if (disconnect_error) {
        ui_log_backend_error("forget: daemon-disconnect", disconnect_error);
        g_clear_error(&disconnect_error);
    }

    ui_log_backend_step("forget: remove local device configuration");

    if (!remove_matching_device_configs(device, error)) {
        g_free(id_copy);
        mb_ui_snapshot_free(snapshot);
        return false;
    }

    ui_log_backend_step("forget: daemon-recheck");

    GError *recheck_error = NULL;
    ignored = run_ctl(facade, "daemon-recheck", id_copy, NULL, &recheck_error);
    g_free(ignored);
    if (recheck_error) {
        ui_log_backend_error("forget: daemon-recheck", recheck_error);
        g_clear_error(&recheck_error);
    }

    g_free(id_copy);
    mb_ui_snapshot_free(snapshot);
    return true;
}


bool mb_ui_facade_remove_imported_device(MbUiFacade *facade,
                                        const char *device_id,
                                        GError **error) {
    if (!device_id || !*device_id) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "No device selected");
        return false;
    }

    char *out = run_ctl(facade, "remove-instrument", device_id, "--yes", error);
    if (!out)
        return false;

    g_printerr("[midi-ble-rt-gui] remove imported device output:\n%s", out);
    g_free(out);
    return true;
}

bool mb_ui_facade_import_scanned_device(MbUiFacade *facade,
                                        const char *device_id,
                                        GError **error) {
    (void)facade;
    (void)device_id;

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                "import via GUI requires daemon D-Bus ImportDevice support");
    return false;
}


bool mb_ui_facade_pair_scanned_device(MbUiFacade *facade,
                                      const char *device_id,
                                      GError **error) {
    (void)facade;
    (void)device_id;

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                "pair/import via GUI requires daemon D-Bus PairAndImportDevice support");
    return false;
}


