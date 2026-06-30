#ifndef MB_BLE_MIDI_H
#define MB_BLE_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_BLE_MIDI_SERVICE_UUID "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define MB_BLE_MIDI_IO_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"
#define MB_BLE_MIDI_ROLAND_IO_UUID_ALIAS "00006bf3-0000-1000-8000-00805f9b34fb"

#define MB_BLE_MIDI_PACKET_MAX_BYTES 20
#define MB_BLE_MIDI_MAX_MIDI_BYTES_PER_PACKET (MB_BLE_MIDI_PACKET_MAX_BYTES - 2)

typedef struct {
    uint8_t running_status;
} MbBleMidiDecoderState;

typedef enum {
    MB_BLE_MIDI_DECODE_OK,
    MB_BLE_MIDI_DECODE_IGNORED,
    MB_BLE_MIDI_DECODE_INVALID_HEADER,
} MbBleMidiDecodeResult;

typedef void (*MbBleMidiEmitByteFunc)(uint8_t byte, void *user_data);

int mb_ble_midi_status_data_len(uint8_t status);
bool mb_ble_midi_packet_has_valid_header(const uint8_t *packet, size_t len);
void mb_ble_midi_decoder_reset(MbBleMidiDecoderState *state);
MbBleMidiDecodeResult mb_ble_midi_decode_packet(MbBleMidiDecoderState *state,
                                                const uint8_t *packet,
                                                size_t len,
                                                MbBleMidiEmitByteFunc emit_byte,
                                                void *user_data);
bool mb_ble_midi_make_packet(uint16_t timestamp_13bit,
                             const uint8_t *midi,
                             size_t midi_len,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *out_len);

#endif
