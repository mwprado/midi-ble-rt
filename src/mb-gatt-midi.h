#ifndef MB_GATT_MIDI_H
#define MB_GATT_MIDI_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

char *mb_gatt_midi_find_service(GDBusConnection *bus,
                                const char *device_path,
                                const char *service_uuid);

char *mb_gatt_midi_find_characteristic(GDBusConnection *bus,
                                       const char *device_path,
                                       const char *service_path,
                                       const char *io_uuid,
                                       const char *io_uuid_alias);

bool mb_gatt_midi_write_value_command(GDBusConnection *bus,
                                      const char *char_path,
                                      const uint8_t *packet,
                                      size_t packet_len,
                                      int timeout_ms);

bool mb_gatt_midi_start_notify(GDBusConnection *bus,
                               const char *char_path,
                               int timeout_ms,
                               GError **error);

#endif /* MB_GATT_MIDI_H */
