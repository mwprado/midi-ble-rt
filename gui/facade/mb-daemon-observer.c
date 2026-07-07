#include "mb-daemon-observer.h"

#include <gio/gio.h>
#include <string.h>

#define MB_DAEMON_SYSTEMD_NAME "org.freedesktop.systemd1"
#define MB_DAEMON_SYSTEMD_MANAGER_PATH "/org/freedesktop/systemd1"
#define MB_DAEMON_SYSTEMD_MANAGER_IFACE "org.freedesktop.systemd1.Manager"
#define MB_DAEMON_SYSTEMD_UNIT_IFACE "org.freedesktop.systemd1.Unit"
#define MB_DAEMON_SYSTEMD_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define MB_DAEMON_UNIT_NAME "midi-ble-rtd.service"

struct _MbDaemonObserver {
    GDBusConnection *connection;
    char *unit_path;
    char *active_state;
    char *sub_state;
    bool active;
    guint properties_signal_id;
    MbDaemonObserverCallback callback;
    void *user_data;
};

static void observer_notify(MbDaemonObserver *observer) {
    if (!observer || !observer->callback)
        return;

    observer->callback(observer->active,
                       observer->active_state ? observer->active_state : "unknown",
                       observer->sub_state ? observer->sub_state : "unknown",
                       observer->user_data);
}

static bool observer_set_states(MbDaemonObserver *observer,
                                const char *active_state,
                                const char *sub_state,
                                bool notify) {
    if (!observer)
        return false;

    const char *new_active_state = active_state ? active_state : observer->active_state;
    const char *new_sub_state = sub_state ? sub_state : observer->sub_state;
    bool new_active = g_strcmp0(new_active_state, "active") == 0;

    bool changed =
        observer->active != new_active ||
        g_strcmp0(observer->active_state, new_active_state) != 0 ||
        g_strcmp0(observer->sub_state, new_sub_state) != 0;

    if (active_state) {
        g_free(observer->active_state);
        observer->active_state = g_strdup(active_state);
    }

    if (sub_state) {
        g_free(observer->sub_state);
        observer->sub_state = g_strdup(sub_state);
    }

    observer->active = new_active;

    if (notify && changed)
        observer_notify(observer);

    return changed;
}

static bool observer_load_unit(MbDaemonObserver *observer, GError **error) {
    if (!observer || !observer->connection) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "observador do daemon não conectado ao D-Bus");
        return false;
    }

    GVariant *reply = g_dbus_connection_call_sync(observer->connection,
                                                  MB_DAEMON_SYSTEMD_NAME,
                                                  MB_DAEMON_SYSTEMD_MANAGER_PATH,
                                                  MB_DAEMON_SYSTEMD_MANAGER_IFACE,
                                                  "LoadUnit",
                                                  g_variant_new("(s)", MB_DAEMON_UNIT_NAME),
                                                  G_VARIANT_TYPE("(o)"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  error);
    if (!reply)
        return false;

    const char *unit_path = NULL;
    g_variant_get(reply, "(&o)", &unit_path);

    g_free(observer->unit_path);
    observer->unit_path = g_strdup(unit_path);

    g_variant_unref(reply);
    return true;
}

static bool observer_read_unit_state(MbDaemonObserver *observer,
                                     bool notify,
                                     GError **error) {
    if (!observer || !observer->connection || !observer->unit_path) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "observador do daemon sem unit systemd carregada");
        return false;
    }

    GVariant *reply = g_dbus_connection_call_sync(observer->connection,
                                                  MB_DAEMON_SYSTEMD_NAME,
                                                  observer->unit_path,
                                                  MB_DAEMON_SYSTEMD_PROPERTIES_IFACE,
                                                  "GetAll",
                                                  g_variant_new("(s)", MB_DAEMON_SYSTEMD_UNIT_IFACE),
                                                  G_VARIANT_TYPE("(a{sv})"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  error);
    if (!reply)
        return false;

    GVariant *props = NULL;
    g_variant_get(reply, "(@a{sv})", &props);

    const char *active_state = NULL;
    const char *sub_state = NULL;

    g_variant_lookup(props, "ActiveState", "&s", &active_state);
    g_variant_lookup(props, "SubState", "&s", &sub_state);

    observer_set_states(observer, active_state, sub_state, notify);

    g_variant_unref(props);
    g_variant_unref(reply);
    return true;
}

static void properties_changed_cb(GDBusConnection *connection,
                                  const char *sender_name,
                                  const char *object_path,
                                  const char *interface_name,
                                  const char *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    MbDaemonObserver *observer = user_data;
    if (!observer)
        return;

    const char *changed_iface = NULL;
    GVariant *changed = NULL;

    g_variant_get_child(parameters, 0, "&s", &changed_iface);
    changed = g_variant_get_child_value(parameters, 1);

    if (g_strcmp0(changed_iface, MB_DAEMON_SYSTEMD_UNIT_IFACE) != 0) {
        g_variant_unref(changed);
        return;
    }

    const char *active_state = NULL;
    const char *sub_state = NULL;

    bool have_active = g_variant_lookup(changed, "ActiveState", "&s", &active_state);
    bool have_sub = g_variant_lookup(changed, "SubState", "&s", &sub_state);

    if (have_active || have_sub)
        observer_set_states(observer,
                            have_active ? active_state : NULL,
                            have_sub ? sub_state : NULL,
                            true);

    g_variant_unref(changed);
}

MbDaemonObserver *mb_daemon_observer_new(void) {
    MbDaemonObserver *observer = g_new0(MbDaemonObserver, 1);
    observer->active_state = g_strdup("unknown");
    observer->sub_state = g_strdup("unknown");
    return observer;
}

void mb_daemon_observer_free(MbDaemonObserver *observer) {
    if (!observer)
        return;

    if (observer->connection && observer->properties_signal_id)
        g_dbus_connection_signal_unsubscribe(observer->connection,
                                             observer->properties_signal_id);

    g_clear_object(&observer->connection);
    g_free(observer->unit_path);
    g_free(observer->active_state);
    g_free(observer->sub_state);
    g_free(observer);
}

bool mb_daemon_observer_start(MbDaemonObserver *observer,
                              MbDaemonObserverCallback callback,
                              void *user_data,
                              GError **error) {
    if (!observer) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "observador do daemon inválido");
        return false;
    }

    observer->callback = callback;
    observer->user_data = user_data;

    if (!observer->connection) {
        observer->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
        if (!observer->connection)
            return false;
    }

    if (!observer_load_unit(observer, error))
        return false;

    if (!observer_read_unit_state(observer, false, error))
        return false;

    if (!observer->properties_signal_id) {
        observer->properties_signal_id =
            g_dbus_connection_signal_subscribe(observer->connection,
                                               MB_DAEMON_SYSTEMD_NAME,
                                               MB_DAEMON_SYSTEMD_PROPERTIES_IFACE,
                                               "PropertiesChanged",
                                               observer->unit_path,
                                               MB_DAEMON_SYSTEMD_UNIT_IFACE,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               properties_changed_cb,
                                               observer,
                                               NULL);
    }

    observer_notify(observer);
    return true;
}

bool mb_daemon_observer_refresh(MbDaemonObserver *observer,
                                GError **error) {
    if (!observer)
        return false;

    if (!observer->unit_path && !observer_load_unit(observer, error))
        return false;

    return observer_read_unit_state(observer, true, error);
}

bool mb_daemon_observer_is_active(const MbDaemonObserver *observer) {
    return observer && observer->active;
}

const char *mb_daemon_observer_active_state(const MbDaemonObserver *observer) {
    return observer && observer->active_state ? observer->active_state : "unknown";
}

const char *mb_daemon_observer_sub_state(const MbDaemonObserver *observer) {
    return observer && observer->sub_state ? observer->sub_state : "unknown";
}
