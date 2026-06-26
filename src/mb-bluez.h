#ifndef MB_BLUEZ_H
#define MB_BLUEZ_H

#include <gio/gio.h>
#include <stdbool.h>

/*
 * BlueZ D-Bus names used by midi-ble-rt.
 *
 * This header is the first extraction point from the legacy daemon core.
 * Keep it limited to BlueZ/ObjectManager/Properties/GATT constants and
 * low-level BlueZ D-Bus helpers.
 */

#define BLUEZ_BUS              "org.bluez"
#define OBJECT_MANAGER_IFACE   "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES_IFACE       "org.freedesktop.DBus.Properties"

#define DEVICE_IFACE           "org.bluez.Device1"
#define GATT_SERVICE_IFACE     "org.bluez.GattService1"
#define GATT_CHRC_IFACE        "org.bluez.GattCharacteristic1"

GVariant *mb_bluez_get_managed_objects(GDBusConnection *bus);
bool mb_bluez_get_device_bool_property(GDBusConnection *bus, const char *device_path, const char *property, bool *out);
bool mb_bluez_set_device_trusted(GDBusConnection *bus, const char *device_path);
bool mb_bluez_pair_device(GDBusConnection *bus, const char *device_path);
bool mb_bluez_connect_device(GDBusConnection *bus, const char *device_path);
bool mb_bluez_wait_services_resolved(GDBusConnection *bus, const char *device_path, int timeout_ms);

#endif /* MB_BLUEZ_H */
