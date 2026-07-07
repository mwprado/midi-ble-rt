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


static char *scan_field_dup(const char *line, size_t start, size_t end) {
    size_t len = strlen(line);
    if (start >= len)
        return g_strdup("");

    if (end > len)
        end = len;

    while (start < end && g_ascii_isspace(line[start]))
        start++;
    while (end > start && g_ascii_isspace(line[end - 1]))
        end--;

    return g_strndup(line + start, end - start);
}


static bool scan_bool_value(const char *value) {
    return value &&
           (g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "1") == 0);
}

static bool looks_like_bt_address(const char *s) {
    if (!s || strlen(s) < 17)
        return false;

    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (s[i] != ':')
                return false;
        } else if (!g_ascii_isxdigit(s[i])) {
            return false;
        }
    }

    return true;
}

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

static void parse_scan_output_into_cache(MbUiFacade *facade, const char *text) {
    if (!facade || !facade->scan_devices)
        return;

    g_ptr_array_set_size(facade->scan_devices, 0);

    if (!text || !*text)
        return;

    char **lines = g_strsplit(text, "\n", -1);

    size_t name_start = 0;
    size_t alias_start = 0;
    size_t paired_start = 0;
    size_t trusted_start = 0;
    size_t connected_start = 0;
    size_t gatt_start = 0;
    size_t uuid_start = 0;
    size_t profile_start = 0;
    bool have_header = false;

    for (guint i = 0; lines && lines[i]; i++) {
        const char *line = lines[i];

        if (g_str_has_prefix(line, "ADDRESS")) {
            const char *p_name = strstr(line, "NAME");
            const char *p_alias = strstr(line, "ALIAS");
            const char *p_paired = strstr(line, "PAIRED");
            const char *p_trusted = strstr(line, "TRUSTED");
            const char *p_connected = strstr(line, "CONNECTED");
            const char *p_gatt = strstr(line, "GATT");
            const char *p_uuid = strstr(line, "UUID-HINT");
            const char *p_profile = strstr(line, "PROFILE");

            if (p_name && p_alias && p_paired && p_trusted &&
                p_connected && p_gatt && p_uuid && p_profile) {
                name_start = (size_t)(p_name - line);
                alias_start = (size_t)(p_alias - line);
                paired_start = (size_t)(p_paired - line);
                trusted_start = (size_t)(p_trusted - line);
                connected_start = (size_t)(p_connected - line);
                gatt_start = (size_t)(p_gatt - line);
                uuid_start = (size_t)(p_uuid - line);
                profile_start = (size_t)(p_profile - line);
                have_header = true;
            }

            continue;
        }

        if (!have_header)
            continue;

        if (!looks_like_bt_address(line))
            continue;

        char *address = g_strndup(line, 17);
        char *name = scan_field_dup(line, name_start, alias_start);
        char *paired = scan_field_dup(line, paired_start, trusted_start);
        char *trusted = scan_field_dup(line, trusted_start, connected_start);
        char *connected = scan_field_dup(line, connected_start, gatt_start);
        char *gatt = scan_field_dup(line, gatt_start, uuid_start);
        char *uuid_hint = scan_field_dup(line, uuid_start, profile_start);
        char *profile = scan_field_dup(line, profile_start, strlen(line));

        if (!name || !*name) {
            g_free(name);
            name = g_strdup(address);
        }

        bool is_midi = false;
        if (uuid_hint && g_strrstr(uuid_hint, "ble-midi"))
            is_midi = true;
        if (profile && g_strcmp0(profile, "-") != 0 && *profile)
            is_midi = true;

        if (is_midi) {
            char *id = scan_device_id_from_name_address(name, address);
            MbUiDevice *device = mb_ui_device_new(id,
                                                  address,
                                                  name,
                                                  "IDLE",
                                                  -1);
            device->paired = scan_bool_value(paired);
            device->trusted = scan_bool_value(trusted);
            device->connected = scan_bool_value(connected);
            device->gatt_resolved = scan_bool_value(gatt);
            device->profile = g_strdup(profile && g_strcmp0(profile, "-") != 0
                                       ? profile
                                       : "standard_ble_midi");
            g_ptr_array_add(facade->scan_devices, device);
            g_free(id);
        }

        g_free(profile);
        g_free(uuid_hint);
        g_free(gatt);
        g_free(connected);
        g_free(trusted);
        g_free(paired);
        g_free(name);
        g_free(address);
    }

    g_strfreev(lines);
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



static bool run_ctl_argv_checked(MbUiFacade *facade,
                                 const char *context,
                                 char **argv,
                                 GError **error) {
    (void)facade;

    char *stdout_text = NULL;
    char *stderr_text = NULL;
    int wait_status = 0;

    if (!argv || !argv[0]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid ctl argv");
        return false;
    }

    g_printerr("[midi-ble-rt-gui] %s:", context ? context : "midi-ble-rtctl");
    for (guint i = 0; argv[i]; i++)
        g_printerr(" %s", argv[i]);
    g_printerr("\n");

    gboolean ok = g_spawn_sync(NULL,
                               argv,
                               NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL,
                               NULL,
                               &stdout_text,
                               &stderr_text,
                               &wait_status,
                               error);

    if (!ok) {
        if (error && *error)
            ui_log_backend_error(context ? context : "midi-ble-rtctl", *error);
        g_free(stdout_text);
        g_free(stderr_text);
        return false;
    }

    if (!g_spawn_check_wait_status(wait_status, error)) {
        if (stderr_text && *stderr_text) {
            g_strchomp(stderr_text);
            g_printerr("[midi-ble-rt-gui] %s stderr: %s\n",
                       context ? context : "midi-ble-rtctl",
                       stderr_text);
            if (error && *error)
                g_prefix_error(error, "%s: ", stderr_text);
        }

        if (stdout_text && *stdout_text) {
            g_strchomp(stdout_text);
            g_printerr("[midi-ble-rt-gui] %s output:\n%s\n",
                       context ? context : "midi-ble-rtctl",
                       stdout_text);
        }

        if (error && *error)
            ui_log_backend_error(context ? context : "midi-ble-rtctl", *error);

        g_free(stdout_text);
        g_free(stderr_text);
        return false;
    }

    if (stdout_text && *stdout_text) {
        g_strchomp(stdout_text);
        g_printerr("[midi-ble-rt-gui] %s output:\n%s\n",
                   context ? context : "midi-ble-rtctl",
                   stdout_text);
    }

    if (stderr_text && *stderr_text) {
        g_strchomp(stderr_text);
        g_printerr("[midi-ble-rt-gui] %s stderr:\n%s\n",
                   context ? context : "midi-ble-rtctl",
                   stderr_text);
    }

    g_free(stdout_text);
    g_free(stderr_text);
    return true;
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

static void overlay_bluez_pairing_state(MbUiSnapshot *catalog) {
    if (!catalog || !catalog->devices || catalog->devices->len == 0)
        return;

    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        g_printerr("[midi-ble-rt-gui] BlueZ catalog overlay skipped: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    GVariant *reply = g_dbus_connection_call_sync(connection,
                                                  "org.bluez",
                                                  "/",
                                                  "org.freedesktop.DBus.ObjectManager",
                                                  "GetManagedObjects",
                                                  NULL,
                                                  G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  &error);
    if (!reply) {
        g_printerr("[midi-ble-rt-gui] BlueZ catalog overlay skipped: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(connection);
        return;
    }

    GVariant *objects = NULL;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter object_iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;

    g_variant_iter_init(&object_iter, objects);
    while (g_variant_iter_next(&object_iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces,
                                                  "org.bluez.Device1",
                                                  G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        char *address = facade_variant_dict_dup_string(props, "Address");
        if (!address || !*address) {
            g_free(address);
            g_variant_unref(props);
            g_variant_unref(ifaces);
            continue;
        }

        bool paired = false;
        bool connected = false;
        bool trusted = false;

        bool have_paired = facade_variant_dict_lookup_bool(props, "Paired", &paired);
        bool have_connected = facade_variant_dict_lookup_bool(props, "Connected", &connected);
        bool have_trusted = facade_variant_dict_lookup_bool(props, "Trusted", &trusted);

        for (guint i = 0; i < catalog->devices->len; i++) {
            MbUiDevice *device = g_ptr_array_index(catalog->devices, i);
            if (!device || !device->address)
                continue;

            if (g_ascii_strcasecmp(device->address, address) != 0)
                continue;

            if (have_paired)
                device->paired = paired;
            if (have_connected)
                device->connected = connected;
            if (have_trusted)
                device->trusted = trusted;

            /*
             * BlueZ overlay only expresses external Bluetooth state.
             * Daemon overlay may later replace this with STREAMING/ERROR/etc.
             */
            if (have_connected && connected) {
                g_free(device->state);
                device->state = g_strdup("CONNECTED");
            } else if (have_paired && !paired) {
                g_free(device->state);
                device->state = g_strdup("UNPAIRED");
                device->alsa_port = -1;
            } else if (have_paired && paired) {
                g_free(device->state);
                device->state = g_strdup("DISCONNECTED");
            }

            g_printerr("[midi-ble-rt-gui] catalog BlueZ overlay: address=%s paired=%d connected=%d trusted=%d path=%s\n",
                       address,
                       device->paired,
                       device->connected,
                       device->trusted,
                       path ? path : "-");
            break;
        }

        g_free(address);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    g_variant_unref(reply);
    g_object_unref(connection);
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
    if (!facade) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid UI facade");
        return false;
    }

    if (timeout_seconds < 1)
        timeout_seconds = 1;
    if (timeout_seconds > 60)
        timeout_seconds = 60;

    char timeout_buf[16];
    g_snprintf(timeout_buf, sizeof(timeout_buf), "%u", timeout_seconds);

    g_printerr("[midi-ble-rt-gui] scan: %s scan --timeout %s --midi-only\n",
               facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
               timeout_buf);

    char *argv[] = {
        facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
        "scan",
        "--timeout",
        timeout_buf,
        "--midi-only",
        NULL,
    };

    char *stdout_text = NULL;
    char *stderr_text = NULL;
    int wait_status = 0;

    gboolean ok = g_spawn_sync(NULL,
                               argv,
                               NULL,
                               G_SPAWN_SEARCH_PATH,
                               NULL,
                               NULL,
                               &stdout_text,
                               &stderr_text,
                               &wait_status,
                               error);

    if (!ok) {
        if (error && *error)
            ui_log_backend_error("scan", *error);
        g_free(stdout_text);
        g_free(stderr_text);
        return false;
    }

    if (!g_spawn_check_wait_status(wait_status, error)) {
        if (stderr_text && *stderr_text) {
            g_strchomp(stderr_text);
            g_printerr("[midi-ble-rt-gui] scan stderr: %s\n", stderr_text);

            if (error && *error) {
                g_prefix_error(error, "%s: ", stderr_text);
            }
        }

        if (error && *error)
            ui_log_backend_error("scan", *error);

        g_free(stdout_text);
        g_free(stderr_text);
        return false;
    }

    if (stdout_text && *stdout_text) {
        g_strchomp(stdout_text);
        g_printerr("[midi-ble-rt-gui] scan output:\n%s\n", stdout_text);
        parse_scan_output_into_cache(facade, stdout_text);
    }

    if (stderr_text && *stderr_text) {
        g_strchomp(stderr_text);
        g_printerr("[midi-ble-rt-gui] scan stderr:\n%s\n", stderr_text);
    }

    g_free(stdout_text);
    g_free(stderr_text);
    return true;
}


MbUiSnapshot *mb_ui_facade_get_snapshot(MbUiFacade *facade) {
    (void)facade;

    MbUiSnapshot *snapshot = mb_ui_snapshot_new();

    /*
     * Main window invariant:
     *
     * The sidebar is the persisted instrument catalog.
     * Rows come only from valid .ini files under devices.d.
     *
     * BlueZ and daemon state are overlays.  They may update paired,
     * connected, ALSA port and runtime state, but they must never create,
     * hide or clear catalog rows.
     */
    load_imported_device_configs(snapshot);

    /*
     * Opening the window must not start discovery.  This reads BlueZ Device1
     * objects already known by bluetoothd and updates paired/connected state.
     */
    overlay_bluez_pairing_state(snapshot);

    /*
     * Do not block the main catalog on the daemon control socket.
     *
     * The window already observes systemd for daemon availability.  The
     * persisted catalog and BlueZ pairing/connection state are enough to render
     * the main list.  Runtime daemon state may be overlaid later, but it must
     * never prevent catalog rows from appearing.
     */
    snapshot->status.online = false;
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



static bool config_file_mentions_address(const char *path, const char *address) {
    if (!path || !*path || !address || !*address)
        return false;

    char *contents = NULL;
    gsize len = 0;

    if (!g_file_get_contents(path, &contents, &len, NULL))
        return false;

    char *contents_down = g_ascii_strdown(contents, -1);
    char *address_down = g_ascii_strdown(address, -1);

    bool found = contents_down && address_down && strstr(contents_down, address_down) != NULL;

    g_free(address_down);
    g_free(contents_down);
    g_free(contents);

    return found;
}

static bool config_dir_has_imported_address(const char *dir_path, const char *address) {
    if (!dir_path || !*dir_path || !address || !*address)
        return false;

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return false;

    bool found = false;
    const char *name = NULL;

    while (!found && (name = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(name, ".ini"))
            continue;

        char *path = g_build_filename(dir_path, name, NULL);
        found = config_file_mentions_address(path, address);
        g_free(path);
    }

    g_dir_close(dir);
    return found;
}

static bool imported_config_exists_for_address(const char *address) {
    if (!address || !*address)
        return false;

    const char *base = g_get_user_config_dir();

    char *devices_dir = g_build_filename(base, "midi-ble-rt", "devices.d", NULL);
    bool found = config_dir_has_imported_address(devices_dir, address);
    g_free(devices_dir);

    if (found)
        return true;

    char *legacy_dir = g_build_filename(base, "midi-ble-rt", NULL);
    found = config_dir_has_imported_address(legacy_dir, address);
    g_free(legacy_dir);

    return found;
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
    char *out = run_ctl(facade, "daemon-list", NULL, NULL, error);
    if (!out)
        return false;

    g_free(out);
    return true;
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
    if (!device_id || !*device_id) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "no device selected");
        return false;
    }

    char *out = run_ctl(facade, command, device_id, NULL, error);
    if (!out)
        return false;

    bool ok = g_str_has_prefix(out, "OK ");
    if (!ok) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "%s failed: %s",
                    command,
                    out);
    }

    g_free(out);
    return ok;
}

bool mb_ui_facade_connect(MbUiFacade *facade,
                          const char *device_id,
                          GError **error) {
    MbUiSnapshot *snapshot = mb_ui_facade_get_scan_snapshot(facade);
    const MbUiDevice *device = mb_ui_snapshot_find_device(snapshot, device_id);

    if (device) {
        if (!mb_ui_facade_save_device_config(facade, device, error)) {
            mb_ui_snapshot_free(snapshot);
            return false;
        }

        GError *recheck_error = NULL;
        char *ignored = run_ctl(facade, "daemon-recheck", device->id, NULL, &recheck_error);
        g_free(ignored);
        g_clear_error(&recheck_error);
    }

    mb_ui_snapshot_free(snapshot);
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

static bool save_device_config_with_options(MbUiFacade *facade,
                                            const MbUiDevice *device,
                                            const MbUiDeviceConfig *options,
                                            GError **error) {
    (void)facade;

    if (!device || !device->id || !device->address) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid device for persistence");
        return false;
    }

    MbUiDeviceConfig defaults;
    mb_ui_device_config_defaults(&defaults);
    const MbUiDeviceConfig *config = options ? options : &defaults;

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
        "connect_on_start = %s\n"
        "alsa_port_name = %s\n"
        "\n"
        "[policy]\n"
        "pair = %s\n"
        "trust = %s\n"
        "reconnect_on_link_loss = %s\n"
        "\n"
        "[midi]\n"
        "enable_tx = %s\n",
        safe_id,
        safe_str(device->address, ""),
        safe_str(device->name, safe_id),
        config->connect_on_start ? "yes" : "no",
        alsa_name,
        config->pair ? "yes" : "no",
        config->trust ? "yes" : "no",
        config->reconnect_on_link_loss ? "yes" : "no",
        config->enable_tx ? "yes" : "no");

    bool ok = g_file_set_contents(path, content, -1, error);

    g_free(content);
    g_free(alsa_name);
    g_free(safe_id);
    g_free(path);

    return ok;
}


static bool mb_ui_device_is_daemon_session_for_gui(const MbUiDevice *device) {
    /*
     * Dispositivos vindos apenas do scan cache entram com alsa_port = -1.
     * Sessões reais do daemon têm porta ALSA >= 0.
     */
    return device && device->alsa_port >= 0;
}

static const char *mb_ui_profile_for_device_for_gui(const MbUiDevice *device) {
    const char *name = device && device->name ? device->name : "";

    if (g_strrstr(name, "GO:KEYS") ||
        g_strrstr(name, "GO KEYS") ||
        g_strrstr(name, "GOKEYS"))
        return "roland_gokeys";

    return "standard_ble_midi";
}

static char *mb_ui_find_daemon_session_id_by_address_for_gui(MbUiSnapshot *snapshot,
                                                             const char *address) {
    if (!snapshot || !snapshot->devices || !address || !*address)
        return NULL;

    for (guint i = 0; i < snapshot->devices->len; i++) {
        const MbUiDevice *device = g_ptr_array_index(snapshot->devices, i);

        if (!device || !device->address || !device->id)
            continue;

        if (g_ascii_strcasecmp(device->address, address) != 0)
            continue;

        if (!mb_ui_device_is_daemon_session_for_gui(device))
            continue;

        return g_strdup(device->id);
    }

    return NULL;
}


typedef struct {
    char *id;
    char *address;
    char *name;
    char *profile;
} PairEnrollDevice;

static void pair_enroll_device_clear(PairEnrollDevice *device) {
    if (!device)
        return;

    g_clear_pointer(&device->id, g_free);
    g_clear_pointer(&device->address, g_free);
    g_clear_pointer(&device->name, g_free);
    g_clear_pointer(&device->profile, g_free);
}

static const char *pair_profile_for_device_name(const char *name) {
    if (name &&
        (g_strrstr(name, "GO:KEYS") ||
         g_strrstr(name, "GO KEYS") ||
         g_strrstr(name, "GOKEYS")))
        return "roland_gokeys";

    return "standard_ble_midi";
}

static bool capture_pair_enroll_device(MbUiFacade *facade,
                                       const char *device_id,
                                       PairEnrollDevice *out,
                                       GError **error) {
    if (!facade || !device_id || !*device_id || !out) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid discovery enrollment request");
        return false;
    }

    MbUiSnapshot *snapshot = mb_ui_facade_get_scan_snapshot(facade);
    const MbUiDevice *device = mb_ui_snapshot_find_device(snapshot, device_id);

    if (!device) {
        mb_ui_snapshot_free(snapshot);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "device is not visible in current discovery result");
        return false;
    }

    out->id = g_strdup(device->id);
    out->address = g_strdup(device->address && *device->address ? device->address : device->id);
    out->name = g_strdup(device->name ? device->name : "");
    out->profile = g_strdup(device->profile && *device->profile
                            ? device->profile
                            : pair_profile_for_device_name(device->name));

    mb_ui_snapshot_free(snapshot);
    return true;
}


bool mb_ui_facade_connect_with_config(MbUiFacade *facade,
                                      const char *device_id,
                                      const MbUiDeviceConfig *config,
                                      GError **error) {
    if (!facade || !device_id || !*device_id) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid connect request");
        return false;
    }

    MbUiSnapshot *snapshot = mb_ui_facade_get_snapshot(facade);
    const MbUiDevice *device = mb_ui_snapshot_find_device(snapshot, device_id);

    if (!device) {
        mb_ui_snapshot_free(snapshot);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "device is not visible in current snapshot");
        return false;
    }

    if (!snapshot->status.online) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "Serviço MIDI-BLE inativo. Inicie midi-ble-rtd com runtime de diretório de configuração antes de conectar.");
        mb_ui_snapshot_free(snapshot);
        return false;
    }

    char *id_copy = g_strdup(device->id);
    char *address_copy = g_strdup(device->address && *device->address ? device->address : device->id);
    char *name_copy = g_strdup(device->name);
    bool was_daemon_session = mb_ui_device_is_daemon_session_for_gui(device);
    const char *profile = mb_ui_profile_for_device_for_gui(device);

    /*
     * Dispositivo vindo do scan cache ainda não é sessão do daemon.
     * Primeiro preparar no BlueZ e escrever configuração via ctl.
     */
    if (!was_daemon_session) {
        char *argv[] = {
            facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
            "connect",
            address_copy,
            "--profile",
            (char *)profile,
            "--write-config",
            NULL,
        };

        if (!run_ctl_argv_checked(facade, "connect scanned device", argv, error)) {
            g_free(id_copy);
            g_free(address_copy);
            g_free(name_copy);
            mb_ui_snapshot_free(snapshot);
            return false;
        }
    } else {
        if (!save_device_config_with_options(facade, device, config, error)) {
            g_free(id_copy);
            g_free(address_copy);
            g_free(name_copy);
            mb_ui_snapshot_free(snapshot);
            return false;
        }
    }

    /*
     * Recarrega configuração no daemon. Pode ser assíncrono do lado do daemon,
     * mas o comando deve pelo menos disparar o recheck.
     */
    GError *recheck_error = NULL;
    char *ignored = run_ctl(facade, "daemon-recheck", id_copy, NULL, &recheck_error);
    g_free(ignored);
    if (recheck_error) {
        ui_log_backend_error("connect: daemon-recheck by original id", recheck_error);
        g_clear_error(&recheck_error);
    }

    /*
     * Agora procurar a sessão real por endereço. O id de scan pode ser
     * "go-keys-midi-1", enquanto a config/daemon pode criar "roland-gokeys"
     * ou outro id normalizado.
     */
    char *daemon_session_id = NULL;

    MbUiSnapshot *after_recheck = mb_ui_facade_get_snapshot(facade);
    daemon_session_id = mb_ui_find_daemon_session_id_by_address_for_gui(after_recheck, address_copy);
    mb_ui_snapshot_free(after_recheck);

    if (!daemon_session_id) {
        /*
         * Fallbacks diagnósticos. Se estes falharem, o log dirá exatamente
         * qual id foi tentado.
         */
        daemon_session_id = g_strdup(id_copy);
    }

    g_printerr("[midi-ble-rt-gui] connect: daemon-connect session id: %s address=%s name=%s\n",
               daemon_session_id,
               address_copy ? address_copy : "-",
               name_copy ? name_copy : "-");

    GError *connect_error = NULL;
    bool ok = run_device_command(facade, "daemon-connect", daemon_session_id, &connect_error);

    if (!ok) {
        ui_log_backend_error("connect: daemon-connect", connect_error);

        if (connect_error)
            g_propagate_error(error, connect_error);
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "daemon-connect failed");

        g_free(daemon_session_id);
        g_free(id_copy);
        g_free(address_copy);
        g_free(name_copy);
        mb_ui_snapshot_free(snapshot);
        return false;
    }

    g_free(daemon_session_id);
    g_free(id_copy);
    g_free(address_copy);
    g_free(name_copy);
    mb_ui_snapshot_free(snapshot);
    return true;
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
    PairEnrollDevice device = {0};

    if (!capture_pair_enroll_device(facade, device_id, &device, error))
        return false;

    /*
     * Import means: BlueZ already knows/pairs the device.
     * Do not Pair() here. Only trust + configure when no local config exists.
     */
    char *trust_argv[] = {
        facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
        "trust",
        device.address,
        NULL,
    };

    if (!run_ctl_argv_checked(facade, "trust imported device", trust_argv, error)) {
        pair_enroll_device_clear(&device);
        return false;
    }

    if (!imported_config_exists_for_address(device.address)) {
        char *configure_argv[] = {
            facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
            "configure",
            device.address,
            "--profile",
            device.profile,
            NULL,
        };

        if (!run_ctl_argv_checked(facade, "configure imported device", configure_argv, error)) {
            pair_enroll_device_clear(&device);
            return false;
        }
    } else {
        g_printerr("[midi-ble-rt-gui] import: local config already exists for %s; not rewriting\n",
                   device.address ? device.address : "-");
    }

    GError *recheck_error = NULL;
    char *ignored = run_ctl(facade, "daemon-recheck", device.address, NULL, &recheck_error);
    g_free(ignored);

    if (recheck_error) {
        g_printerr("[midi-ble-rt-gui] import: daemon-recheck skipped/failed: %s\n",
                   recheck_error->message ? recheck_error->message : "unknown error");
        g_clear_error(&recheck_error);
    }

    g_printerr("[midi-ble-rt-gui] import: enrolled %s address=%s profile=%s\n",
               device.name ? device.name : "-",
               device.address ? device.address : "-",
               device.profile ? device.profile : "-");

    pair_enroll_device_clear(&device);
    return true;
}


bool mb_ui_facade_pair_scanned_device(MbUiFacade *facade,
                                      const char *device_id,
                                      GError **error) {
    PairEnrollDevice device = {0};

    if (!capture_pair_enroll_device(facade, device_id, &device, error))
        return false;

    char *pair_argv[] = {
        facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
        "pair",
        device.address,
        NULL,
    };

    if (!run_ctl_argv_checked(facade, "pair scanned device", pair_argv, error)) {
        pair_enroll_device_clear(&device);
        return false;
    }

    char *trust_argv[] = {
        facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
        "trust",
        device.address,
        NULL,
    };

    if (!run_ctl_argv_checked(facade, "trust paired device", trust_argv, error)) {
        pair_enroll_device_clear(&device);
        return false;
    }

    if (!imported_config_exists_for_address(device.address)) {
        char *configure_argv[] = {
            facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
            "configure",
            device.address,
            "--profile",
            device.profile,
            NULL,
        };

        if (!run_ctl_argv_checked(facade, "configure paired device", configure_argv, error)) {
            pair_enroll_device_clear(&device);
            return false;
        }
    } else {
        g_printerr("[midi-ble-rt-gui] pair: local config already exists for %s; not rewriting\n",
                   device.address ? device.address : "-");
    }

    GError *recheck_error = NULL;
    char *ignored = run_ctl(facade, "daemon-recheck", device.address, NULL, &recheck_error);
    g_free(ignored);

    if (recheck_error) {
        g_printerr("[midi-ble-rt-gui] pair: daemon-recheck skipped/failed: %s\n",
                   recheck_error->message ? recheck_error->message : "unknown error");
        g_clear_error(&recheck_error);
    }

    g_printerr("[midi-ble-rt-gui] pair: enrolled %s address=%s profile=%s\n",
               device.name ? device.name : "-",
               device.address ? device.address : "-",
               device.profile ? device.profile : "-");

    pair_enroll_device_clear(&device);
    return true;
}


