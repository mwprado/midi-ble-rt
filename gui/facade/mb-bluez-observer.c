#include "mb-bluez-observer.h"

#include <gio/gio.h>
#include <string.h>

#define MB_BLUEZ_NAME "org.bluez"
#define MB_BLUEZ_OBJECT_MANAGER_IFACE "org.freedesktop.DBus.ObjectManager"
#define MB_BLUEZ_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define MB_BLUEZ_ADAPTER_IFACE "org.bluez.Adapter1"

struct _MbBluezObserver {
    GDBusConnection *connection;
    char *adapter_path;
    bool available;
    bool powered;
    bool discovering;
    guint properties_signal_id;
    guint interfaces_added_signal_id;
    guint interfaces_removed_signal_id;
    MbBluezObserverCallback callback;
    void *user_data;
};


static bool variant_dict_lookup_bool(GVariant *dict,
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

static void observer_notify(MbBluezObserver *observer) {
    if (!observer || !observer->callback)
        return;

    observer->callback(observer->available,
                       observer->powered,
                       observer->discovering,
                       observer->adapter_path ? observer->adapter_path : "",
                       observer->user_data);
}

static bool observer_set_state(MbBluezObserver *observer,
                               bool available,
                               bool powered,
                               bool discovering,
                               const char *adapter_path,
                               bool notify) {
    if (!observer)
        return false;

    bool changed =
        observer->available != available ||
        observer->powered != powered ||
        observer->discovering != discovering ||
        g_strcmp0(observer->adapter_path, adapter_path) != 0;

    observer->available = available;
    observer->powered = powered;
    observer->discovering = discovering;

    if (adapter_path) {
        g_free(observer->adapter_path);
        observer->adapter_path = g_strdup(adapter_path);
    } else {
        g_clear_pointer(&observer->adapter_path, g_free);
    }

    if (notify && changed)
        observer_notify(observer);

    return changed;
}

static bool observer_read_adapters(MbBluezObserver *observer,
                                   bool notify,
                                   GError **error) {
    if (!observer || !observer->connection) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "observador BlueZ não conectado ao D-Bus");
        return false;
    }

    GVariant *reply = g_dbus_connection_call_sync(observer->connection,
                                                  MB_BLUEZ_NAME,
                                                  "/",
                                                  MB_BLUEZ_OBJECT_MANAGER_IFACE,
                                                  "GetManagedObjects",
                                                  NULL,
                                                  G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  -1,
                                                  NULL,
                                                  error);
    if (!reply)
        return false;

    GVariant *objects = NULL;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);

    bool found = false;
    bool powered = false;
    bool discovering = false;
    char *adapter_path = NULL;

    bool fallback_found = false;
    bool fallback_powered = false;
    bool fallback_discovering = false;
    char *fallback_adapter_path = NULL;

    bool hci0_found = false;
    bool hci0_powered = false;
    bool hci0_discovering = false;
    char *hci0_adapter_path = NULL;

    GVariantIter object_iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;

    g_variant_iter_init(&object_iter, objects);
    while (g_variant_iter_next(&object_iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces,
                                                  MB_BLUEZ_ADAPTER_IFACE,
                                                  G_VARIANT_TYPE("a{sv}"));

        if (props) {
            bool p = false;
            bool d = false;

            variant_dict_lookup_bool(props, "Powered", &p);
            variant_dict_lookup_bool(props, "Discovering", &d);

            g_printerr("[midi-ble-rt-gui] BlueZ adapter: path=%s powered=%d discovering=%d\n",
                       path,
                       p,
                       d);

            if (!fallback_found) {
                fallback_found = true;
                fallback_powered = p;
                fallback_discovering = d;
                fallback_adapter_path = g_strdup(path);
            }

            if (g_strcmp0(path, "/org/bluez/hci0") == 0) {
                hci0_found = true;
                hci0_powered = p;
                hci0_discovering = d;
                g_free(hci0_adapter_path);
                hci0_adapter_path = g_strdup(path);
            }

            /*
             * Scan is a BlueZ operation. Prefer any powered adapter over a
             * hard-coded hci0, because bluetoothctl may select another
             * controller as the default one.
             */
            if (p && !found) {
                found = true;
                powered = p;
                discovering = d;
                adapter_path = g_strdup(path);
            }

            g_variant_unref(props);
        }

        g_variant_unref(ifaces);
    }

    if (!found && hci0_found) {
        found = true;
        powered = hci0_powered;
        discovering = hci0_discovering;
        adapter_path = g_strdup(hci0_adapter_path);
    }

    if (!found && fallback_found) {
        found = true;
        powered = fallback_powered;
        discovering = fallback_discovering;
        adapter_path = g_strdup(fallback_adapter_path);
    }

    g_free(fallback_adapter_path);
    g_free(hci0_adapter_path);

    observer_set_state(observer,
                       found,
                       found ? powered : false,
                       found ? discovering : false,
                       found ? adapter_path : NULL,
                       notify);

    g_free(adapter_path);
    g_variant_unref(objects);
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
    (void)interface_name;
    (void)signal_name;

    MbBluezObserver *observer = user_data;
    if (!observer)
        return;

    const char *changed_iface = NULL;
    GVariant *changed = NULL;

    g_variant_get_child(parameters, 0, "&s", &changed_iface);
    changed = g_variant_get_child_value(parameters, 1);

    if (g_strcmp0(changed_iface, MB_BLUEZ_ADAPTER_IFACE) != 0) {
        g_variant_unref(changed);
        return;
    }

    if (observer->adapter_path && g_strcmp0(observer->adapter_path, object_path) != 0) {
        g_variant_unref(changed);
        return;
    }

    bool powered = observer->powered;
    bool discovering = observer->discovering;

    g_variant_lookup(changed, "Powered", "b", &powered);
    g_variant_lookup(changed, "Discovering", "b", &discovering);

    observer_set_state(observer,
                       true,
                       powered,
                       discovering,
                       object_path,
                       true);

    g_variant_unref(changed);
}

static void interfaces_added_cb(GDBusConnection *connection,
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

    MbBluezObserver *observer = user_data;
    if (!observer)
        return;

    const char *path = NULL;
    GVariant *ifaces = NULL;

    g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &ifaces);

    GVariant *props = g_variant_lookup_value(ifaces,
                                              MB_BLUEZ_ADAPTER_IFACE,
                                              G_VARIANT_TYPE("a{sv}"));

    if (props) {
        GError *error = NULL;
        if (!observer_read_adapters(observer, true, &error)) {
            g_printerr("[midi-ble-rt-gui] BlueZ observer refresh failed: %s\n",
                       error && error->message ? error->message : "unknown error");
            g_clear_error(&error);
        }
        g_variant_unref(props);
    }

    g_variant_unref(ifaces);
}

static void interfaces_removed_cb(GDBusConnection *connection,
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

    MbBluezObserver *observer = user_data;
    if (!observer)
        return;

    const char *path = NULL;
    GVariantIter *interfaces = NULL;
    const char *iface = NULL;
    bool adapter_removed = false;

    g_variant_get(parameters, "(&oas)", &path, &interfaces);

    while (g_variant_iter_next(interfaces, "&s", &iface)) {
        if (g_strcmp0(iface, MB_BLUEZ_ADAPTER_IFACE) == 0) {
            adapter_removed = true;
            break;
        }
    }

    g_variant_iter_free(interfaces);

    if (!adapter_removed)
        return;

    if (!observer->adapter_path || g_strcmp0(observer->adapter_path, path) == 0) {
        GError *error = NULL;
        if (!observer_read_adapters(observer, true, &error)) {
            g_printerr("[midi-ble-rt-gui] BlueZ observer refresh failed: %s\n",
                       error && error->message ? error->message : "unknown error");
            g_clear_error(&error);
        }
    }
}

MbBluezObserver *mb_bluez_observer_new(void) {
    return g_new0(MbBluezObserver, 1);
}

void mb_bluez_observer_free(MbBluezObserver *observer) {
    if (!observer)
        return;

    if (observer->connection && observer->properties_signal_id)
        g_dbus_connection_signal_unsubscribe(observer->connection,
                                             observer->properties_signal_id);

    if (observer->connection && observer->interfaces_added_signal_id)
        g_dbus_connection_signal_unsubscribe(observer->connection,
                                             observer->interfaces_added_signal_id);

    if (observer->connection && observer->interfaces_removed_signal_id)
        g_dbus_connection_signal_unsubscribe(observer->connection,
                                             observer->interfaces_removed_signal_id);

    g_clear_object(&observer->connection);
    g_free(observer->adapter_path);
    g_free(observer);
}

bool mb_bluez_observer_start(MbBluezObserver *observer,
                             MbBluezObserverCallback callback,
                             void *user_data,
                             GError **error) {
    if (!observer) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "observador BlueZ inválido");
        return false;
    }

    observer->callback = callback;
    observer->user_data = user_data;

    if (!observer->connection) {
        observer->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
        if (!observer->connection)
            return false;
    }

    if (!observer->properties_signal_id) {
        observer->properties_signal_id =
            g_dbus_connection_signal_subscribe(observer->connection,
                                               MB_BLUEZ_NAME,
                                               MB_BLUEZ_PROPERTIES_IFACE,
                                               "PropertiesChanged",
                                               NULL,
                                               MB_BLUEZ_ADAPTER_IFACE,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               properties_changed_cb,
                                               observer,
                                               NULL);
    }

    if (!observer->interfaces_added_signal_id) {
        observer->interfaces_added_signal_id =
            g_dbus_connection_signal_subscribe(observer->connection,
                                               MB_BLUEZ_NAME,
                                               MB_BLUEZ_OBJECT_MANAGER_IFACE,
                                               "InterfacesAdded",
                                               "/",
                                               NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               interfaces_added_cb,
                                               observer,
                                               NULL);
    }

    if (!observer->interfaces_removed_signal_id) {
        observer->interfaces_removed_signal_id =
            g_dbus_connection_signal_subscribe(observer->connection,
                                               MB_BLUEZ_NAME,
                                               MB_BLUEZ_OBJECT_MANAGER_IFACE,
                                               "InterfacesRemoved",
                                               "/",
                                               NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               interfaces_removed_cb,
                                               observer,
                                               NULL);
    }

    if (!observer_read_adapters(observer, false, error))
        return false;

    observer_notify(observer);
    return true;
}

bool mb_bluez_observer_refresh(MbBluezObserver *observer,
                               GError **error) {
    return observer_read_adapters(observer, true, error);
}

bool mb_bluez_observer_is_available(const MbBluezObserver *observer) {
    return observer && observer->available;
}

bool mb_bluez_observer_is_powered(const MbBluezObserver *observer) {
    return observer && observer->powered;
}

bool mb_bluez_observer_is_discovering(const MbBluezObserver *observer) {
    return observer && observer->discovering;
}

bool mb_bluez_observer_can_scan(const MbBluezObserver *observer) {
    return observer && observer->available && observer->powered && !observer->discovering;
}

const char *mb_bluez_observer_adapter_path(const MbBluezObserver *observer) {
    return observer && observer->adapter_path ? observer->adapter_path : "";
}
