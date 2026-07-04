#include "mb-gui-daemon-client.h"

#include <gio/gio.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct _MbGuiDaemonClient {
    char *socket_path;
};

static char *default_control_socket_path(void) {
    const char *runtime_dir = g_get_user_runtime_dir();
    if (!runtime_dir || !*runtime_dir)
        runtime_dir = g_get_tmp_dir();

    return g_build_filename(runtime_dir, "midi-ble-rt", "control.sock", NULL);
}

MbGuiDaemonClient *mb_gui_daemon_client_new(void) {
    MbGuiDaemonClient *client = g_new0(MbGuiDaemonClient, 1);
    client->socket_path = default_control_socket_path();
    return client;
}

void mb_gui_daemon_client_free(MbGuiDaemonClient *client) {
    if (!client)
        return;
    g_free(client->socket_path);
    g_free(client);
}

static bool write_all(int fd, const char *text, GError **error) {
    size_t len = strlen(text);
    const char *p = text;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "could not write control command: %s",
                        g_strerror(errno));
            return false;
        }
        p += n;
        len -= (size_t)n;
    }

    return true;
}

static char *client_send_command(MbGuiDaemonClient *client,
                                 const char *command,
                                 GError **error) {
    g_return_val_if_fail(client != NULL, NULL);
    g_return_val_if_fail(command != NULL, NULL);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "could not create control socket client: %s",
                    g_strerror(errno));
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(client->socket_path) >= sizeof(addr.sun_path)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FILENAME_TOO_LONG,
                    "control socket path too long: %s",
                    client->socket_path);
        close(fd);
        return NULL;
    }

    strncpy(addr.sun_path, client->socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "could not connect to midi-ble-rtd control socket %s: %s",
                    client->socket_path,
                    g_strerror(errno));
        close(fd);
        return NULL;
    }

    char *line = g_strdup_printf("%s\n", command);
    bool ok = write_all(fd, line, error);
    g_free(line);
    if (!ok) {
        close(fd);
        return NULL;
    }

    GString *reply = g_string_new(NULL);
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error(error,
                        G_IO_ERROR,
                        g_io_error_from_errno(errno),
                        "could not read control reply: %s",
                        g_strerror(errno));
            g_string_free(reply, TRUE);
            close(fd);
            return NULL;
        }
        if (n == 0)
            break;
        buf[n] = '\0';
        g_string_append(reply, buf);
    }

    close(fd);
    return g_string_free(reply, FALSE);
}

static const char *kv_lookup(GHashTable *table, const char *key, const char *fallback) {
    const char *value = table ? g_hash_table_lookup(table, key) : NULL;
    return value && *value ? value : fallback;
}

static unsigned kv_lookup_uint(GHashTable *table, const char *key) {
    const char *value = kv_lookup(table, key, "0");
    return (unsigned)g_ascii_strtoull(value, NULL, 10);
}

static GHashTable *parse_key_values(const char *line) {
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char **tokens = g_strsplit_set(line ? line : "", " \t\n", 0);

    for (gsize i = 0; tokens && tokens[i]; i++) {
        char *eq = strchr(tokens[i], '=');
        if (!eq || eq == tokens[i])
            continue;
        *eq = '\0';
        g_hash_table_insert(table, g_strdup(tokens[i]), g_strdup(eq + 1));
    }

    g_strfreev(tokens);
    return table;
}

static void parse_status_reply(const char *reply, MbGuiSnapshot *snapshot) {
    if (!reply || !snapshot)
        return;

    char **lines = g_strsplit(reply, "\n", 0);
    const char *status_line = NULL;

    for (gsize i = 0; lines && lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "OK STATUS")) {
            status_line = lines[i];
            break;
        }
    }

    if (!status_line) {
        g_strfreev(lines);
        return;
    }

    GHashTable *kv = parse_key_values(status_line);
    snapshot->status.online = true;
    snapshot->status.devices = kv_lookup_uint(kv, "devices");
    snapshot->status.streaming = kv_lookup_uint(kv, "streaming");
    snapshot->status.rx_workers = kv_lookup_uint(kv, "rx_workers");
    snapshot->status.tx_workers = kv_lookup_uint(kv, "tx_workers");
    snapshot->status.rtkit_priority = kv_lookup_uint(kv, "rtkit_priority");
    snapshot->status.lifecycle_queue = kv_lookup_uint(kv, "lifecycle_queue");
    snapshot->status.alsa_tx_thread = g_strdup(kv_lookup(kv, "alsa_tx_thread", "-"));
    snapshot->status.rtkit = g_strdup(kv_lookup(kv, "rtkit", "off"));
    snapshot->status.rtkit_rx = g_strdup(kv_lookup(kv, "rtkit_rx", "off"));
    snapshot->status.rtkit_tx = g_strdup(kv_lookup(kv, "rtkit_tx", "off"));
    snapshot->status.lifecycle_busy = g_strdup(kv_lookup(kv, "lifecycle_busy", "no"));

    g_hash_table_unref(kv);
    g_strfreev(lines);
}

static int header_index(char **header, const char *name) {
    for (int i = 0; header && header[i]; i++) {
        if (g_strcmp0(header[i], name) == 0)
            return i;
    }
    return -1;
}

static const char *col(char **row, int index, const char *fallback) {
    if (index < 0 || !row || !row[index] || !*row[index])
        return fallback;
    return row[index];
}

static void parse_list_reply(const char *reply, MbGuiSnapshot *snapshot) {
    if (!reply || !snapshot)
        return;

    char **lines = g_strsplit(reply, "\n", 0);
    bool in_list = false;
    char **header = NULL;

    for (gsize i = 0; lines && lines[i]; i++) {
        const char *line = lines[i];
        if (!line || !*line)
            continue;

        if (g_strcmp0(line, "OK LIST") == 0) {
            in_list = true;
            continue;
        }

        if (!in_list)
            continue;

        if (g_strcmp0(line, ".") == 0)
            break;

        if (!header) {
            header = g_strsplit(line, "\t", 0);
            continue;
        }

        char **row = g_strsplit(line, "\t", 0);
        int id_i = header_index(header, "id");
        int address_i = header_index(header, "address");
        int name_i = header_index(header, "name");
        int state_i = header_index(header, "state");
        int alsa_i = header_index(header, "alsa_port");
        int alsa_port = (int)g_ascii_strtoll(col(row, alsa_i, "-1"), NULL, 10);

        g_ptr_array_add(snapshot->devices,
                        mb_gui_device_new(col(row, id_i, "-"),
                                          col(row, address_i, "-"),
                                          col(row, name_i, "-"),
                                          col(row, state_i, "UNKNOWN"),
                                          alsa_port));
        g_strfreev(row);
    }

    g_strfreev(header);
    g_strfreev(lines);
}

MbGuiSnapshot *mb_gui_daemon_client_snapshot(MbGuiDaemonClient *client) {
    MbGuiSnapshot *snapshot = mb_gui_snapshot_new();
    GError *error = NULL;

    char *status = client_send_command(client, "STATUS", &error);
    if (!status) {
        snapshot->status.online = false;
        snapshot->last_error = g_strdup(error ? error->message : "daemon unavailable");
        g_clear_error(&error);
        return snapshot;
    }

    parse_status_reply(status, snapshot);
    g_free(status);

    char *list = client_send_command(client, "LIST", &error);
    if (!list) {
        snapshot->last_error = g_strdup(error ? error->message : "could not read device list");
        g_clear_error(&error);
        return snapshot;
    }

    parse_list_reply(list, snapshot);
    g_free(list);
    return snapshot;
}

static bool client_device_command(MbGuiDaemonClient *client,
                                  const char *verb,
                                  const char *device_id,
                                  GError **error) {
    if (!device_id || !*device_id) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "no device selected");
        return false;
    }

    char *command = g_strdup_printf("%s %s", verb, device_id);
    char *reply = client_send_command(client, command, error);
    g_free(command);

    if (!reply)
        return false;

    bool ok = g_str_has_prefix(reply, "OK ");
    if (!ok) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "daemon rejected command: %s",
                    reply);
    }
    g_free(reply);
    return ok;
}

bool mb_gui_daemon_client_connect_device(MbGuiDaemonClient *client,
                                         const char *device_id,
                                         GError **error) {
    return client_device_command(client, "CONNECT", device_id, error);
}

bool mb_gui_daemon_client_disconnect_device(MbGuiDaemonClient *client,
                                            const char *device_id,
                                            GError **error) {
    return client_device_command(client, "DISCONNECT", device_id, error);
}

bool mb_gui_daemon_client_recheck_device(MbGuiDaemonClient *client,
                                         const char *device_id,
                                         GError **error) {
    return client_device_command(client, "RECHECK", device_id, error);
}
