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
