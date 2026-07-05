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

        if (id && device->id && g_ascii_strcasecmp(id, device->id) == 0)
            return true;

        if (address && device->address && g_ascii_strcasecmp(address, device->address) == 0)
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
    size_t uuid_start = 0;
    size_t profile_start = 0;
    bool have_header = false;

    for (guint i = 0; lines && lines[i]; i++) {
        const char *line = lines[i];

        if (g_str_has_prefix(line, "ADDRESS")) {
            const char *p_name = strstr(line, "NAME");
            const char *p_alias = strstr(line, "ALIAS");
            const char *p_uuid = strstr(line, "UUID-HINT");
            const char *p_profile = strstr(line, "PROFILE");

            if (p_name && p_alias && p_uuid && p_profile) {
                name_start = (size_t)(p_name - line);
                alias_start = (size_t)(p_alias - line);
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
                                                  name,
                                                  address,
                                                  "IDLE",
                                                  -1);
            g_ptr_array_add(facade->scan_devices, device);
            g_free(id);
        }

        g_free(profile);
        g_free(uuid_hint);
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
                                            safe_str(scan_device->name, "Dispositivo BLE-MIDI"),
                                            safe_str(scan_device->address, "-"),
                                            "IDLE",
                                            -1);
        g_ptr_array_add(snapshot->devices, copy);
    }
}

static char *devices_config_dir(void) {
    return g_build_filename(g_get_user_config_dir(),
                            "midi-ble-rt",
                            "devices.d",
                            NULL);
}


static char *dup_field_value(const char *line, const char *key) {
    if (!line || !key)
        return NULL;

    char *needle = g_strdup_printf("%s=", key);
    char *p = strstr(line, needle);
    g_free(needle);

    if (!p)
        return NULL;

    p = strchr(p, '=');
    if (!p)
        return NULL;
    p++;

    const char *end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r')
        end++;

    return g_strndup(p, (gsize)(end - p));
}

static unsigned field_uint(const char *line, const char *key) {
    char *value = dup_field_value(line, key);
    if (!value)
        return 0;

    unsigned out = (unsigned)g_ascii_strtoull(value, NULL, 10);
    g_free(value);
    return out;
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

static void parse_status_line(MbUiSnapshot *snapshot, const char *line) {
    if (!snapshot || !line)
        return;

    snapshot->status.online = g_str_has_prefix(line, "OK STATUS");
    snapshot->status.devices = field_uint(line, "devices");
    snapshot->status.streaming = field_uint(line, "streaming");
    snapshot->status.rx_workers = field_uint(line, "rx_workers");
    snapshot->status.tx_workers = field_uint(line, "tx_workers");
    snapshot->status.rtkit_priority = field_uint(line, "rtkit_priority");
    snapshot->status.lifecycle_queue = field_uint(line, "lifecycle_queue");

    g_free(snapshot->status.alsa_tx_thread);
    g_free(snapshot->status.rtkit);
    g_free(snapshot->status.rtkit_rx);
    g_free(snapshot->status.rtkit_tx);
    g_free(snapshot->status.lifecycle_busy);

    snapshot->status.alsa_tx_thread = dup_field_value(line, "alsa_tx_thread");
    snapshot->status.rtkit = dup_field_value(line, "rtkit");
    snapshot->status.rtkit_rx = dup_field_value(line, "rtkit_rx");
    snapshot->status.rtkit_tx = dup_field_value(line, "rtkit_tx");
    snapshot->status.lifecycle_busy = dup_field_value(line, "lifecycle_busy");
}

static void parse_list_output(MbUiSnapshot *snapshot, const char *text) {
    if (!snapshot || !text)
        return;

    char **lines = g_strsplit(text, "\n", 0);
    bool in_table = false;

    for (gsize i = 0; lines && lines[i]; i++) {
        char *line = lines[i];
        g_strstrip(line);

        if (*line == '\0')
            continue;

        if (g_strcmp0(line, "OK LIST") == 0) {
            in_table = true;
            continue;
        }

        if (!in_table)
            continue;

        if (g_strcmp0(line, ".") == 0)
            break;

        if (g_str_has_prefix(line, "id\taddress\tname\tstate\talsa_port"))
            continue;

        char **cols = g_strsplit(line, "\t", 5);
        if (!cols || !cols[0] || !cols[1] || !cols[2] || !cols[3]) {
            g_strfreev(cols);
            continue;
        }

        MbUiDevice *device = mb_ui_device_new(safe_str(cols[0], "-"),
                                              safe_str(cols[1], "-"),
                                              safe_str(cols[2], "-"),
                                              safe_str(cols[3], "UNKNOWN"),
                                              cols[4] ? atoi(cols[4]) : -1);

        g_ptr_array_add(snapshot->devices, device);
        g_strfreev(cols);
    }

    g_strfreev(lines);
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
    MbUiSnapshot *snapshot = mb_ui_snapshot_new();

    GError *error = NULL;
    char *status = run_ctl(facade, "daemon-status", NULL, NULL, &error);
    if (!status) {
        snapshot->status.online = false;
        snapshot->last_error = g_strdup(error ? error->message : "service status failed");
        g_clear_error(&error);
        return snapshot;
    }

    char **status_lines = g_strsplit(status, "\n", 0);
    for (gsize i = 0; status_lines && status_lines[i]; i++) {
        char *line = status_lines[i];
        g_strstrip(line);
        if (g_str_has_prefix(line, "OK STATUS")) {
            parse_status_line(snapshot, line);
            break;
        }
    }

    g_strfreev(status_lines);
    g_free(status);

    char *list = run_ctl(facade, "daemon-list", NULL, NULL, &error);
    if (!list) {
        if (!snapshot->last_error)
            snapshot->last_error = g_strdup(error ? error->message : "device list failed");
        g_clear_error(&error);
        return snapshot;
    }

    parse_list_output(snapshot, list);
    g_free(list);

    return snapshot;
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
                    "invalid pair request");
        return false;
    }

    MbUiSnapshot *snapshot = mb_ui_facade_get_snapshot(facade);
    const MbUiDevice *device = mb_ui_snapshot_find_device(snapshot, device_id);

    if (!device) {
        mb_ui_snapshot_free(snapshot);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "device is not visible in current scan result");
        return false;
    }

    out->id = g_strdup(device->id);
    out->address = g_strdup(device->address && *device->address ? device->address : device->id);
    out->name = g_strdup(device->name ? device->name : "");
    out->profile = g_strdup(pair_profile_for_device_name(device->name));

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




bool mb_ui_facade_pair_scanned_device(MbUiFacade *facade,
                                      const char *device_id,
                                      GError **error) {
    PairEnrollDevice device = {0};

    if (!capture_pair_enroll_device(facade, device_id, &device, error))
        return false;

    /*
     * This is the enrollment flow used by the scan dialog.
     * The main window deliberately does not expose Pair/Trust/Configure
     * as separate musician-facing actions.
     */
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

    char *configure_argv[] = {
        facade->ctl_path ? facade->ctl_path : "midi-ble-rtctl",
        "configure",
        device.address,
        "--profile",
        device.profile,
        "--force",
        NULL,
    };

    if (!run_ctl_argv_checked(facade, "configure paired device", configure_argv, error)) {
        pair_enroll_device_clear(&device);
        return false;
    }

    GError *recheck_error = NULL;
    char *ignored = run_ctl(facade, "daemon-recheck", NULL, NULL, &recheck_error);
    g_free(ignored);

    if (recheck_error) {
        /*
         * Daemon may be offline during enrollment. That is not fatal for
         * pairing/configuration; the main window will show service state.
         */
        g_printerr("[midi-ble-rt-gui] pair: daemon-recheck skipped/failed: %s\\n",
                   recheck_error->message);
        g_clear_error(&recheck_error);
    }

    g_printerr("[midi-ble-rt-gui] pair: enrolled %s address=%s profile=%s\\n",
               device.name ? device.name : "-",
               device.address ? device.address : "-",
               device.profile ? device.profile : "-");

    pair_enroll_device_clear(&device);
    return true;
}

