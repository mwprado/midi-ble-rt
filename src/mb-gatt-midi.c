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

static bool string_array_contains_exact(GVariant *array, const char *value) {
    if (!array)
        return false;

    GVariantIter iter;
    const char *s = NULL;

    g_variant_iter_init(&iter, array);
    while (g_variant_iter_next(&iter, "&s", &s)) {
        if (g_strcmp0(s, value) == 0)
            return true;
    }

    return false;
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

char *mb_gatt_midi_find_characteristic(GDBusConnection *bus,
                                       const char *device_path,
                                       const char *service_path,
                                       const char *io_uuid,
                                       const char *io_uuid_alias) {
    GVariant *objects = mb_bluez_get_managed_objects(bus);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    char *best = NULL;
    int best_score = -1;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        if (!path_has_prefix(path, device_path)) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *props = g_variant_lookup_value(ifaces, GATT_CHRC_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *v_uuid = g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
        GVariant *v_service = g_variant_lookup_value(props, "Service", G_VARIANT_TYPE_OBJECT_PATH);
        GVariant *v_flags = g_variant_lookup_value(props, "Flags", G_VARIANT_TYPE("as"));
        GVariant *v_mtu = g_variant_lookup_value(props, "MTU", G_VARIANT_TYPE_UINT16);

        const char *uuid = v_uuid ? g_variant_get_string(v_uuid, NULL) : NULL;
        const char *service = v_service ? g_variant_get_string(v_service, NULL) : NULL;
        guint16 mtu = v_mtu ? g_variant_get_uint16(v_mtu) : 0;

        bool in_service = g_strcmp0(service, service_path) == 0;
        bool official = uuid_equal(uuid, io_uuid);
        bool alias = uuid_equal(uuid, io_uuid_alias);
        bool notify = string_array_contains_exact(v_flags, "notify");
        bool indicate = string_array_contains_exact(v_flags, "indicate");
        bool write_cmd = string_array_contains_exact(v_flags, "write-without-response");
        bool write_req = string_array_contains_exact(v_flags, "write");
        bool read = string_array_contains_exact(v_flags, "read");

        if (in_service) {
            int score = 0;
            if (official) score += 1000;
            if (alias) score += 950;
            if (notify || indicate) score += 100;
            if (write_cmd) score += 100;
            if (write_req) score += 20;
            if (read) score += 20;
            if (!official && !alias && (notify || indicate) && write_cmd) score += 500;

            g_print("Characteristic under BLE-MIDI service: %s\n", path);
            g_print("  UUID: %s%s%s\n", uuid ? uuid : "(none)",
                    official ? " [official]" : "",
                    alias ? " [alias]" : "");
            g_print("  Flags:");
            if (v_flags) {
                GVariantIter flags_iter;
                const char *flag = NULL;
                g_variant_iter_init(&flags_iter, v_flags);
                while (g_variant_iter_next(&flags_iter, "&s", &flag))
                    g_print(" %s", flag);
            }
            g_print("\n");
            if (mtu)
                g_print("  MTU: %u\n", mtu);
            g_print("  Score: %d\n", score);

            if (score > best_score) {
                g_free(best);
                best = g_strdup(path);
                best_score = score;
            }
        }

        if (v_uuid) g_variant_unref(v_uuid);
        if (v_service) g_variant_unref(v_service);
        if (v_flags) g_variant_unref(v_flags);
        if (v_mtu) g_variant_unref(v_mtu);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);

    if (best)
        g_print("Selected BLE-MIDI I/O characteristic: %s\n", best);

    return best;
}

bool mb_gatt_midi_write_value_command(GDBusConnection *bus,
                                      const char *char_path,
                                      const uint8_t *packet,
                                      size_t packet_len,
                                      int timeout_ms) {
    if (!bus || !char_path || !packet || packet_len == 0)
        return false;

    GVariant *value = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                packet,
                                                packet_len,
                                                sizeof(guint8));

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string("command"));

    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        bus,
        BLUEZ_BUS,
        char_path,
        GATT_CHRC_IFACE,
        "WriteValue",
        g_variant_new("(@aya{sv})", value, &options),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        timeout_ms,
        NULL,
        &error);

    if (!ret) {
        g_printerr("WriteValue failed: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    return true;
}
