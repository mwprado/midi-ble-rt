#include "mb-device-discovery.h"

#include "mb-bluez.h"

#include <stdbool.h>
#include <string.h>

static bool uuid_equal(const char *a, const char *b) {
    return a && b && g_ascii_strcasecmp(a, b) == 0;
}

static bool string_contains_casefold(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle)
        return false;

    char *h = g_ascii_strdown(haystack, -1);
    char *n = g_ascii_strdown(needle, -1);
    bool ok = strstr(h, n) != NULL;
    g_free(h);
    g_free(n);
    return ok;
}

static bool string_array_contains_uuid(GVariant *array, const char *uuid) {
    if (!array)
        return false;

    GVariantIter iter;
    const char *s = NULL;

    g_variant_iter_init(&iter, array);
    while (g_variant_iter_next(&iter, "&s", &s)) {
        if (uuid_equal(s, uuid))
            return true;
    }

    return false;
}

char *mb_device_discovery_find(GDBusConnection *bus,
                               const char *address_filter,
                               const char *name_filter,
                               const char *service_uuid) {
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
        GVariant *props = g_variant_lookup_value(ifaces, DEVICE_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *v_address = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
        GVariant *v_name = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
        GVariant *v_alias = g_variant_lookup_value(props, "Alias", G_VARIANT_TYPE_STRING);
        GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

        const char *address = v_address ? g_variant_get_string(v_address, NULL) : NULL;
        const char *name = v_name ? g_variant_get_string(v_name, NULL) : NULL;
        const char *alias = v_alias ? g_variant_get_string(v_alias, NULL) : NULL;

        bool exact_address = address_filter && *address_filter &&
            address && g_ascii_strcasecmp(address, address_filter) == 0;
        bool name_match = name_filter && *name_filter &&
            ((name && string_contains_casefold(name, name_filter)) ||
             (alias && string_contains_casefold(alias, name_filter)));
        bool has_service_uuid = string_array_contains_uuid(v_uuids, service_uuid);

        if (exact_address || name_match || has_service_uuid) {
            int score = 0;
            if (exact_address) score += 10000;
            if (has_service_uuid) score += 1000;
            if (name && string_contains_casefold(name, "midi")) score += 100;
            if (alias && string_contains_casefold(alias, "midi")) score += 100;

            g_print("Candidate Device1: %s\n", path);
            g_print("  Address: %s\n", address ? address : "(none)");
            g_print("  Name:    %s\n", name ? name : "(none)");
            g_print("  Alias:   %s\n", alias ? alias : "(none)");
            g_print("  BLE-MIDI UUID in Device1.UUIDs: %s\n", has_service_uuid ? "yes" : "no");
            g_print("  Score: %d\n", score);

            if (score > best_score) {
                g_free(best);
                best = g_strdup(path);
                best_score = score;
            }
        }

        if (v_address) g_variant_unref(v_address);
        if (v_name) g_variant_unref(v_name);
        if (v_alias) g_variant_unref(v_alias);
        if (v_uuids) g_variant_unref(v_uuids);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);

    if (best)
        g_print("Selected Device1: %s\n", best);

    return best;
}
