/*
 * midi-ble-rtctl
 *
 * Control-plane CLI for BLE-MIDI devices on BlueZ.
 *
 * Current scope:
 *   - list known BlueZ devices
 *   - scan nearby devices
 *   - show device info
 *   - probe BLE-MIDI GATT service/characteristic candidates
 *   - pair/trust/connect/disconnect/forget devices through BlueZ
 *   - write local midi-ble-rtd config files
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
#include "mb-timeouts.h"

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

typedef enum {
    PROFILE_UNKNOWN = 0,
    PROFILE_STANDARD_BLE_MIDI,
    PROFILE_ROLAND_GOKEYS,
} ProfileKind;

typedef struct {
    ProfileKind profile;
    bool force_pair;
    bool no_pair;
    bool no_trust;
    bool no_probe;
    bool write_config;
    char *output_path;
} ConnectOptions;

typedef struct {
    ProfileKind profile;
    char *output_path;
    bool print_only;
    bool force;
} ConfigureOptions;

static const char *yesno(bool v) {
    return v ? "yes" : "no";
}

static bool is_help_arg(const char *s) {
    return g_strcmp0(s, "--help") == 0 || g_strcmp0(s, "-h") == 0;
}

static const char *profile_name(ProfileKind profile) {
    switch (profile) {
        case PROFILE_STANDARD_BLE_MIDI:
            return "standard_ble_midi";
        case PROFILE_ROLAND_GOKEYS:
            return "roland_gokeys";
        case PROFILE_UNKNOWN:
        default:
            return "-";
    }
}

static ProfileKind profile_from_string(const char *s) {
    if (!s || !*s)
        return PROFILE_UNKNOWN;
    if (g_ascii_strcasecmp(s, "standard_ble_midi") == 0 ||
        g_ascii_strcasecmp(s, "standard") == 0)
        return PROFILE_STANDARD_BLE_MIDI;
    if (g_ascii_strcasecmp(s, "roland_gokeys") == 0 ||
        g_ascii_strcasecmp(s, "go_keys") == 0 ||
        g_ascii_strcasecmp(s, "gokeys") == 0)
        return PROFILE_ROLAND_GOKEYS;
    return PROFILE_UNKNOWN;
}

static void help_device_selector(void) {
    g_print(
        "DEVICE selector:\n"
        "  DEVICE may be one of:\n"
        "    - Bluetooth address, for example CB:81:F4:62:FF:07\n"
        "    - BlueZ object path, for example /org/bluez/hci0/dev_CB_81_F4_62_FF_07\n"
        "    - case-insensitive substring of Name or Alias, for example gokeys\n"
        "\n"
        "  Address is preferred because it is stable. Name/Alias matching is convenient but\n"
        "  may select the wrong device if several devices have similar names.\n"
        "\n");
}

static void help_global(const char *argv0) {
    g_print(
        "midi-ble-rtctl - BlueZ control CLI for BLE-MIDI devices\n"
        "\n"
        "Usage:\n"
        "  %s COMMAND [ARGS...]\n"
        "  %s --help\n"
        "  %s help [COMMAND]\n"
        "\n"
        "Commands:\n"
        "  list         List known BlueZ devices\n"
        "  scan         Run BlueZ discovery, then list devices\n"
        "  info         Show one device's BlueZ properties\n"
        "  probe        Connect if needed and inspect BLE-MIDI GATT objects\n"
        "  pair         Pair a device through BlueZ Device1.Pair\n"
        "  trust        Set BlueZ Device1.Trusted=true\n"
        "  untrust      Set BlueZ Device1.Trusted=false\n"
        "  connect      Prepare a BLE-MIDI device using BlueZ\n"
        "  configure    Write a local midi-ble-rtd config file\n"
        "  disconnect   Disconnect a BlueZ device\n"
        "  forget       Remove a device from BlueZ cache/pairing records\n"
        "\n"
        "Help:\n"
        "  %s help configure\n"
        "  %s connect --help\n"
        "\n"
        "Typical GO:KEYS flow:\n"
        "  %s scan --timeout 10 --midi-only\n"
        "  %s connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config\n"
        "  %s probe CB:81:F4:62:FF:07\n"
        "  ./build/midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini\n"
        "\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static bool help_command(const char *argv0, const char *cmd) {
    if (!cmd || !*cmd) {
        help_global(argv0);
        return true;
    }

    if (g_strcmp0(cmd, "list") == 0) {
        g_print(
            "Usage:\n"
            "  %s list [--midi-only] [--connected]\n"
            "\n"
            "Description:\n"
            "  Lists devices known to BlueZ from ObjectManager.GetManagedObjects().\n"
            "  This does not start discovery. Use scan for live discovery.\n"
            "\n"
            "Options:\n"
            "  --midi-only\n"
            "      Show only devices with a BLE-MIDI hint. Hints include the BLE-MIDI\n"
            "      service UUID, a MIDI-looking name/alias, or a GO:KEYS-looking name.\n"
            "\n"
            "  --connected\n"
            "      Show only devices whose BlueZ Device1.Connected property is true.\n"
            "\n"
            "Examples:\n"
            "  %s list\n"
            "  %s list --midi-only\n"
            "  %s list --midi-only --connected\n"
            "\n",
            argv0, argv0, argv0, argv0);
        return true;
    }

    if (g_strcmp0(cmd, "scan") == 0) {
        g_print(
            "Usage:\n"
            "  %s scan [--timeout SECONDS] [--midi-only]\n"
            "\n"
            "Description:\n"
            "  Calls BlueZ Adapter1.StartDiscovery(), waits, then calls StopDiscovery()\n"
            "  and prints the discovered/known devices.\n"
            "\n"
            "Options:\n"
            "  --timeout SECONDS\n"
            "      Discovery duration. Minimum 1 second, maximum 60 seconds.\n"
            "      Default: 8 seconds.\n"
            "\n"
            "  --midi-only\n"
            "      After scanning, show only devices with a BLE-MIDI hint.\n"
            "\n"
            "Examples:\n"
            "  %s scan\n"
            "  %s scan --timeout 10 --midi-only\n"
            "\n",
            argv0, argv0, argv0);
        return true;
    }

    if (g_strcmp0(cmd, "info") == 0) {
        g_print(
            "Usage:\n"
            "  %s info DEVICE\n"
            "\n"
            "Description:\n"
            "  Shows one BlueZ Device1 object's Address, Name, Alias, RSSI, Paired,\n"
            "  Trusted, Connected, ServicesResolved, guessed profile, and advertised UUIDs.\n"
            "\n",
            argv0);
        help_device_selector();
        g_print(
            "Examples:\n"
            "  %s info CB:81:F4:62:FF:07\n"
            "  %s info gokeys\n"
            "\n",
            argv0, argv0);
        return true;
    }

    if (g_strcmp0(cmd, "probe") == 0) {
        g_print(
            "Usage:\n"
            "  %s probe DEVICE\n"
            "\n"
            "Description:\n"
            "  Connects the device if needed, waits for ServicesResolved=true, enumerates\n"
            "  GATT services under the device, finds the BLE-MIDI service, then scores\n"
            "  MIDI I/O characteristic candidates.\n"
            "\n"
            "  It accepts:\n"
            "    - official BLE-MIDI I/O UUID: %s\n"
            "    - Roland GO:KEYS alias:       %s\n"
            "\n"
            "  This command validates whether midi-ble-rtd can likely use StartNotify.\n"
            "  It does not create an ALSA port. The data plane remains midi-ble-rtd.\n"
            "\n",
            argv0, BLE_MIDI_IO_UUID, ROLAND_GOKEYS_IO_ALIAS);
        help_device_selector();
        g_print(
            "Example:\n"
            "  %s probe CB:81:F4:62:FF:07\n"
            "\n",
            argv0);
        return true;
    }

    if (g_strcmp0(cmd, "pair") == 0) {
        g_print(
            "Usage:\n"
            "  %s pair DEVICE\n"
            "\n"
            "Description:\n"
            "  Calls BlueZ Device1.Pair() unless the device is already paired.\n"
            "  If BlueZ requires user authorization and no Agent1 is available, this may fail.\n"
            "  Agent1 support is planned for a later phase.\n"
            "\n",
            argv0);
        help_device_selector();
        return true;
    }

    if (g_strcmp0(cmd, "trust") == 0 || g_strcmp0(cmd, "untrust") == 0) {
        g_print(
            "Usage:\n"
            "  %s trust DEVICE\n"
            "  %s untrust DEVICE\n"
            "\n"
            "Description:\n"
            "  Sets BlueZ Device1.Trusted.\n"
            "\n"
            "  trust:\n"
            "      Device1.Trusted=true. Useful for known MIDI devices that should\n"
            "      reconnect or authorize without repeated prompts.\n"
            "\n"
            "  untrust:\n"
            "      Device1.Trusted=false. Keeps the device in BlueZ but removes trust.\n"
            "\n",
            argv0, argv0);
        help_device_selector();
        return true;
    }

    if (g_strcmp0(cmd, "connect") == 0) {
        g_print(
            "Usage:\n"
            "  %s connect DEVICE [OPTIONS]\n"
            "\n"
            "Description:\n"
            "  Prepares a BLE-MIDI device through BlueZ. It can pair, trust, connect,\n"
            "  wait for ServicesResolved=true, and validate BLE-MIDI GATT objects.\n"
            "\n"
            "  This is still control-plane only. It does not start midi-ble-rtd yet.\n"
            "\n"
            "Options:\n"
            "  --profile PROFILE\n"
            "      Select device policy explicitly. Supported values:\n"
            "        roland_gokeys\n"
            "            Forces pair by default, sets Trusted=true unless --no-trust is used,\n"
            "            connects, waits for GATT, and accepts the Roland %s alias.\n"
            "\n"
            "        standard_ble_midi\n"
            "            Uses the standard BLE-MIDI service and official I/O characteristic.\n"
            "\n"
            "  --pair\n"
            "      Force Device1.Pair() before connect, even when profile is not Roland.\n"
            "\n"
            "  --no-pair\n"
            "      Do not call Device1.Pair(). Overrides profile default.\n"
            "\n"
            "  --no-trust\n"
            "      Do not set Trusted=true before connect.\n"
            "\n"
            "  --no-probe\n"
            "      Skip ServicesResolved wait and BLE-MIDI GATT validation after connect.\n"
            "      Useful only for debugging connection problems.\n"
            "\n"
            "  --write-config\n"
            "      After successful connect/probe, write a midi-ble-rtd config file.\n"
            "      Default path for roland_gokeys: ~/.config/midi-ble-rt/roland-gokeys.ini.\n"
            "\n"
            "  --output PATH\n"
            "      Config path used by --write-config.\n"
            "\n",
            argv0, ROLAND_GOKEYS_IO_ALIAS);
        help_device_selector();
        g_print(
            "Examples:\n"
            "  %s connect CB:81:F4:62:FF:07 --profile roland_gokeys\n"
            "  %s connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config\n"
            "  %s connect CB:81:F4:62:FF:07 --profile standard_ble_midi\n"
            "\n",
            argv0, argv0, argv0);
        return true;
    }

    if (g_strcmp0(cmd, "configure") == 0) {
        g_print(
            "Usage:\n"
            "  %s configure DEVICE --profile PROFILE [OPTIONS]\n"
            "\n"
            "Description:\n"
            "  Writes a local midi-ble-rtd config file for an already known BlueZ device.\n"
            "  This command does not start the daemon and does not create an ALSA port.\n"
            "\n"
            "Options:\n"
            "  --profile PROFILE\n"
            "      Required unless the device can be inferred. Supported values:\n"
            "        roland_gokeys\n"
            "        standard_ble_midi\n"
            "\n"
            "  --output PATH\n"
            "      Config file path. Default for roland_gokeys:\n"
            "      ~/.config/midi-ble-rt/roland-gokeys.ini\n"
            "\n"
            "  --print\n"
            "      Print the config to stdout instead of writing a file.\n"
            "\n"
            "  --force\n"
            "      Overwrite an existing config file. Without --force the command refuses\n"
            "      to overwrite an existing file.\n"
            "\n",
            argv0);
        help_device_selector();
        g_print(
            "Examples:\n"
            "  %s configure CB:81:F4:62:FF:07 --profile roland_gokeys\n"
            "  %s configure gokeys --profile roland_gokeys --print\n"
            "  %s configure CB:81:F4:62:FF:07 --profile roland_gokeys --force\n"
            "\n",
            argv0, argv0, argv0);
        return true;
    }

    if (g_strcmp0(cmd, "disconnect") == 0) {
        g_print(
            "Usage:\n"
            "  %s disconnect DEVICE\n"
            "\n"
            "Description:\n"
            "  Calls BlueZ Device1.Disconnect(). This disconnects the BlueZ device.\n"
            "  It does not yet coordinate with a running midi-ble-rtd session.\n"
            "\n",
            argv0);
        help_device_selector();
        return true;
    }

    if (g_strcmp0(cmd, "forget") == 0) {
        g_print(
            "Usage:\n"
            "  %s forget DEVICE --yes\n"
            "\n"
            "Description:\n"
            "  Disconnects the device if needed, then calls BlueZ Adapter1.RemoveDevice().\n"
            "  This removes BlueZ cache/pairing records for the device.\n"
            "\n"
            "Required option:\n"
            "  --yes\n"
            "      Required safety flag. Without it the command refuses to run.\n"
            "\n",
            argv0);
        help_device_selector();
        g_print(
            "Example:\n"
            "  %s forget CB:81:F4:62:FF:07 --yes\n"
            "\n",
            argv0);
        return true;
    }

    g_printerr("Unknown help topic: %s\n\n", cmd);
    help_global(argv0);
    return false;
}

static void usage(const char *argv0) {
    help_global(argv0);
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
        G_DBUS_CALL_FLAGS_NONE, MB_DBUS_GET_MANAGED_OBJECTS_TIMEOUT_MS, NULL, &error);

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

static ProfileKind guess_profile_kind(const char *name, const char *alias, GVariant *uuids) {
    if (contains_casefold(name, "go:keys") || contains_casefold(alias, "go:keys") ||
        contains_casefold(name, "go keys") || contains_casefold(alias, "go keys"))
        return PROFILE_ROLAND_GOKEYS;

    if (string_array_contains_uuid(uuids, BLE_MIDI_SERVICE_UUID))
        return PROFILE_STANDARD_BLE_MIDI;

    return PROFILE_UNKNOWN;
}

static const char *guess_profile(const char *name, const char *alias, GVariant *uuids) {
    ProfileKind p = guess_profile_kind(name, alias, uuids);
    if (p != PROFILE_UNKNOWN)
        return profile_name(p);

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
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, MB_CTL_ADAPTER_CALL_TIMEOUT_MS, NULL, &error);

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

static char *props_dup_string(GVariant *props, const char *key) {
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
    if (!v)
        return NULL;
    char *s = g_strdup(g_variant_get_string(v, NULL));
    g_variant_unref(v);
    return s;
}

static void print_device_row(GVariant *props) {
    GVariant *v_rssi = g_variant_lookup_value(props, "RSSI", G_VARIANT_TYPE_INT16);
    GVariant *v_paired = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_trusted = g_variant_lookup_value(props, "Trusted", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_connected = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_resolved = g_variant_lookup_value(props, "ServicesResolved", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    char *address = props_dup_string(props, "Address");
    char *name = props_dup_string(props, "Name");
    char *alias = props_dup_string(props, "Alias");

    bool paired = v_paired ? g_variant_get_boolean(v_paired) : false;
    bool trusted = v_trusted ? g_variant_get_boolean(v_trusted) : false;
    bool connected = v_connected ? g_variant_get_boolean(v_connected) : false;
    bool resolved = v_resolved ? g_variant_get_boolean(v_resolved) : false;
    const char *uuid_hint = string_array_contains_uuid(v_uuids, BLE_MIDI_SERVICE_UUID) ? "ble-midi" : "-";
    const char *profile = guess_profile(name ? name : "", alias ? alias : "", v_uuids);

    char rssi_buf[16];
    if (v_rssi)
        g_snprintf(rssi_buf, sizeof(rssi_buf), "%d", g_variant_get_int16(v_rssi));
    else
        g_snprintf(rssi_buf, sizeof(rssi_buf), "-");

    g_print("%-17s %-5s %-24.24s %-24.24s %-6s %-7s %-9s %-8s %-9s %s\n",
            address ? address : "-", rssi_buf, name ? name : "-", alias ? alias : "-",
            yesno(paired), yesno(trusted), yesno(connected), yesno(resolved),
            uuid_hint, profile);

    g_free(address);
    g_free(name);
    g_free(alias);
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

        char *name = props_dup_string(props, "Name");
        char *alias = props_dup_string(props, "Alias");
        GVariant *v_connected = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
        GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

        bool connected = v_connected ? g_variant_get_boolean(v_connected) : false;
        bool show = true;

        if (opts->midi_only && !has_midi_hint(name ? name : "", alias ? alias : "", v_uuids))
            show = false;
        if (opts->connected_only && !connected)
            show = false;

        if (show) {
            print_device_row(props);
            count++;
        }

        g_free(name);
        g_free(alias);
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

        char *address = props_dup_string(props, "Address");
        char *name = props_dup_string(props, "Name");
        char *alias = props_dup_string(props, "Alias");
        GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

        int score = -1;

        /*
         * CLI selector ranking.
         *
         * These weights decide which BlueZ Device1 object a user-supplied
         * selector refers to.  They are intentionally separated from daemon
         * discovery policy: this is interactive control-plane behavior.
         *
         * Priority model:
         *
         *   exact object path  >  exact Bluetooth address  >  name match  >  alias match
         *
         * A BLE-MIDI UUID bonus is small and additive.  It should prefer MIDI
         * devices among otherwise similar matches, but it must not override an
         * exact path or exact address selected by the user.
         */
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

        g_free(address);
        g_free(name);
        g_free(alias);
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

    char *address = props_dup_string(props, "Address");
    char *name = props_dup_string(props, "Name");
    char *alias = props_dup_string(props, "Alias");
    GVariant *v_rssi = g_variant_lookup_value(props, "RSSI", G_VARIANT_TYPE_INT16);
    GVariant *v_paired = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_trusted = g_variant_lookup_value(props, "Trusted", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_connected = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_resolved = g_variant_lookup_value(props, "ServicesResolved", G_VARIANT_TYPE_BOOLEAN);
    GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    g_print("Device: %s\n", path);
    g_print("  Address:          %s\n", address ? address : "-");
    g_print("  Name:             %s\n", name ? name : "-");
    g_print("  Alias:            %s\n", alias ? alias : "-");
    if (v_rssi)
        g_print("  RSSI:             %d\n", g_variant_get_int16(v_rssi));
    else
        g_print("  RSSI:             (none)\n");
    g_print("  Paired:           %s\n", yesno(v_paired ? g_variant_get_boolean(v_paired) : false));
    g_print("  Trusted:          %s\n", yesno(v_trusted ? g_variant_get_boolean(v_trusted) : false));
    g_print("  Connected:        %s\n", yesno(v_connected ? g_variant_get_boolean(v_connected) : false));
    g_print("  ServicesResolved: %s\n", yesno(v_resolved ? g_variant_get_boolean(v_resolved) : false));
    g_print("  Profile guess:    %s\n", guess_profile(name ? name : "", alias ? alias : "", v_uuids));
    g_print("  UUIDs:\n");
    print_string_array(v_uuids, "    ");

    g_free(address);
    g_free(name);
    g_free(alias);
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
        G_DBUS_CALL_FLAGS_NONE, MB_DBUS_DEVICE_PROPERTY_GET_TIMEOUT_MS, NULL, &error);

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

static char *get_device_object_path_property(Ctl *ctl, const char *path, const char *property) {
    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, PROPERTIES_IFACE, "Get",
        g_variant_new("(ss)", DEVICE_IFACE, property), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, MB_DBUS_DEVICE_PROPERTY_GET_TIMEOUT_MS, NULL, &error);

    if (!ret) {
        g_clear_error(&error);
        return NULL;
    }

    GVariant *value = NULL;
    g_variant_get(ret, "(v)", &value);
    char *out = g_strdup(g_variant_get_string(value, NULL));
    g_variant_unref(value);
    g_variant_unref(ret);
    return out;
}

static bool set_device_trusted(Ctl *ctl, const char *path, bool trusted) {
    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, PROPERTIES_IFACE, "Set",
        g_variant_new("(ssv)", DEVICE_IFACE, "Trusted", g_variant_new_boolean(trusted)),
        NULL, G_DBUS_CALL_FLAGS_NONE, MB_BLUEZ_SET_TRUSTED_TIMEOUT_MS, NULL, &error);

    if (!ret) {
        g_printerr("Set Device1.Trusted=%s failed: %s\n", yesno(trusted), error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Trusted=%s set for %s\n", yesno(trusted), path);
    return true;
}

static bool pair_device(Ctl *ctl, const char *path) {
    bool paired = false;
    if (get_device_bool(ctl, path, "Paired", &paired) && paired) {
        g_print("Device already paired.\n");
        return true;
    }

    GError *error = NULL;
    g_print("Calling Device1.Pair()...\n");

    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, DEVICE_IFACE, "Pair",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, MB_BLUEZ_PAIR_TIMEOUT_MS, NULL, &error);

    if (!ret) {
        const char *remote = g_dbus_error_get_remote_error(error);
        if (remote && g_strcmp0(remote, "org.bluez.Error.AlreadyExists") == 0) {
            g_clear_error(&error);
            g_print("Device is already paired according to BlueZ.\n");
            return true;
        }

        g_printerr("Device1.Pair failed: %s\n", error->message);
        g_printerr("If BlueZ asks for authorization, Agent1 support is the next implementation step.\n");
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Pair ok.\n");
    return true;
}

static bool connect_device(Ctl *ctl, const char *path) {
    bool connected = false;
    if (get_device_bool(ctl, path, "Connected", &connected) && connected) {
        g_print("Device already connected.\n");
        return true;
    }

    GError *error = NULL;
    g_print("Calling Device1.Connect()...\n");

    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, DEVICE_IFACE, "Connect",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, MB_CTL_BLUEZ_CONNECT_TIMEOUT_MS, NULL, &error);

    if (!ret) {
        const char *remote = g_dbus_error_get_remote_error(error);
        if (remote &&
            (g_strcmp0(remote, "org.bluez.Error.AlreadyConnected") == 0 ||
             g_strcmp0(remote, "org.bluez.Error.InProgress") == 0)) {
            g_clear_error(&error);
            g_print("Device already connected or connection in progress.\n");
            return true;
        }

        g_printerr("Device1.Connect failed: %s\n", error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Connect ok.\n");
    return true;
}

static bool disconnect_device(Ctl *ctl, const char *path) {
    bool connected = false;
    if (get_device_bool(ctl, path, "Connected", &connected) && !connected) {
        g_print("Device already disconnected.\n");
        return true;
    }

    GError *error = NULL;
    g_print("Calling Device1.Disconnect()...\n");

    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, path, DEVICE_IFACE, "Disconnect",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, MB_CTL_BLUEZ_DISCONNECT_TIMEOUT_MS, NULL, &error);

    if (!ret) {
        const char *remote = g_dbus_error_get_remote_error(error);
        if (remote && g_strcmp0(remote, "org.bluez.Error.NotConnected") == 0) {
            g_clear_error(&error);
            g_print("Device not connected.\n");
            return true;
        }

        g_printerr("Device1.Disconnect failed: %s\n", error->message);
        g_clear_error(&error);
        return false;
    }

    g_variant_unref(ret);
    g_print("Disconnect ok.\n");
    return true;
}

static bool wait_services_resolved(Ctl *ctl, const char *path, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        bool resolved = false;
        if (get_device_bool(ctl, path, "ServicesResolved", &resolved) && resolved)
            return true;
        g_usleep(MB_CTL_SERVICES_RESOLVED_POLL_INTERVAL_MS * G_TIME_SPAN_MILLISECOND);
        elapsed += MB_CTL_SERVICES_RESOLVED_POLL_INTERVAL_MS;
    }
    return false;
}

static bool remove_device(Ctl *ctl, const char *path) {
    char *adapter = get_device_object_path_property(ctl, path, "Adapter");
    if (!adapter)
        adapter = first_adapter_path(ctl);

    if (!adapter) {
        g_printerr("Cannot find adapter for device %s\n", path);
        return false;
    }

    GError *error = NULL;
    g_print("Calling Adapter1.RemoveDevice(%s) on %s...\n", path, adapter);

    GVariant *ret = g_dbus_connection_call_sync(
        ctl->bus, BLUEZ_BUS, adapter, ADAPTER_IFACE, "RemoveDevice",
        g_variant_new("(o)", path), NULL,
        G_DBUS_CALL_FLAGS_NONE, MB_CTL_BLUEZ_REMOVE_DEVICE_TIMEOUT_MS, NULL, &error);

    if (!ret) {
        g_printerr("Adapter1.RemoveDevice failed: %s\n", error->message);
        g_clear_error(&error);
        g_free(adapter);
        return false;
    }

    g_variant_unref(ret);
    g_free(adapter);
    g_print("RemoveDevice ok.\n");
    return true;
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

            /*
             * CLI GATT characteristic ranking.
             *
             * Mirrors the daemon-side BLE-MIDI characteristic heuristic for
             * diagnostics and probe output.  The numbers are ranking weights,
             * not timeout values.
             *
             * The official BLE-MIDI I/O UUID is preferred.  The Roland alias is
             * slightly weaker but still strong.  Notify and write-command are
             * the important dataplane capabilities.  Write-request/read are
             * minor compatibility hints.  A generic notify+write-command
             * characteristic gets a fallback score for non-standard devices.
             */
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

static bool validate_ble_midi_gatt(Ctl *ctl, const char *device_path, bool print_services) {
    if (!wait_services_resolved(ctl, device_path, MB_GATT_SERVICES_RESOLVED_TIMEOUT_MS)) {
        g_printerr("Timed out waiting for ServicesResolved=true.\n");
        return false;
    }

    g_print("ServicesResolved=true\n");
    if (print_services)
        g_print("GATT services under device:\n");

    char *service_path = find_ble_midi_service(ctl, device_path, print_services);
    if (!service_path) {
        g_printerr("BLE-MIDI service not found. For Roland GO:KEYS, connect MIDI before Audio/A2DP.\n");
        return false;
    }

    g_print("Selected BLE-MIDI service: %s\n", service_path);
    g_print("GATT characteristics under BLE-MIDI service:\n");

    char *char_path = find_best_midi_characteristic(ctl, device_path, service_path);
    if (!char_path) {
        g_printerr("No usable BLE-MIDI I/O characteristic found.\n");
        g_free(service_path);
        return false;
    }

    g_print("Selected BLE-MIDI I/O characteristic: %s\n", char_path);
    g_free(char_path);
    g_free(service_path);
    return true;
}

static int cmd_probe(Ctl *ctl, const char *selector) {
    char *device_path = find_device_path(ctl, selector);
    if (!device_path) {
        g_printerr("Device not found: %s\n", selector);
        g_printerr("Run `midi-ble-rtctl scan --midi-only` first.\n");
        return 1;
    }

    g_print("Probe device: %s\n", device_path);

    if (!connect_device(ctl, device_path)) {
        g_free(device_path);
        return 1;
    }

    bool ok = validate_ble_midi_gatt(ctl, device_path, true);
    if (ok)
        g_print("Status: usable for midi-ble-rtd StartNotify path.\n");

    g_free(device_path);
    return ok ? 0 : 1;
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
        g_usleep(MB_CTL_SCAN_SLEEP_INTERVAL_MS * G_TIME_SPAN_MILLISECOND);

    adapter_call(ctl, adapter, "StopDiscovery");
    g_free(adapter);

    return cmd_list(ctl, opts);
}

static int cmd_pair(Ctl *ctl, const char *selector) {
    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        return 1;
    }
    bool ok = pair_device(ctl, path);
    g_free(path);
    return ok ? 0 : 1;
}

static int cmd_trust(Ctl *ctl, const char *selector, bool trusted) {
    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        return 1;
    }
    bool ok = set_device_trusted(ctl, path, trusted);
    g_free(path);
    return ok ? 0 : 1;
}

static int cmd_disconnect(Ctl *ctl, const char *selector) {
    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        return 1;
    }
    bool ok = disconnect_device(ctl, path);
    g_free(path);
    return ok ? 0 : 1;
}

static int cmd_forget(Ctl *ctl, const char *selector, bool yes) {
    if (!yes) {
        g_printerr("Refusing to forget without --yes. This removes the device from BlueZ.\n");
        return 2;
    }

    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        return 1;
    }

    disconnect_device(ctl, path);
    bool ok = remove_device(ctl, path);
    g_free(path);
    return ok ? 0 : 1;
}

static ProfileKind infer_device_profile(Ctl *ctl, const char *path) {
    GVariant *props = device_props(ctl, path);
    if (!props)
        return PROFILE_UNKNOWN;

    char *name = props_dup_string(props, "Name");
    char *alias = props_dup_string(props, "Alias");
    GVariant *v_uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));

    ProfileKind p = guess_profile_kind(name ? name : "", alias ? alias : "", v_uuids);

    g_free(name);
    g_free(alias);
    if (v_uuids) g_variant_unref(v_uuids);
    g_variant_unref(props);
    return p;
}

static char *default_config_path(ProfileKind profile) {
    const char *dir = g_get_user_config_dir();
    if (profile == PROFILE_ROLAND_GOKEYS)
        return g_build_filename(dir, "midi-ble-rt", "roland-gokeys.ini", NULL);
    if (profile == PROFILE_STANDARD_BLE_MIDI)
        return g_build_filename(dir, "midi-ble-rt", "standard-ble-midi.ini", NULL);
    return g_build_filename(dir, "midi-ble-rt", "device.ini", NULL);
}

static char *config_text_for_device(Ctl *ctl, const char *device_path, ProfileKind profile) {
    GVariant *props = device_props(ctl, device_path);
    if (!props)
        return NULL;

    char *address = props_dup_string(props, "Address");
    char *name = props_dup_string(props, "Name");
    char *alias = props_dup_string(props, "Alias");
    if (!address)
        address = g_strdup("UNKNOWN");
    if (!name)
        name = alias ? g_strdup(alias) : g_strdup("BLE-MIDI device");

    const char *port_name = profile == PROFILE_ROLAND_GOKEYS
        ? "Roland GO:KEYS BLE-MIDI"
        : "BLE-MIDI Device";
    const char *io_alias_line = profile == PROFILE_ROLAND_GOKEYS
        ? "io_uuid_alias = " ROLAND_GOKEYS_IO_ALIAS "\n"
        : "# io_uuid_alias =\n";

    char *text = g_strdup_printf(
        "[device]\n"
        "# Written by midi-ble-rtctl configure.\n"
        "address = %s\n"
        "name = %s\n"
        "profile = %s\n"
        "connect_order = midi-first\n"
        "pair = no\n"
        "trust = yes\n"
        "auto_reconnect = yes\n"
        "\n"
        "[gatt]\n"
        "service_uuid = " BLE_MIDI_SERVICE_UUID "\n"
        "io_uuid = " BLE_MIDI_IO_UUID "\n"
        "%s"
        "require_notify = yes\n"
        "require_write_without_response = yes\n"
        "\n"
        "[alsa]\n"
        "client_name = midi-ble-rt\n"
        "port_name = %s\n"
        "\n"
        "[debug]\n"
        "print_ble_packets = yes\n"
        "print_midi_events = no\n",
        address, name, profile_name(profile), io_alias_line, port_name);

    g_free(address);
    g_free(name);
    g_free(alias);
    g_variant_unref(props);
    return text;
}

static bool write_config_file(const char *path, const char *text, bool force) {
    if (!force && g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_printerr("Config already exists: %s\n", path);
        g_printerr("Use --force to overwrite, or --output PATH to write elsewhere.\n");
        return false;
    }

    char *dir = g_path_get_dirname(path);
    GError *error = NULL;
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_printerr("Failed to create config dir: %s\n", dir);
        g_free(dir);
        return false;
    }
    g_free(dir);

    if (!g_file_set_contents(path, text, -1, &error)) {
        g_printerr("Failed to write %s: %s\n", path, error->message);
        g_clear_error(&error);
        return false;
    }

    g_print("written: %s\n", path);
    return true;
}

static int cmd_configure(Ctl *ctl, const char *selector, const ConfigureOptions *opts) {
    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        g_printerr("Run `midi-ble-rtctl scan --midi-only` first.\n");
        return 1;
    }

    ProfileKind profile = opts->profile != PROFILE_UNKNOWN ? opts->profile : infer_device_profile(ctl, path);
    if (profile == PROFILE_UNKNOWN) {
        g_printerr("Could not infer profile. Use --profile roland_gokeys or --profile standard_ble_midi.\n");
        g_free(path);
        return 2;
    }

    char *text = config_text_for_device(ctl, path, profile);
    if (!text) {
        g_printerr("Could not build config for %s\n", path);
        g_free(path);
        return 1;
    }

    if (opts->print_only) {
        g_print("%s", text);
        g_free(text);
        g_free(path);
        return 0;
    }

    char *out = opts->output_path ? g_strdup(opts->output_path) : default_config_path(profile);
    bool ok = write_config_file(out, text, opts->force);
    if (ok) {
        g_print("profile: %s\n", profile_name(profile));
        g_print("next: ./build/midi-ble-rtd --config %s\n", out);
    }

    g_free(out);
    g_free(text);
    g_free(path);
    return ok ? 0 : 1;
}

static int cmd_connect(Ctl *ctl, const char *selector, const ConnectOptions *opts) {
    char *path = find_device_path(ctl, selector);
    if (!path) {
        g_printerr("Device not found: %s\n", selector);
        g_printerr("Run `midi-ble-rtctl scan --midi-only` first.\n");
        return 1;
    }

    ProfileKind profile = opts->profile != PROFILE_UNKNOWN ? opts->profile : infer_device_profile(ctl, path);
    g_print("Connect device: %s\n", path);
    g_print("Profile: %s\n", profile_name(profile));

    bool should_pair = opts->force_pair;
    if (profile == PROFILE_ROLAND_GOKEYS)
        should_pair = true;
    if (opts->no_pair)
        should_pair = false;

    bool should_trust = !opts->no_trust;
    if (profile == PROFILE_UNKNOWN && !opts->force_pair)
        g_print("Profile unknown; connect will avoid forced pair unless --pair is given.\n");

    if (should_pair && !pair_device(ctl, path)) {
        g_free(path);
        return 1;
    }

    if (should_trust && !set_device_trusted(ctl, path, true)) {
        g_free(path);
        return 1;
    }

    if (!connect_device(ctl, path)) {
        g_free(path);
        return 1;
    }

    bool ok = true;
    if (!opts->no_probe)
        ok = validate_ble_midi_gatt(ctl, path, false);

    if (ok && opts->write_config) {
        ConfigureOptions c = {0};
        c.profile = profile;
        c.output_path = opts->output_path;
        c.force = true;
        c.print_only = false;
        ok = cmd_configure(ctl, selector, &c) == 0;
    }

    if (ok) {
        g_print("BlueZ connection ready.\n");
        g_print("Data plane is still started separately, for now: midi-ble-rtd --config <file>\n");
    }

    g_free(path);
    return ok ? 0 : 1;
}

static bool any_help_arg(int argc, char **argv, int first) {
    for (int i = first; i < argc; i++) {
        if (is_help_arg(argv[i]))
            return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (is_help_arg(argv[1])) {
        help_global(argv[0]);
        return 0;
    }

    if (g_strcmp0(argv[1], "help") == 0) {
        if (argc == 2)
            help_global(argv[0]);
        else if (argc == 3)
            return help_command(argv[0], argv[2]) ? 0 : 2;
        else {
            g_printerr("Usage: %s help [COMMAND]\n", argv[0]);
            return 2;
        }
        return 0;
    }

    const char *cmd = argv[1];
    if (any_help_arg(argc, argv, 2))
        return help_command(argv[0], cmd) ? 0 : 2;

    GError *error = NULL;
    Ctl ctl = {0};
    ctl.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!ctl.bus) {
        g_printerr("Could not connect to system bus: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

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
    } else if (g_strcmp0(cmd, "pair") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        rc = cmd_pair(&ctl, argv[2]);
    } else if (g_strcmp0(cmd, "trust") == 0 || g_strcmp0(cmd, "untrust") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        rc = cmd_trust(&ctl, argv[2], g_strcmp0(cmd, "trust") == 0);
    } else if (g_strcmp0(cmd, "connect") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        ConnectOptions opts = {0};
        for (int i = 3; i < argc; i++) {
            if (g_strcmp0(argv[i], "--profile") == 0 && i + 1 < argc) {
                opts.profile = profile_from_string(argv[++i]);
                if (opts.profile == PROFILE_UNKNOWN) {
                    g_printerr("Unknown profile: %s\n", argv[i]);
                    rc = 2;
                    goto out;
                }
            } else if (g_strcmp0(argv[i], "--pair") == 0) {
                opts.force_pair = true;
            } else if (g_strcmp0(argv[i], "--no-pair") == 0) {
                opts.no_pair = true;
            } else if (g_strcmp0(argv[i], "--no-trust") == 0) {
                opts.no_trust = true;
            } else if (g_strcmp0(argv[i], "--no-probe") == 0) {
                opts.no_probe = true;
            } else if (g_strcmp0(argv[i], "--write-config") == 0) {
                opts.write_config = true;
            } else if (g_strcmp0(argv[i], "--output") == 0 && i + 1 < argc) {
                opts.output_path = argv[++i];
            } else {
                usage(argv[0]);
                rc = 2;
                goto out;
            }
        }
        rc = cmd_connect(&ctl, argv[2], &opts);
    } else if (g_strcmp0(cmd, "configure") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        ConfigureOptions opts = {0};
        for (int i = 3; i < argc; i++) {
            if (g_strcmp0(argv[i], "--profile") == 0 && i + 1 < argc) {
                opts.profile = profile_from_string(argv[++i]);
                if (opts.profile == PROFILE_UNKNOWN) {
                    g_printerr("Unknown profile: %s\n", argv[i]);
                    rc = 2;
                    goto out;
                }
            } else if (g_strcmp0(argv[i], "--output") == 0 && i + 1 < argc) {
                opts.output_path = argv[++i];
            } else if (g_strcmp0(argv[i], "--print") == 0) {
                opts.print_only = true;
            } else if (g_strcmp0(argv[i], "--force") == 0) {
                opts.force = true;
            } else {
                usage(argv[0]);
                rc = 2;
                goto out;
            }
        }
        rc = cmd_configure(&ctl, argv[2], &opts);
    } else if (g_strcmp0(cmd, "disconnect") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        rc = cmd_disconnect(&ctl, argv[2]);
    } else if (g_strcmp0(cmd, "forget") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            rc = 2;
            goto out;
        }
        bool yes = false;
        for (int i = 3; i < argc; i++) {
            if (g_strcmp0(argv[i], "--yes") == 0)
                yes = true;
            else {
                usage(argv[0]);
                rc = 2;
                goto out;
            }
        }
        rc = cmd_forget(&ctl, argv[2], yes);
    } else {
        usage(argv[0]);
        rc = 2;
    }

out:
    g_object_unref(ctl.bus);
    return rc;
}
