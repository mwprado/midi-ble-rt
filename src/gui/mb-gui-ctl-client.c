#include "mb-gui-ctl-client.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct _MbGuiCtlClient {
    char *ctl_path;
};

static const char *safe_str(const char *s, const char *fallback) {
    return s && *s ? s : fallback;
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

static char *run_ctl(MbGuiCtlClient *client,
                     const char *arg1,
                     const char *arg2,
                     const char *arg3,
                     GError **error) {
    if (!client || !client->ctl_path || !arg1) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid ctl client command");
        return NULL;
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, client->ctl_path);
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
        if (stderr_text && *stderr_text) {
            g_prefix_error(error, "%s: ", g_strstrip(stderr_text));
        }
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    g_free(stderr_text);
    return stdout_text;
}

static void parse_status_line(MbGuiSnapshot *snapshot, const char *line) {
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

static void parse_list_output(MbGuiSnapshot *snapshot, const char *text) {
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

        MbGuiDevice *device = mb_gui_device_new(safe_str(cols[0], "-"),
                                                safe_str(cols[1], "-"),
                                                safe_str(cols[2], "-"),
                                                safe_str(cols[3], "UNKNOWN"),
                                                cols[4] ? atoi(cols[4]) : -1);

        g_ptr_array_add(snapshot->devices, device);
        g_strfreev(cols);
    }

    g_strfreev(lines);
}

MbGuiCtlClient *mb_gui_ctl_client_new(void) {
    MbGuiCtlClient *client = g_new0(MbGuiCtlClient, 1);
    client->ctl_path = g_strdup("midi-ble-rtctl");
    return client;
}

void mb_gui_ctl_client_free(MbGuiCtlClient *client) {
    if (!client)
        return;

    g_free(client->ctl_path);
    g_free(client);
}

MbGuiSnapshot *mb_gui_ctl_client_snapshot(MbGuiCtlClient *client) {
    MbGuiSnapshot *snapshot = mb_gui_snapshot_new();

    GError *error = NULL;
    char *status = run_ctl(client, "daemon-status", NULL, NULL, &error);
    if (!status) {
        snapshot->status.online = false;
        snapshot->last_error = g_strdup(error ? error->message : "daemon-status failed");
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

    char *list = run_ctl(client, "daemon-list", NULL, NULL, &error);
    if (!list) {
        if (!snapshot->last_error)
            snapshot->last_error = g_strdup(error ? error->message : "daemon-list failed");
        g_clear_error(&error);
        return snapshot;
    }

    parse_list_output(snapshot, list);
    g_free(list);

    return snapshot;
}

static bool run_device_command(MbGuiCtlClient *client,
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

    char *out = run_ctl(client, command, device_id, NULL, error);
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

bool mb_gui_ctl_client_connect_device(MbGuiCtlClient *client,
                                      const char *device_id,
                                      GError **error) {
    return run_device_command(client, "daemon-connect", device_id, error);
}

bool mb_gui_ctl_client_disconnect_device(MbGuiCtlClient *client,
                                         const char *device_id,
                                         GError **error) {
    return run_device_command(client, "daemon-disconnect", device_id, error);
}

bool mb_gui_ctl_client_recheck_device(MbGuiCtlClient *client,
                                      const char *device_id,
                                      GError **error) {
    return run_device_command(client, "daemon-recheck", device_id, error);
}
