#ifndef MB_BLUEZ_H
#define MB_BLUEZ_H

#include <gio/gio.h>

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

#endif /* MB_BLUEZ_H */
