#include "mb-ble-midi.h"

#include <string.h>

int mb_ble_midi_status_data_len(uint8_t status) {
    if (status < 0x80) return -1;
    if (status >= 0x80 && status <= 0xBF) return 2;
    if (status >= 0xC0 && status <= 0xDF) return 1;
    if (status >= 0xE0 && status <= 0xEF) return 2;

    switch (status) {
        case 0xF1:
        case 0xF3:
            return 1;
        case 0xF2:
            return 2;
        case 0xF6:
        case 0xF7:
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            return 0;
        case 0xF0:
            return -2;
        default:
            return -1;
    }
}

bool mb_ble_midi_packet_has_valid_header(const uint8_t *packet, size_t len) {
    return packet && len >= 1 && (packet[0] & 0x80) != 0;
}

void mb_ble_midi_decoder_reset(MbBleMidiDecoderState *state) {
    if (state)
        memset(state, 0, sizeof(*state));
}

MbBleMidiDecodeResult mb_ble_midi_decode_packet(MbBleMidiDecoderState *state,
                                                const uint8_t *packet,
                                                size_t len,
                                                MbBleMidiEmitByteFunc emit_byte,
                                                void *user_data) {
    if (!state || !packet || !emit_byte || len < 3)
        return MB_BLE_MIDI_DECODE_IGNORED;

    if (!mb_ble_midi_packet_has_valid_header(packet, len))
        return MB_BLE_MIDI_DECODE_INVALID_HEADER;

    /*
     * Canonical decoder loop extracted from the threaded multi-device
     * runtime path. Keep App/orchestrator and daemon dataplane consumers
     * on this implementation so running-status and SysEx behavior do not
     * diverge again.
     */
    size_t i = 1;
    while (i < len) {
        uint8_t ts = packet[i];
        if ((ts & 0x80) == 0) {
            i++;
            continue;
        }

        i++;
        if (i >= len)
            break;

        uint8_t status = packet[i];
        if (status < 0x80) {
            if (state->running_status >= 0x80) {
                int n = mb_ble_midi_status_data_len(state->running_status);
                if (n > 0 && i + (size_t)n <= len) {
                    emit_byte(state->running_status, user_data);
                    for (int j = 0; j < n; j++)
                        emit_byte(packet[i++], user_data);
                    continue;
                }
            }
            i++;
            continue;
        }

        i++;
        emit_byte(status, user_data);

        if (status < 0xF0)
            state->running_status = status;
        else if (status != 0xF7)
            state->running_status = 0;

        int data_len = mb_ble_midi_status_data_len(status);
        if (data_len >= 0) {
            for (int j = 0; j < data_len && i < len; j++) {
                if (packet[i] & 0x80)
                    break;
                emit_byte(packet[i++], user_data);
            }
            continue;
        }

        if (data_len == -2) {
            while (i < len) {
                emit_byte(packet[i], user_data);
                if (packet[i] == 0xF7) {
                    i++;
                    break;
                }
                i++;
            }
        }
    }

    return MB_BLE_MIDI_DECODE_OK;
}

bool mb_ble_midi_make_packet(uint16_t timestamp_13bit,
                             const uint8_t *midi,
                             size_t midi_len,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *out_len) {
    if (out_len)
        *out_len = 0;

    if (!midi || !out || midi_len == 0)
        return false;

    if (midi_len + 2 > out_cap)
        return false;

    uint16_t ts = timestamp_13bit & 0x1fff;
    out[0] = 0x80 | ((ts >> 7) & 0x3f);
    out[1] = 0x80 | (ts & 0x7f);
    memcpy(&out[2], midi, midi_len);

    if (out_len)
        *out_len = midi_len + 2;

    return true;
}
