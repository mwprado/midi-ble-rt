#ifndef MB_GATT_MIDI_H
#define MB_GATT_MIDI_H

#include <gio/gio.h>

char *mb_gatt_midi_find_service(GDBusConnection *bus,
                                const char *device_path,
                                const char *service_uuid);

char *mb_gatt_midi_find_characteristic(GDBusConnection *bus,
                                       const char *device_path,
                                       const char *service_path,
                                       const char *io_uuid,
                                       const char *io_uuid_alias);

#endif /* MB_GATT_MIDI_H */
