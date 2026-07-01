#ifndef MB_TIMEOUTS_H
#define MB_TIMEOUTS_H

/*
 * Operational timeouts and periodic intervals, in milliseconds.
 *
 * Keep semantic constants here. Do not use this header as a generic
 * dumping ground for unrelated sizes, counters or protocol values.
 */

#define MB_DBUS_GET_MANAGED_OBJECTS_TIMEOUT_MS        10000
#define MB_DBUS_DEVICE_PROPERTY_GET_TIMEOUT_MS         5000

#define MB_BLUEZ_SET_TRUSTED_TIMEOUT_MS                5000
#define MB_BLUEZ_PAIR_TIMEOUT_MS                      60000
#define MB_BLUEZ_CONNECT_TIMEOUT_MS                   20000
#define MB_BLUEZ_DISCONNECT_TIMEOUT_MS                10000

#define MB_BLUEZ_CONNECTED_WAIT_TIMEOUT_MS            15000
#define MB_BLUEZ_CONNECTED_POLL_INTERVAL_MS             250
#define MB_BLUEZ_AMBIGUOUS_TIMEOUT_GRACE_MS            3000
#define MB_BLUEZ_RESET_AFTER_FAILURE_DELAY_MS          1000

#define MB_GATT_SERVICES_RESOLVED_TIMEOUT_MS          15000
#define MB_GATT_SERVICES_RESOLVED_POLL_INTERVAL_MS      100
#define MB_GATT_START_NOTIFY_TIMEOUT_MS               15000
#define MB_GATT_WRITE_VALUE_TIMEOUT_MS                 5000

#define MB_ALSA_RX_POLL_INTERVAL_MS                       1
#define MB_ALSA_TX_POLL_TIMEOUT_MS                      100
#define MB_DEVICE_HEALTH_INTERVAL_MS                   1000
#define MB_RECONNECT_INTERVAL_MS                       10000

#define MB_STATS_DEFAULT_INTERVAL_MS                    1000
#define MB_STATS_CTL_DEFAULT_INTERVAL_MS                1000
#define MB_STATS_CTL_MIN_INTERVAL_MS                     100
#define MB_STATS_CTL_MAX_INTERVAL_MS                   60000
#define MB_CONFIG_MAX_INTERVAL_MS                      60000

#define MB_CTL_ADAPTER_CALL_TIMEOUT_MS              10000
#define MB_CTL_BLUEZ_CONNECT_TIMEOUT_MS             30000
#define MB_CTL_BLUEZ_DISCONNECT_TIMEOUT_MS          15000
#define MB_CTL_BLUEZ_REMOVE_DEVICE_TIMEOUT_MS       15000
#define MB_CTL_SERVICES_RESOLVED_POLL_INTERVAL_MS     100
#define MB_CTL_SCAN_SLEEP_INTERVAL_MS                1000

#endif /* MB_TIMEOUTS_H */
