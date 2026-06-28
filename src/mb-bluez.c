#include "mb-bluez.h"

#include <glib.h>

GVariant *mb_bluez_get_managed_objects(GDBusConnection *bus) {
    GError *error = NULL;

    GVariant *ret = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS, "/", OBJECT_MANAGER_IFACE, "GetManagedObjects",
        NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);

    if (!ret) {
        g_printerr("GetManagedObjects failed: %s\n", error->message);
        g_clear_error(&error);
        return NULL;
    }

    GVariant *objects = g_variant_get_child_value(ret, 0);
    g_variant_unref(ret);
    return objects;
}

char *mb_bluez_find_device_path_by_address(GDBusConnection *bus, const char *address) {
    if (!bus || !address || !*address)
        return NULL;

    GVariant *objects = mb_bluez_get_managed_objects(bus);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    char *match = NULL;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces, DEVICE_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *v_address = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
        const char *candidate_address = v_address ? g_variant_get_string(v_address, NULL) : NULL;

        if (candidate_address && g_ascii_strcasecmp(candidate_address, address) == 0)
            match = g_strdup(path);

        if (v_address)
            g_variant_unref(v_address);
        g_variant_unref(props);
        g_variant_unref(ifaces);

        if (match)
            break;
    }

    g_variant_unref(objects);
    return match;
}

bool mb_bluez_get_device_bool_property(GDBusConnection *bus, const char *device_path, const char *property, bool *out) {
    GError *error = NULL;

    GVariant *ret = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS, device_path, PROPERTIES_IFACE, "Get",
        g_variant_new("(ss)", DEVICE_IFACE, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

    if (!ret) {
        g_printerr("Get Device1.%s failed: %s\n", property, error->message);
        g_clear_error(&error);
        return false;
    }

    GVariant *value = NULL;
    g_variant_get(ret, "(v)", &value);
    *out = g_variant_get_boolean(value);

    g_variant_unref(value);
    g_variant_unref(ret);
    return true;
}

bool mb_bluez_set_device_trusted(GDBusConnection *bus, const char *device_path) {
    GError *error = NULL;

    GVariant *ret = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS, device_path, PROPERTIES_IFACE, "Set",
        g_variant_new("(ssv)", DEVICE_IFACE, "Trusted", g_variant_new_boolean(TRUE)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

    if (!ret) {
        g_printerr("Set Trusted=true failed: %s\n", error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Trusted=true set.\n");
    return true;
}

bool mb_bluez_pair_device(GDBusConnection *bus, const char *device_path) {
    bool paired = false;
    if (mb_bluez_get_device_bool_property(bus, device_path, "Paired", &paired) && paired) {
        g_print("Device already paired.\n");
        return true;
    }

    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS, device_path, DEVICE_IFACE, "Pair",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);

    if (!ret) {
        g_printerr("Device Pair() failed: %s\n", error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Device Pair() ok.\n");
    return true;
}

bool mb_bluez_connect_device(GDBusConnection *bus, const char *device_path) {
    bool connected = false;
    if (mb_bluez_get_device_bool_property(bus, device_path, "Connected", &connected) && connected) {
        g_print("Device already connected.\n");
        return true;
    }

    GError *error = NULL;
    g_print("Device Connected=false; calling Device1.Connect()...\n");

    GVariant *ret = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS, device_path, DEVICE_IFACE, "Connect",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);

    if (!ret) {
        const char *remote = g_dbus_error_get_remote_error(error);
        if (remote &&
            (g_strcmp0(remote, "org.bluez.Error.AlreadyConnected") == 0 ||
             g_strcmp0(remote, "org.bluez.Error.InProgress") == 0)) {
            g_print("Device already connected or in progress.\n");
            g_clear_error(&error);
            return true;
        }

        g_printerr("Device Connect() failed: %s\n", error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Device Connect() ok.\n");
    return true;
}

bool mb_bluez_wait_services_resolved(GDBusConnection *bus, const char *device_path, int timeout_ms) {
    const int step_ms = 100;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        bool resolved = false;
        if (mb_bluez_get_device_bool_property(bus, device_path, "ServicesResolved", &resolved) && resolved) {
            g_print("ServicesResolved=true.\n");
            return true;
        }

        g_usleep(step_ms * 1000);
        elapsed += step_ms;
    }

    g_printerr("Timed out waiting for ServicesResolved=true.\n");
    return false;
}

guint mb_bluez_subscribe_properties_changed(GDBusConnection *bus,
                                            const char *object_path,
                                            GDBusSignalCallback callback,
                                            gpointer user_data) {
    if (!bus || !object_path || !callback)
        return 0;

    return g_dbus_connection_signal_subscribe(
        bus,
        BLUEZ_BUS,
        PROPERTIES_IFACE,
        "PropertiesChanged",
        object_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        callback,
        user_data,
        NULL);
}
