/*
 * midi-ble-rtctl
 *
 * Phase 1 control-plane CLI for BLE-MIDI devices on BlueZ.
 *
 * Scope:
 *   - list known BlueZ devices
 *   - scan nearby devices
 *   - show device info
 *   - probe BLE-MIDI GATT service/characteristic candidates
 *
 * The data plane remains midi-ble-rtd.
 */

#include <gio/gio.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLUEZ_BUS              "org.bluez"
#define OBJECT_MANAGER_IFACE   "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES_IFACE       "org.freedesktop.DBus.Properties"
#define ADAPTER_IFACE          "org.bluez.Adapter1"
#define DEVICE_IFACE           "org.bluez.Device1"
#define GATT_SERVICE_IFACE     "org.bluez.GattService1"
#define GATT_CHRC_IFACE        "org.bluez.GattCharacteristic1"

#define BLE_MIDI_SERVICE_UUID  "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define BLE_MIDI_IO_UUID       "7772e5db-3868-4112-a1a9-f2669d106bf3"
#define ROLAND_GOKEYS_IO_ALIAS "00006bf3-0000-1000-8000-00805f9b34fb"

typedef struct {
    GDBusConnection *bus;
} Ctl;

typedef struct {
    bool midi_only;
    bool connected_only;
} ListOptions;

static void usage(const char *argv0) {
    g_printerr(
        "Usage:\n"
        "  %s list [--midi-only] [--connected]\n"
        "  %s scan [--timeout SECONDS] [--midi-only]\n"
        "  %s info DEVICE\n"
        "  %s probe DEVICE\n"
        "\n"
        "DEVICE may be a Bluetooth address, BlueZ object path, or a case-insensitive\n"
        "substring of the BlueZ Name/Alias.\n",
        argv0, argv0, argv0, argv0);
}

static const char *yesno(bool v) {
    return v ? "yes" : "no";
}

static bool uuid_equal(const char *a, const char *b) {
    return a && b && g_ascii_strcasecmp(a, b) == 0;
}

static bool contains_casefold(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle)
        return false;

    char *h = g_ascii_strdown(haystack, -1);
    char *n = g_ascii_strdown(needle, -1);
    bool ok = strstr(h, n) != NULL;
    g_free(h);
    g_free(n);
    return ok;
}

static bool path_has_prefix(const char *path, const char *prefix) {
    return path && prefix && g_str_has_prefix(path, prefix);
}

static GVariant *get_managed_objects(Ctl *ctl) {
    GError *error = NULL;

    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, "/", OBJECT_MANAGER_IFACE, "GetManagedObjects",
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

static void print_string_array(GVariant *array, const char *indent) {
    if (!array) {
        g_print("%s(none)\n", indent);
        return;
    }

    GVariantIter iter;
    const char *s = NULL;
    bool any = false;

    g_variant_iter_init(&iter, array);
    while (g_variant_iter_next(&iter, "&s", &s)) {
        g_print("%s%s\n", indent, s);
        any = true;
    }

    if (!any)
        g_print("%s(none)\n", indent);
}

static const char *guess_profile(const char *name, const char *alias, GVariant *uuids) {
    if (contains_casefold(name, "go:keys") || contains_casefold(alias, "go:keys") ||
        contains_casefold(name, "go keys") || contains_casefold(alias, "go keys"))
        return "roland_gokeys";

    if (string_array_contains_uuid(uuids, BLE_MIDI_SERVICE_UUID))
        return "standard_ble_midi";

    if (contains_casefold(name, "midi") || contains_casefold(alias, "midi"))
        return "unknown_midi";

    return "-";
}

static bool has_midi_hint(const char *name, const char *alias, GVariant *uuids) {
    return string_array_contains_uuid(uuids, BLE_MIDI_SERVICE_UUID) ||
           contains_casefold(name, "midi") || contains_casefold(alias, "midi") ||
           contains_casefold(name, "go:keys") || contains_casefold(alias, "go:keys");
}

static char *first_adapter_path(Ctl *ctl) {
    GVariant *objects = get_managed_objects(ctl);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    char *adapter = NULL;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces, ADAPTER_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (props) {
            adapter = g_strdup(path);
            g_variant_unref(props);
            g_variant_unref(ifaces);
            break;
        }
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    return adapter;
}

static bool adapter_call(Ctl *ctl, const char *adapter_path, const char *method) {
    GError *error = NULL;

    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, adapter_path, ADAPTER_IFACE, method,
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);

    if (!ret) {
        const char *remote = g_dbus_error_get_remote_error(error);
        if (remote && g_strcmp0(remote, "org.bluez.Error.InProgress") == 0) {
            g_clear_error(&error);
            return true;
        }
        g_printerr("Adapter1.%s failed on %s: %s\n", method, adapter_path, error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    return true;
}

static void print_device_row(GVariant *props) {
    GVariant *v_address = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
    GVariant *v_name = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
    GVariant *v_alias = g_variant_lookup_value(props, "Alias", G_VARIANT_TYPE_STRING);
    GVariant *v_rssi = g_variant_lookup_value(props, "RSSI", G_VARIANT_TYPE_INT16);
    GVariant *v_paired = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_trusted = g_variant_lookup_value(props, "Trusted", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_connected = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_resolved = g_variant_lookup_value(props, "ServicesResolved", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    const char *address = v_address ? g_variant_get_string(v_address, NULL) : "-";
    const char *name = v_name ? g_variant_get_string(v_name, NULL) : "-";
    const char *alias = v_alias ? g_variant_get_string(v_alias, NULL) : "-";
    bool paired = v_paired ? g_variant_get_boolean(v_paired) : false;
    bool trusted = v_trusted ? g_variant_get_boolean(v_trusted) : false;
    bool connected = v_connected ? g_variant_get_boolean(v_connected) : false;
    bool resolved = v_resolved ? g_variant_get_boolean(v_resolved) : false;
    const char *uuid_hint = string_array_contains_uuid(v_uuids, BLE_MIDI_SERVICE_UUID) ? "ble-midi" : "-";
    const char *profile = guess_profile(name, alias, v_uuids);

    char rssi_buf[16];
    if (v_rssi)
        g_snprintf(rssi_buf, sizeof(rssi_buf), "%d", g_variant_get_int16(v_rssi));
    else
        g_snprintf(rssi_buf, sizeof(rssi_buf), "-");

    g_print("%-17s %-5s %-24.24s %-24.24s %-6s %-7s %-9s %-8s %-9s %s\n",
            address, rssi_buf, name, alias, yesno(paired), yesno(trusted),
            yesno(connected), yesno(resolved), uuid_hint, profile);

    if (v_address) g_variant_unref(v_address);
    if (v_name) g_variant_unref(v_name);
    if (v_alias) g_variant_unref(v_alias);
    if (v_rssi) g_variant_unref(v_rssi);
    if (v_paired) g_variant_unref(v_paired);
    if (v_trusted) g_variant_unref(v_trusted);
    if (v_connected) g_variant_unref(v_connected);
    if (v_resolved) g_variant_unref(v_resolved);
    if (v_uuids) g_variant_unref(v_uuids);
}

static int cmd_list(Ctl *ctl, const ListOptions *opts) {
    GVariant *objects = get_managed_objects(ctl);
    if (!objects)
        return 1;

    g_print("%-17s %-5s %-24s %-24s %-6s %-7s %-9s %-8s %-9s %s\n",
            "ADDRESS", "RSSI", "NAME", "ALIAS", "PAIRED", "TRUSTED",
            "CONNECTED", "GATT", "UUID-HINT", "PROFILE");

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    unsigned count = 0;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces, DEVICE_IFACE, G_VARIANT_TYPE("a{sv}"));
        if (!props) {
            g_variant_unref(ifaces);
            continue;
        }

        GVariant *v_name = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
        GVariant *v_alias = g_variant_lookup_value(props, "Alias", G_VARIANT_TYPE_STRING);
        GVariant *v_connected = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
        GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

        const char *name = v_name ? g_variant_get_string(v_name, NULL) : "";
        const char *alias = v_alias ? g_variant_get_string(v_alias, NULL) : "";
        bool connected = v_connected ? g_variant_get_boolean(v_connected) : false;
        bool show = true;

        if (opts->midi_only && !has_midi_hint(name, alias, v_uuids))
            show = false;
        if (opts->connected_only && !connected)
            show = false;

        if (show) {
            print_device_row(props);
            count++;
        }

        if (v_name) g_variant_unref(v_name);
        if (v_alias) g_variant_unref(v_alias);
        if (v_connected) g_variant_unref(v_connected);
        if (v_uuids) g_variant_unref(v_uuids);
        g_variant_unref(props);
        g_variant_unref(ifaces);
        (void)path;
    }

    g_variant_unref(objects);

    if (count == 0)
        g_print("No matching BlueZ devices found. Use `midi-ble-rtctl scan`.\n");

    return 0;
}

static char *find_device_path(Ctl *ctl, const char *selector) {
    GVariant *objects = get_managed_objects(ctl);
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

        int score = -1;
        if (g_strcmp0(path, selector) == 0)
            score = 100000;
        else if (address && g_ascii_strcasecmp(address, selector) == 0)
            score = 90000;
        else if (name && contains_casefold(name, selector))
            score = 1000;
        else if (alias && contains_casefold(alias, selector))
            score = 900;

        if (score >= 0 && string_array_contains_uuid(v_uuids, BLE_MIDI_SERVICE_UUID))
            score += 100;

        if (score > best_score) {
            g_free(best);
            best = g_strdup(path);
            best_score = score;
        }

        if (v_address) g_variant_unref(v_address);
        if (v_name) g_variant_unref(v_name);
        if (v_alias) g_variant_unref(v_alias);
        if (v_uuids) g_variant_unref(v_uuids);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    return best;
}

static GVariant *device_props(Ctl *ctl, const char *device_path) {
    GVariant *objects = get_managed_objects(ctl);
    if (!objects)
        return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    GVariant *props = NULL;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        if (g_strcmp0(path, device_path) == 0) {
            props = g_variant_lookup_value(ifaces, DEVICE_IFACE, G_VARIANT_TYPE("a{sv}"));
            g_variant_unref(ifaces);
            break;
        }
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    return props;
}

static int cmd_info(Ctl *ctl, const char *selector) {
    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        return 1;
    }

    GVariant *props = device_props(ctl, path);
    if (!props) {
        g_printerr("Device props not found for %s\n", path);
        g_free(path);
        return 1;
    }

    GVariant *v_address = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
    GVariant *v_name = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
    GVariant *v_alias = g_variant_lookup_value(props, "Alias", G_VARIANT_TYPE_STRING);
    GVariant *v_rssi = g_variant_lookup_value(props, "RSSI", G_VARIANT_TYPE_INT16);
    GVariant *v_paired = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_trusted = g_variant_lookup_value(props, "Trusted", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_connected = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_resolved = g_variant_lookup_value(props, "ServicesResolved", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    const char *address = v_address ? g_variant_get_string(v_address, NULL) : "-";
    const char *name = v_name ? g_variant_get_string(v_name, NULL) : "-";
    const char *alias = v_alias ? g_variant_get_string(v_alias, NULL) : "-";

    g_print("Device: %s\n", path);
    g_print("  Address:          %s\n", address);
    g_print("  Name:             %s\n", name);
    g_print("  Alias:            %s\n", alias);
    if (v_rssi)
        g_print("  RSSI:             %d\n", g_variant_get_int16(v_rssi));
    else
        g_print("  RSSI:             (none)\n");
    g_print("  Paired:           %s\n", yesno(v_paired ? g_variant_get_boolean(v_paired) : false));
    g_print("  Trusted:          %s\n", yesno(v_trusted ? g_variant_get_boolean(v_trusted) : false));
    g_print("  Connected:        %s\n", yesno(v_connected ? g_variant_get_boolean(v_connected) : false));
    g_print("  ServicesResolved: %s\n", yesno(v_resolved ? g_variant_get_boolean(v_resolved) : false));
    g_print("  Profile guess:    %s\n", guess_profile(name, alias, v_uuids));
    g_print("  UUIDs:\n");
    print_string_array(v_uuids, "    ");

    if (v_address) g_variant_unref(v_address);
    if (v_name) g_variant_unref(v_name);
    if (v_alias) g_variant_unref(v_alias);
    if (v_rssi) g_variant_unref(v_rssi);
    if (v_paired) g_variant_unref(v_paired);
    if (v_trusted) g_variant_unref(v_trusted);
    if (v_connected) g_variant_unref(v_connected);
    if (v_resolved) g_variant_unref(v_resolved);
    if (v_uuids) g_variant_unref(v_uuids);
    g_variant_unref(props);
    g_free(path);
    return 0;
}

static bool get_device_bool(Ctl *ctl, const char *path, const char *property, bool *out) {
    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, PROPERTIES_IFACE, "Get",
        g_variant_new("(ss)", DEVICE_IFACE, property), G_VARIANT_TYPE("(v)"),
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

static bool connect_for_probe(Ctl *ctl, const char *path) {
    bool connected = false;
    if (get_device_bool(ctl, path, "Connected", &connected) && connected)
        return true;

    g_print("Device not connected; calling Device1.Connect() for probe...\n");

    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, DEVICE_IFACE, "Connect",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &error);

    if (!ret) {
        const char *remote = g_dbus_error_get_remote_error(error);
        if (remote &&
            (g_strcmp0(remote, "org.bluez.Error.AlreadyConnected") == 0 ||
             g_strcmp0(remote, "org.bluez.Error.InProgress") == 0)) {
            g_clear_error(&error);
            return true;
        }

        g_printerr("Device1.Connect failed: %s\n", error->message);
        g_printerr("If authorization is required, use bluetoothctl for now; profile-aware connect comes next.\n");
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    return true;
}

static bool wait_services_resolved(Ctl *ctl, const char *path, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        bool resolved = false;
        if (get_device_bool(ctl, path, "ServicesResolved", &resolved) && resolved)
            return true;
        g_usleep(100 * 1000);
        elapsed += 100;
    }
    return false;
}

static char *find_ble_midi_service(Ctl *ctl, const char *device_path, bool print_all) {
    GVariant *objects = get_managed_objects(ctl);
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
        const char *uuid = v_uuid ? g_variant_get_string(v_uuid, NULL) : "";
        bool is_midi = uuid_equal(uuid, BLE_MIDI_SERVICE_UUID);

        if (print_all)
            g_print("Service: %s%s\n  UUID: %s\n", path, is_midi ? " [BLE-MIDI]" : "", uuid);
        if (is_midi && !found)
            found = g_strdup(path);

        if (v_uuid) g_variant_unref(v_uuid);
        g_variant_unref(props);
        g_variant_unref(ifaces);
    }

    g_variant_unref(objects);
    return found;
}

static char *find_best_midi_characteristic(Ctl *ctl, const char *device_path, const char *service_path) {
    GVariant *objects = get_managed_objects(ctl);
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

        const char *uuid = v_uuid ? g_variant_get_string(v_uuid, NULL) : "";
        const char *service = v_service ? g_variant_get_string(v_service, NULL) : "";

        if (g_strcmp0(service, service_path) == 0) {
            bool official = uuid_equal(uuid, BLE_MIDI_IO_UUID);
            bool alias = uuid_equal(uuid, ROLAND_GOKEYS_IO_ALIAS);
            bool notify = string_array_contains_exact(v_flags, "notify");
            bool write_cmd = string_array_contains_exact(v_flags, "write-without-response");
            bool write_req = string_array_contains_exact(v_flags, "write");
            bool read = string_array_contains_exact(v_flags, "read");
            guint16 mtu = v_mtu ? g_variant_get_uint16(v_mtu) : 0;

            int score = 0;
            if (official) score += 1000;
            if (alias) score += 950;
            if (notify) score += 100;
            if (write_cmd) score += 100;
            if (write_req) score += 20;
            if (read) score += 20;
            if (!official && !alias && notify && write_cmd) score += 500;

            g_print("Characteristic: %s\n", path);
            g_print("  UUID: %s%s%s\n", uuid,
                    official ? " [official]" : "",
                    alias ? " [roland-alias]" : "");
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
    return best;
}

static int cmd_probe(Ctl *ctl, const char *selector) {
    char *device_path = find_device_path(ctl, selector);
    if (!device_path) {
        g_printerr("Device not found: %s\n", selector);
        g_printerr("Run `midi-ble-rtctl scan --midi-only` first.\n");
        return 1;
    }

    g_print("Probe device: %s\n", device_path);

    if (!connect_for_probe(ctl, device_path)) {
        g_free(device_path);
        return 1;
    }

    if (!wait_services_resolved(ctl, device_path, 15000)) {
        g_printerr("Timed out waiting for ServicesResolved=true.\n");
        g_free(device_path);
        return 1;
    }

    g_print("ServicesResolved=true\n");
    g_print("GATT services under device:\n");

    char *service_path = find_ble_midi_service(ctl, device_path, true);
    if (!service_path) {
        g_printerr("BLE-MIDI service not found. For Roland GO:KEYS, connect MIDI before Audio/A2DP.\n");
        g_free(device_path);
        return 1;
    }

    g_print("Selected BLE-MIDI service: %s\n", service_path);
    g_print("GATT characteristics under BLE-MIDI service:\n");

    char *char_path = find_best_midi_characteristic(ctl, device_path, service_path);
    if (!char_path) {
        g_printerr("No usable BLE-MIDI I/O characteristic found.\n");
        g_free(service_path);
        g_free(device_path);
        return 1;
    }

    g_print("Selected BLE-MIDI I/O characteristic: %s\n", char_path);
    g_print("Status: usable for midi-ble-rtd StartNotify path.\n");

    g_free(char_path);
    g_free(service_path);
    g_free(device_path);
    return 0;
}

static int cmd_scan(Ctl *ctl, int timeout_s, const ListOptions *opts) {
    char *adapter = first_adapter_path(ctl);
    if (!adapter) {
        g_printerr("No BlueZ Adapter1 found. Is bluetoothd running?\n");
        return 1;
    }

    g_print("Using adapter: %s\n", adapter);
    g_print("Starting discovery for %d second(s)...\n", timeout_s);

    if (!adapter_call(ctl, adapter, "StartDiscovery")) {
        g_free(adapter);
        return 1;
    }

    for (int i = 0; i < timeout_s; i++)
        g_usleep(1000 * 1000);

    adapter_call(ctl, adapter, "StopDiscovery");
    g_free(adapter);

    return cmd_list(ctl, opts);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    GError *error = NULL;
    Ctl ctl = {0};
    ctl.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!ctl.bus) {
        g_printerr("Could not connect to system bus: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    const char *cmd = argv[1];
    int rc = 0;

    if (g_strcmp0(cmd, "list") == 0) {
        ListOptions opts = {0};
        for (int i = 2; i < argc; i++) {
            if (g_strcmp0(argv[i], "--midi-only") == 0)
                opts.midi_only = true;
            else if (g_strcmp0(argv[i], "--connected") == 0)
                opts.connected_only = true;
            else {
                usage(argv[0]);
                rc = 2;
                goto out;
            }
        }
        rc = cmd_list(&ctl, &opts);
    } else if (g_strcmp0(cmd, "scan") == 0) {
        ListOptions opts = {0};
        int timeout_s = 8;
        for (int i = 2; i < argc; i++) {
            if (g_strcmp0(argv[i], "--timeout") == 0 && i + 1 < argc)
                timeout_s = atoi(argv[++i]);
            else if (g_strcmp0(argv[i], "--midi-only") == 0)
                opts.midi_only = true;
            else {
                usage(argv[0]);
                rc = 2;
                goto out;
            }
        }
        if (timeout_s < 1)
            timeout_s = 1;
        if (timeout_s > 60)
            timeout_s = 60;
        rc = cmd_scan(&ctl, timeout_s, &opts);
    } else if (g_strcmp0(cmd, "info") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        rc = cmd_info(&ctl, argv[2]);
    } else if (g_strcmp0(cmd, "probe") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        rc = cmd_probe(&ctl, argv[2]);
    } else {
        usage(argv[0]);
        rc = 2;
    }

out:
    g_object_unref(ctl.bus);
    return rc;
}
