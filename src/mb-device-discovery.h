#ifndef MB_DEVICE_DISCOVERY_H
#define MB_DEVICE_DISCOVERY_H

#include <gio/gio.h>

char *mb_device_discovery_find(GDBusConnection *bus,
                               const char *address_filter,
                               const char *name_filter,
                               const char *service_uuid);

#endif /* MB_DEVICE_DISCOVERY_H */
