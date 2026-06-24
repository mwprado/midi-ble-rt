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
