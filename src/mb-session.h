#ifndef MB_SESSION_H
#define MB_SESSION_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef enum {
    MB_SESSION_IDLE = 0,
    MB_SESSION_SCANNING,
    MB_SESSION_CONNECTING,
    MB_SESSION_WAIT_SERVICES,
    MB_SESSION_BINDING_MIDI,
    MB_SESSION_CREATING_ALSA,
    MB_SESSION_ENABLING_NOTIFY,
    MB_SESSION_STREAMING,
    MB_SESSION_RECONNECTING,
    MB_SESSION_ERROR
} MbSessionState;

typedef enum {
    MB_ERR_NONE = 0,
    MB_ERR_BLUEZ_UNAVAILABLE,
    MB_ERR_ADAPTER_POWERED_OFF,
    MB_ERR_CONNECT_FAILED,
    MB_ERR_CONNECT_TIMEOUT,
    MB_ERR_SERVICES_TIMEOUT,
    MB_ERR_MIDI_SERVICE_NOT_FOUND,
    MB_ERR_MIDI_CHAR_NOT_FOUND,
    MB_ERR_NOTIFY_FAILED,
    MB_ERR_NOTIFY_TIMEOUT,
    MB_ERR_GATT_WRITE_FAILED,
    MB_ERR_ALSA_FAILED,
    MB_ERR_ALSA_IO_FAILED,
    MB_ERR_ALSA_DECODE_FAILED,
    MB_ERR_RUNTIME_FAILED,
    MB_ERR_DISCONNECTED,
    MB_ERR_INVALID_TRANSITION,
    MB_ERR_INTERNAL
} MbErrorCode;

typedef enum {
    MB_EV_CMD_SCAN = 0,
    MB_EV_CMD_CONNECT,
    MB_EV_CMD_DISCONNECT,
    MB_EV_CMD_FORGET,

    MB_EV_BLUEZ_DEVICE_FOUND,
    MB_EV_BLUEZ_CONNECTED,
    MB_EV_BLUEZ_DISCONNECTED,
    MB_EV_BLUEZ_SERVICES_RESOLVED,
    MB_EV_BLUEZ_ADAPTER_OFF,
    MB_EV_BLUEZ_CONNECT_FAILED,

    MB_EV_MIDI_SERVICE_NOT_FOUND,
    MB_EV_MIDI_CHAR_FOUND,
    MB_EV_MIDI_CHAR_NOT_FOUND,

    MB_EV_ALSA_READY,
    MB_EV_ALSA_FAILED,
    MB_EV_ALSA_IO_FAILED,
    MB_EV_ALSA_DECODE_FAILED,

    MB_EV_NOTIFY_OK,
    MB_EV_NOTIFY_FAILED,

    MB_EV_GATT_WRITE_FAILED,
    MB_EV_RUNTIME_FAILED,

    MB_EV_RECONNECT_TIMER,
    MB_EV_TIMEOUT,
    MB_EV_FATAL
} MbEventType;

typedef enum {
    MB_DAEMON_STOPPED = 0,
    MB_DAEMON_READY,
    MB_DAEMON_SCANNING
} MbDaemonState;

typedef struct {
    MbSessionState state;
    MbErrorCode error;

    char *device_path;
    char *address;
    char *name;

    bool bluez_connected;
    bool services_resolved;

    char *midi_service_path;
    char *midi_char_path;
    char *midi_char_uuid;

    bool notify_enabled;

    int alsa_port_id;
    bool alsa_ready;

    bool auto_reconnect;
    unsigned reconnect_attempts;

    uint8_t running_status;
} MbSession;

typedef struct {
    MbDaemonState state;
    int alsa_client_id;

    GHashTable *sessions_by_device_path;
    GHashTable *sessions_by_address;
} MbDaemon;

const char *mb_session_state_name(MbSessionState state);
const char *mb_error_name(MbErrorCode error);
const char *mb_event_name(MbEventType event);

void mb_session_init(MbSession *session,
                     const char *device_path,
                     const char *address,
                     const char *name);
MbSession *mb_session_new(const char *device_path,
                          const char *address,
                          const char *name);
void mb_session_clear(MbSession *session);
void mb_session_free(MbSession *session);

void mb_session_set_identity(MbSession *session,
                             const char *device_path,
                             const char *address,
                             const char *name);
void mb_session_set_midi_binding(MbSession *session,
                                 const char *service_path,
                                 const char *char_path,
                                 const char *char_uuid);
void mb_session_set_alsa_port(MbSession *session, int alsa_port_id);

bool mb_session_handle_event(MbSession *session, MbEventType event);
bool mb_session_is_streaming_ready(const MbSession *session);

void mb_daemon_init(MbDaemon *daemon);
void mb_daemon_clear(MbDaemon *daemon);

MbSession *mb_daemon_ensure_session(MbDaemon *daemon,
                                    const char *device_path,
                                    const char *address,
                                    const char *name);
MbSession *mb_daemon_find_session_by_device_path(MbDaemon *daemon,
                                                 const char *device_path);
MbSession *mb_daemon_find_session_by_address(MbDaemon *daemon,
                                             const char *address);
bool mb_daemon_remove_session_by_device_path(MbDaemon *daemon,
                                             const char *device_path);
size_t mb_daemon_session_count(const MbDaemon *daemon);

G_END_DECLS

#endif /* MB_SESSION_H */
