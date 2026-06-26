#ifndef MB_BLUEZ_H
#define MB_BLUEZ_H

/*
 * BlueZ D-Bus names used by midi-ble-rt.
 *
 * This header is the first extraction point from the legacy daemon core.
 * Keep it limited to BlueZ/ObjectManager/Properties/GATT constants for now.
 * Function extraction should happen in later cleanup commits.
 */

#define BLUEZ_BUS              "org.bluez"
#define OBJECT_MANAGER_IFACE   "org.freedesktop.DBus.ObjectManager"
#define PROPERTIES_IFACE       "org.freedesktop.DBus.Properties"

#define DEVICE_IFACE           "org.bluez.Device1"
#define GATT_SERVICE_IFACE     "org.bluez.GattService1"
#define GATT_CHRC_IFACE        "org.bluez.GattCharacteristic1"

#endif /* MB_BLUEZ_H */
