#include "mb-gatt-midi.h"

#include "mb-bluez.h"

#include <glib.h>
#include <stdbool.h>

static bool uuid_equal(const char *a, const char *b) {
    return a && b && g_ascii_strcasecmp(a, b) == 0;
}

static bool path_has_prefix(const char *path, const char *prefix) {
    return path && prefix && g_str_has_prefix(path, prefix);
}

char *mb_gatt_midi_find_service(GDBusConnection *bus,
                                const char *device_path,
                                const char *service_uuid) {
    GVariant *objects = mb_bluez_get_managed_objects(bus);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    char *found = NULL;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        if (!path_has_prefix(path, device_path)) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *props = g_variant_lookup_value(ifaces, GATT_SERVICE_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *v_uuid = g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
        const char *uuid = v_uuid ? g_variant_get_string(v_uuid, NULL) : NULL;

        if (uuid_equal(uuid, service_uuid)) {
            found = g_strdup(path);
            g_print("Found BLE-MIDI service: %s\n", found);
        }

        if (v_uuid)
            g_variant_unref(v_uuid);
        g_variant_unref(props);
        g_variant_unref(ifaces);

        if (found)
            break;
    }

    g_variant_unref(objects);
    return found;
}
