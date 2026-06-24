#ifndef MB_BLE_MIDI_H
#define MB_BLE_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_BLE_MIDI_SERVICE_UUID "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define MB_BLE_MIDI_IO_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"
#define MB_BLE_MIDI_ROLAND_IO_UUID_ALIAS "00006bf3-0000-1000-8000-00805f9b34fb"

int mb_ble_midi_status_data_len(uint8_t status);
bool mb_ble_midi_packet_has_valid_header(const uint8_t *packet, size_t len);
bool mb_ble_midi_make_packet(uint16_t timestamp_13bit,
                             const uint8_t *midi,
                             size_t midi_len,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *out_len);

#endif
