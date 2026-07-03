#include "mb-session.h"

#include <string.h>

static char *dup_or_null(const char *s) {
    return (s && *s) ? g_strdup(s) : NULL;
}

static void replace_string(char **dst, const char *src) {
    g_free(*dst);
    *dst = dup_or_null(src);
}

static void mb_session_reset_runtime(MbSession *session, bool keep_alsa_port) {
    if (!session)
        return;

    session->bluez_connected = false;
    session->services_resolved = false;
    session->notify_enabled = false;
    session->running_status = 0;

    g_clear_pointer(&session->midi_service_path, g_free);
    g_clear_pointer(&session->midi_char_path, g_free);
    g_clear_pointer(&session->midi_char_uuid, g_free);

    if (!keep_alsa_port) {
        session->alsa_ready = false;
        session->alsa_port_id = -1;
    }
}

const char *mb_session_state_name(MbSessionState state) {
    switch (state) {
        case MB_SESSION_IDLE: return "IDLE";
        case MB_SESSION_SCANNING: return "SCANNING";
        case MB_SESSION_CONNECTING: return "CONNECTING";
        case MB_SESSION_WAIT_SERVICES: return "WAIT_SERVICES";
        case MB_SESSION_BINDING_MIDI: return "BINDING_MIDI";
        case MB_SESSION_CREATING_ALSA: return "CREATING_ALSA";
        case MB_SESSION_ENABLING_NOTIFY: return "ENABLING_NOTIFY";
        case MB_SESSION_STREAMING: return "STREAMING";
        case MB_SESSION_RECONNECTING: return "RECONNECTING";
        case MB_SESSION_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *mb_error_name(MbErrorCode error) {
    switch (error) {
        case MB_ERR_NONE: return "NONE";
        case MB_ERR_BLUEZ_UNAVAILABLE: return "BLUEZ_UNAVAILABLE";
        case MB_ERR_ADAPTER_POWERED_OFF: return "ADAPTER_POWERED_OFF";
        case MB_ERR_CONNECT_FAILED: return "CONNECT_FAILED";
        case MB_ERR_CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
        case MB_ERR_SERVICES_TIMEOUT: return "SERVICES_TIMEOUT";
        case MB_ERR_MIDI_SERVICE_NOT_FOUND: return "MIDI_SERVICE_NOT_FOUND";
        case MB_ERR_MIDI_CHAR_NOT_FOUND: return "MIDI_CHAR_NOT_FOUND";
        case MB_ERR_NOTIFY_FAILED: return "NOTIFY_FAILED";
        case MB_ERR_NOTIFY_TIMEOUT: return "NOTIFY_TIMEOUT";
        case MB_ERR_GATT_WRITE_FAILED: return "GATT_WRITE_FAILED";
        case MB_ERR_ALSA_FAILED: return "ALSA_FAILED";
        case MB_ERR_ALSA_IO_FAILED: return "ALSA_IO_FAILED";
        case MB_ERR_ALSA_DECODE_FAILED: return "ALSA_DECODE_FAILED";
        case MB_ERR_RUNTIME_FAILED: return "RUNTIME_FAILED";
        case MB_ERR_DISCONNECTED: return "DISCONNECTED";
        case MB_ERR_INVALID_TRANSITION: return "INVALID_TRANSITION";
        case MB_ERR_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

const char *mb_event_name(MbEventType event) {
    switch (event) {
        case MB_EV_CMD_SCAN: return "CMD_SCAN";
        case MB_EV_CMD_CONNECT: return "CMD_CONNECT";
        case MB_EV_CMD_DISCONNECT: return "CMD_DISCONNECT";
        case MB_EV_CMD_FORGET: return "CMD_FORGET";
        case MB_EV_BLUEZ_DEVICE_FOUND: return "BLUEZ_DEVICE_FOUND";
        case MB_EV_BLUEZ_CONNECTED: return "BLUEZ_CONNECTED";
        case MB_EV_BLUEZ_DISCONNECTED: return "BLUEZ_DISCONNECTED";
        case MB_EV_BLUEZ_SERVICES_RESOLVED: return "BLUEZ_SERVICES_RESOLVED";
        case MB_EV_BLUEZ_ADAPTER_OFF: return "BLUEZ_ADAPTER_OFF";
        case MB_EV_BLUEZ_CONNECT_FAILED: return "BLUEZ_CONNECT_FAILED";
        case MB_EV_MIDI_SERVICE_NOT_FOUND: return "MIDI_SERVICE_NOT_FOUND";
        case MB_EV_MIDI_CHAR_FOUND: return "MIDI_CHAR_FOUND";
        case MB_EV_MIDI_CHAR_NOT_FOUND: return "MIDI_CHAR_NOT_FOUND";
        case MB_EV_ALSA_READY: return "ALSA_READY";
        case MB_EV_ALSA_FAILED: return "ALSA_FAILED";
        case MB_EV_ALSA_IO_FAILED: return "ALSA_IO_FAILED";
        case MB_EV_ALSA_DECODE_FAILED: return "ALSA_DECODE_FAILED";
        case MB_EV_NOTIFY_OK: return "NOTIFY_OK";
        case MB_EV_NOTIFY_FAILED: return "NOTIFY_FAILED";
        case MB_EV_GATT_WRITE_FAILED: return "GATT_WRITE_FAILED";
        case MB_EV_RUNTIME_FAILED: return "RUNTIME_FAILED";
        case MB_EV_RECONNECT_TIMER: return "RECONNECT_TIMER";
        case MB_EV_TIMEOUT: return "TIMEOUT";
        case MB_EV_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

void mb_session_init(MbSession *session,
                     const char *device_path,
                     const char *address,
                     const char *name) {
    g_return_if_fail(session != NULL);

    memset(session, 0, sizeof(*session));
    session->state = MB_SESSION_IDLE;
    session->error = MB_ERR_NONE;
    session->alsa_port_id = -1;
    session->auto_reconnect = true;
    mb_session_set_identity(session, device_path, address, name);
}

MbSession *mb_session_new(const char *device_path,
                          const char *address,
                          const char *name) {
    MbSession *session = g_new0(MbSession, 1);
    mb_session_init(session, device_path, address, name);
    return session;
}

void mb_session_clear(MbSession *session) {
    if (!session)
        return;

    g_clear_pointer(&session->device_path, g_free);
    g_clear_pointer(&session->address, g_free);
    g_clear_pointer(&session->name, g_free);
    g_clear_pointer(&session->midi_service_path, g_free);
    g_clear_pointer(&session->midi_char_path, g_free);
    g_clear_pointer(&session->midi_char_uuid, g_free);

    memset(session, 0, sizeof(*session));
    session->alsa_port_id = -1;
}

void mb_session_free(MbSession *session) {
    if (!session)
        return;
    mb_session_clear(session);
    g_free(session);
}

void mb_session_set_identity(MbSession *session,
                             const char *device_path,
                             const char *address,
                             const char *name) {
    g_return_if_fail(session != NULL);

    replace_string(&session->device_path, device_path);
    replace_string(&session->address, address);
    replace_string(&session->name, name);
}

void mb_session_set_midi_binding(MbSession *session,
                                 const char *service_path,
                                 const char *char_path,
                                 const char *char_uuid) {
    g_return_if_fail(session != NULL);

    replace_string(&session->midi_service_path, service_path);
    replace_string(&session->midi_char_path, char_path);
    replace_string(&session->midi_char_uuid, char_uuid);
}

void mb_session_set_alsa_port(MbSession *session, int alsa_port_id) {
    g_return_if_fail(session != NULL);

    session->alsa_port_id = alsa_port_id;
    session->alsa_ready = alsa_port_id >= 0;
}

static void mb_session_enter_error(MbSession *session, MbErrorCode error) {
    session->state = MB_SESSION_ERROR;
    session->error = error;
}

static void mb_session_enter_reconnecting_or_error(MbSession *session,
                                                   MbErrorCode error,
                                                   bool keep_alsa_port) {
    mb_session_reset_runtime(session, keep_alsa_port);
    if (session->auto_reconnect) {
        session->state = MB_SESSION_RECONNECTING;
        session->error = error;
    } else {
        mb_session_enter_error(session, error);
    }
}

static void mb_session_note_nonfatal_error(MbSession *session, MbErrorCode error) {
    session->error = error;
}

bool mb_session_handle_event(MbSession *session, MbEventType event) {
    g_return_val_if_fail(session != NULL, false);

    if (event == MB_EV_CMD_FORGET) {
        mb_session_reset_runtime(session, false);
        session->state = MB_SESSION_IDLE;
        session->error = MB_ERR_NONE;
        return true;
    }

    if (event == MB_EV_CMD_DISCONNECT) {
        mb_session_reset_runtime(session, false);
        session->state = MB_SESSION_IDLE;
        session->error = MB_ERR_NONE;
        return true;
    }

    if (event == MB_EV_FATAL) {
        mb_session_enter_error(session, MB_ERR_INTERNAL);
        return true;
    }

    if (event == MB_EV_RUNTIME_FAILED) {
        mb_session_enter_error(session, MB_ERR_RUNTIME_FAILED);
        return true;
    }

    if (event == MB_EV_BLUEZ_ADAPTER_OFF) {
        mb_session_reset_runtime(session, false);
        mb_session_enter_error(session, MB_ERR_ADAPTER_POWERED_OFF);
        return true;
    }

    if (event == MB_EV_BLUEZ_CONNECT_FAILED) {
        if (session->state == MB_SESSION_CONNECTING)
            mb_session_enter_reconnecting_or_error(session, MB_ERR_CONNECT_FAILED, false);
        else
            mb_session_enter_error(session, MB_ERR_CONNECT_FAILED);
        return true;
    }

    if (event == MB_EV_TIMEOUT) {
        if (session->state == MB_SESSION_CONNECTING) {
            mb_session_enter_reconnecting_or_error(session, MB_ERR_CONNECT_TIMEOUT, false);
        } else if (session->state == MB_SESSION_WAIT_SERVICES) {
            mb_session_enter_reconnecting_or_error(session, MB_ERR_SERVICES_TIMEOUT, true);
        } else if (session->state == MB_SESSION_ENABLING_NOTIFY) {
            mb_session_enter_reconnecting_or_error(session, MB_ERR_NOTIFY_TIMEOUT, true);
        } else {
            mb_session_enter_error(session, MB_ERR_INTERNAL);
        }
        return true;
    }

    if (event == MB_EV_MIDI_SERVICE_NOT_FOUND) {
        mb_session_enter_error(session, MB_ERR_MIDI_SERVICE_NOT_FOUND);
        return true;
    }

    if (event == MB_EV_MIDI_CHAR_NOT_FOUND) {
        mb_session_enter_error(session, MB_ERR_MIDI_CHAR_NOT_FOUND);
        return true;
    }

    if (event == MB_EV_ALSA_FAILED) {
        mb_session_enter_error(session, MB_ERR_ALSA_FAILED);
        return true;
    }

    if (event == MB_EV_NOTIFY_FAILED) {
        mb_session_enter_reconnecting_or_error(session, MB_ERR_NOTIFY_FAILED, true);
        return true;
    }

    if (event == MB_EV_GATT_WRITE_FAILED) {
        mb_session_enter_reconnecting_or_error(session, MB_ERR_GATT_WRITE_FAILED, true);
        return true;
    }

    if (event == MB_EV_ALSA_IO_FAILED) {
        if (session->state == MB_SESSION_STREAMING) {
            mb_session_note_nonfatal_error(session, MB_ERR_ALSA_IO_FAILED);
            return true;
        }
        mb_session_enter_error(session, MB_ERR_ALSA_IO_FAILED);
        return true;
    }

    if (event == MB_EV_ALSA_DECODE_FAILED) {
        if (session->state == MB_SESSION_STREAMING) {
            mb_session_note_nonfatal_error(session, MB_ERR_ALSA_DECODE_FAILED);
            return true;
        }
        mb_session_enter_error(session, MB_ERR_ALSA_DECODE_FAILED);
        return true;
    }

    switch (session->state) {
        case MB_SESSION_IDLE:
            if (event == MB_EV_CMD_SCAN) {
                session->state = MB_SESSION_SCANNING;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_CMD_CONNECT) {
                session->state = MB_SESSION_CONNECTING;
                session->error = MB_ERR_NONE;
                return true;
            }
            break;

        case MB_SESSION_SCANNING:
            if (event == MB_EV_BLUEZ_DEVICE_FOUND)
                return true;
            if (event == MB_EV_CMD_CONNECT) {
                session->state = MB_SESSION_CONNECTING;
                session->error = MB_ERR_NONE;
                return true;
            }
            break;

        case MB_SESSION_CONNECTING:
            if (event == MB_EV_BLUEZ_CONNECTED) {
                session->bluez_connected = true;
                session->state = MB_SESSION_WAIT_SERVICES;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_BLUEZ_DISCONNECTED) {
                mb_session_enter_reconnecting_or_error(session, MB_ERR_CONNECT_FAILED, false);
                return true;
            }
            break;

        case MB_SESSION_WAIT_SERVICES:
            if (event == MB_EV_BLUEZ_SERVICES_RESOLVED) {
                session->services_resolved = true;
                session->state = MB_SESSION_BINDING_MIDI;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_BLUEZ_DISCONNECTED) {
                mb_session_enter_reconnecting_or_error(session, MB_ERR_DISCONNECTED, true);
                return true;
            }
            break;

        case MB_SESSION_BINDING_MIDI:
            if (event == MB_EV_MIDI_CHAR_FOUND) {
                session->state = MB_SESSION_CREATING_ALSA;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_BLUEZ_DISCONNECTED) {
                mb_session_enter_reconnecting_or_error(session, MB_ERR_DISCONNECTED, true);
                return true;
            }
            break;

        case MB_SESSION_CREATING_ALSA:
            if (event == MB_EV_ALSA_READY) {
                session->alsa_ready = true;
                if (session->alsa_port_id < 0)
                    session->alsa_port_id = 0;
                session->state = MB_SESSION_ENABLING_NOTIFY;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_BLUEZ_DISCONNECTED) {
                mb_session_enter_reconnecting_or_error(session, MB_ERR_DISCONNECTED, true);
                return true;
            }
            break;

        case MB_SESSION_ENABLING_NOTIFY:
            if (event == MB_EV_NOTIFY_OK) {
                session->notify_enabled = true;
                session->state = MB_SESSION_STREAMING;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_BLUEZ_DISCONNECTED) {
                mb_session_enter_reconnecting_or_error(session, MB_ERR_DISCONNECTED, true);
                return true;
            }
            break;

        case MB_SESSION_STREAMING:
            if (event == MB_EV_BLUEZ_DISCONNECTED) {
                mb_session_enter_reconnecting_or_error(session, MB_ERR_DISCONNECTED, true);
                return true;
            }
            break;

        case MB_SESSION_RECONNECTING:
            if (event == MB_EV_RECONNECT_TIMER || event == MB_EV_CMD_CONNECT) {
                session->reconnect_attempts++;
                session->state = MB_SESSION_CONNECTING;
                session->error = MB_ERR_NONE;
                return true;
            }
            break;

        case MB_SESSION_ERROR:
            if (event == MB_EV_CMD_CONNECT) {
                mb_session_reset_runtime(session, false);
                session->state = MB_SESSION_CONNECTING;
                session->error = MB_ERR_NONE;
                return true;
            }
            if (event == MB_EV_CMD_SCAN) {
                mb_session_reset_runtime(session, false);
                session->state = MB_SESSION_SCANNING;
                session->error = MB_ERR_NONE;
                return true;
            }
            break;
    }

    session->error = MB_ERR_INVALID_TRANSITION;
    return false;
}

bool mb_session_is_streaming_ready(const MbSession *session) {
    return session &&
           session->state == MB_SESSION_STREAMING &&
           session->bluez_connected &&
           session->services_resolved &&
           session->midi_char_path &&
           session->notify_enabled &&
           session->alsa_ready &&
           session->alsa_port_id >= 0;
}

void mb_daemon_init(MbDaemon *daemon) {
    g_return_if_fail(daemon != NULL);

    memset(daemon, 0, sizeof(*daemon));
    daemon->state = MB_DAEMON_READY;
    daemon->alsa_client_id = -1;
    daemon->sessions_by_device_path = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                            g_free,
                                                            (GDestroyNotify)mb_session_free);
    daemon->sessions_by_address = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                        g_free, NULL);
}

void mb_daemon_clear(MbDaemon *daemon) {
    if (!daemon)
        return;

    g_clear_pointer(&daemon->sessions_by_address, g_hash_table_unref);
    g_clear_pointer(&daemon->sessions_by_device_path, g_hash_table_unref);
    memset(daemon, 0, sizeof(*daemon));
    daemon->alsa_client_id = -1;
}

static void mb_daemon_steal_path_index(MbDaemon *daemon, const char *device_path, MbSession *expected_session) {
    if (!daemon || !device_path || !*device_path)
        return;

    gpointer stored_key = NULL;
    gpointer stored_value = NULL;
    if (!g_hash_table_lookup_extended(daemon->sessions_by_device_path,
                                      device_path,
                                      &stored_key,
                                      &stored_value))
        return;

    if (stored_value != expected_session)
        return;

    g_hash_table_steal(daemon->sessions_by_device_path, device_path);
    g_free(stored_key);
}

static void mb_daemon_reindex_device_path(MbDaemon *daemon, MbSession *session,
                                          const char *old_device_path) {
    if (!daemon || !session)
        return;

    if (old_device_path && *old_device_path &&
        g_strcmp0(old_device_path, session->device_path) != 0)
        mb_daemon_steal_path_index(daemon, old_device_path, session);

    if (!session->device_path || !*session->device_path)
        return;

    MbSession *existing = g_hash_table_lookup(daemon->sessions_by_device_path,
                                             session->device_path);
    if (existing == session)
        return;

    g_hash_table_replace(daemon->sessions_by_device_path,
                         g_strdup(session->device_path), session);
}

static void mb_daemon_reindex_address(MbDaemon *daemon, MbSession *session,
                                      const char *old_address) {
    if (old_address && *old_address)
        g_hash_table_remove(daemon->sessions_by_address, old_address);

    if (session->address && *session->address)
        g_hash_table_replace(daemon->sessions_by_address,
                             g_strdup(session->address), session);
}

MbSession *mb_daemon_ensure_session(MbDaemon *daemon,
                                    const char *device_path,
                                    const char *address,
                                    const char *name) {
    g_return_val_if_fail(daemon != NULL, NULL);
    g_return_val_if_fail(device_path != NULL && *device_path != '\0', NULL);

    if (address && *address) {
        MbSession *session_by_address = g_hash_table_lookup(daemon->sessions_by_address, address);
        if (session_by_address) {
            char *old_device_path = dup_or_null(session_by_address->device_path);
            char *old_address = dup_or_null(session_by_address->address);
            mb_session_set_identity(session_by_address, device_path, address, name);
            mb_daemon_reindex_device_path(daemon, session_by_address, old_device_path);
            mb_daemon_reindex_address(daemon, session_by_address, old_address);
            g_free(old_device_path);
            g_free(old_address);
            return session_by_address;
        }
    }

    MbSession *session = g_hash_table_lookup(daemon->sessions_by_device_path, device_path);
    if (session) {
        char *old_address = dup_or_null(session->address);
        mb_session_set_identity(session, device_path, address, name);
        mb_daemon_reindex_address(daemon, session, old_address);
        g_free(old_address);
        return session;
    }

    session = mb_session_new(device_path, address, name);
    g_hash_table_insert(daemon->sessions_by_device_path,
                        g_strdup(session->device_path), session);
    if (session->address && *session->address)
        g_hash_table_insert(daemon->sessions_by_address,
                            g_strdup(session->address), session);
    return session;
}

MbSession *mb_daemon_find_session_by_device_path(MbDaemon *daemon,
                                                 const char *device_path) {
    if (!daemon || !device_path)
        return NULL;
    return g_hash_table_lookup(daemon->sessions_by_device_path, device_path);
}

MbSession *mb_daemon_find_session_by_address(MbDaemon *daemon,
                                             const char *address) {
    if (!daemon || !address)
        return NULL;
    return g_hash_table_lookup(daemon->sessions_by_address, address);
}

bool mb_daemon_remove_session_by_device_path(MbDaemon *daemon,
                                             const char *device_path) {
    if (!daemon || !device_path)
        return false;

    MbSession *session = g_hash_table_lookup(daemon->sessions_by_device_path, device_path);
    if (!session)
        return false;

    if (session->address && *session->address)
        g_hash_table_remove(daemon->sessions_by_address, session->address);

    return g_hash_table_remove(daemon->sessions_by_device_path, device_path);
}

size_t mb_daemon_session_count(const MbDaemon *daemon) {
    if (!daemon || !daemon->sessions_by_device_path)
        return 0;
    return (size_t)g_hash_table_size(daemon->sessions_by_device_path);
}
