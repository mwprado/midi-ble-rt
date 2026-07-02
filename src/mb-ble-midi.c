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

static bool mb_ble_midi_is_realtime(uint8_t status) {
    return status >= 0xF8;
}

static void mb_ble_midi_emit_pending(MbBleMidiDecoderState *state,
                                     MbBleMidiEmitByteFunc emit_byte,
                                     void *user_data) {
    emit_byte(state->pending_status, user_data);
    for (uint8_t i = 0; i < state->pending_len; i++)
        emit_byte(state->pending_data[i], user_data);

    state->pending_status = 0;
    state->pending_len = 0;
    state->pending_expected = 0;
}

static void mb_ble_midi_start_pending(MbBleMidiDecoderState *state,
                                      uint8_t status,
                                      uint8_t expected) {
    state->pending_status = status;
    state->pending_len = 0;
    state->pending_expected = expected;
}

static MbBleMidiDecodeResult mb_ble_midi_resync(MbBleMidiDecoderState *state) {
    mb_ble_midi_decoder_reset(state);
    return MB_BLE_MIDI_DECODE_RESYNC;
}

static MbBleMidiDecodeResult mb_ble_midi_consume_pending_data(MbBleMidiDecoderState *state,
                                                              const uint8_t *packet,
                                                              size_t len,
                                                              size_t *index,
                                                              uint8_t first_data,
                                                              MbBleMidiEmitByteFunc emit_byte,
                                                              void *user_data) {
    uint8_t b = first_data;

    for (;;) {
        if (b >= 0x80) {
            if (mb_ble_midi_is_realtime(b)) {
                emit_byte(b, user_data);
            } else {
                return mb_ble_midi_resync(state);
            }
        } else {
            if (state->pending_len >= sizeof(state->pending_data))
                return mb_ble_midi_resync(state);

            state->pending_data[state->pending_len++] = b;
            if (state->pending_len == state->pending_expected) {
                mb_ble_midi_emit_pending(state, emit_byte, user_data);
                return MB_BLE_MIDI_DECODE_OK;
            }
        }

        if (*index >= len)
            return MB_BLE_MIDI_DECODE_OK;

        b = packet[(*index)++];
    }
}

static MbBleMidiDecodeResult mb_ble_midi_consume_sysex(MbBleMidiDecoderState *state,
                                                       const uint8_t *packet,
                                                       size_t len,
                                                       size_t *index,
                                                       uint8_t first_byte,
                                                       MbBleMidiEmitByteFunc emit_byte,
                                                       void *user_data) {
    uint8_t b = first_byte;

    for (;;) {
        if (b == 0xF7) {
            emit_byte(b, user_data);
            state->sysex_active = false;
            return MB_BLE_MIDI_DECODE_OK;
        }

        if (b >= 0x80) {
            if (mb_ble_midi_is_realtime(b)) {
                emit_byte(b, user_data);
            } else {
                return mb_ble_midi_resync(state);
            }
        } else {
            emit_byte(b, user_data);
        }

        if (*index >= len)
            return MB_BLE_MIDI_DECODE_OK;

        b = packet[(*index)++];
    }
}

MbBleMidiDecodeResult mb_ble_midi_decode_packet(MbBleMidiDecoderState *state,
                                                const uint8_t *packet,
                                                size_t len,
                                                MbBleMidiEmitByteFunc emit_byte,
                                                void *user_data) {
    if (!state || !packet || !emit_byte || len < 3)
        return MB_BLE_MIDI_DECODE_IGNORED;

    if (!mb_ble_midi_packet_has_valid_header(packet, len)) {
        mb_ble_midi_decoder_reset(state);
        return MB_BLE_MIDI_DECODE_INVALID_HEADER;
    }

    /*
     * BLE notification boundaries are transport boundaries, not MIDI message
     * boundaries.  This decoder is intentionally incremental: short channel
     * messages, running-status data and SysEx payloads may continue in the next
     * BLE packet.
     *
     * Resync policy is deliberately fail-closed.  A data byte is accepted only
     * when it belongs to an incomplete message, to running status, or to an
     * active SysEx.  If the stream becomes ambiguous, reset the decoder and let
     * the caller emit a MIDI panic if this RX path may have left notes or
     * controllers logically pressed.
     */
    size_t i = 1;
    while (i < len) {
        uint8_t timestamp = packet[i++];
        if ((timestamp & 0x80) == 0)
            return mb_ble_midi_resync(state);

        if (i >= len)
            break;

        uint8_t b = packet[i++];

        if (state->sysex_active) {
            MbBleMidiDecodeResult r = mb_ble_midi_consume_sysex(state, packet, len, &i, b,
                                                                emit_byte, user_data);
            if (r != MB_BLE_MIDI_DECODE_OK)
                return r;
            continue;
        }

        if (state->pending_status) {
            MbBleMidiDecodeResult r = mb_ble_midi_consume_pending_data(state, packet, len, &i, b,
                                                                       emit_byte, user_data);
            if (r != MB_BLE_MIDI_DECODE_OK)
                return r;
            continue;
        }

        if (b < 0x80) {
            if (state->running_status >= 0x80) {
                int expected = mb_ble_midi_status_data_len(state->running_status);
                if (expected <= 0)
                    return mb_ble_midi_resync(state);

                mb_ble_midi_start_pending(state, state->running_status, (uint8_t)expected);
                MbBleMidiDecodeResult r = mb_ble_midi_consume_pending_data(state, packet, len, &i, b,
                                                                           emit_byte, user_data);
                if (r != MB_BLE_MIDI_DECODE_OK)
                    return r;
                continue;
            }

            return mb_ble_midi_resync(state);
        }

        if (mb_ble_midi_is_realtime(b)) {
            emit_byte(b, user_data);
            continue;
        }

        if (b == 0xF0) {
            state->running_status = 0;
            state->sysex_active = true;
            emit_byte(b, user_data);

            if (i < len) {
                MbBleMidiDecodeResult r = mb_ble_midi_consume_sysex(state, packet, len, &i,
                                                                    packet[i++],
                                                                    emit_byte, user_data);
                if (r != MB_BLE_MIDI_DECODE_OK)
                    return r;
            }
            continue;
        }

        if (b == 0xF7)
            return mb_ble_midi_resync(state);

        int expected = mb_ble_midi_status_data_len(b);
        if (expected < 0)
            return mb_ble_midi_resync(state);

        if (b < 0xF0)
            state->running_status = b;
        else
            state->running_status = 0;

        if (expected == 0) {
            emit_byte(b, user_data);
            continue;
        }

        mb_ble_midi_start_pending(state, b, (uint8_t)expected);
        if (i < len) {
            MbBleMidiDecodeResult r = mb_ble_midi_consume_pending_data(state, packet, len, &i,
                                                                       packet[i++],
                                                                       emit_byte, user_data);
            if (r != MB_BLE_MIDI_DECODE_OK)
                return r;
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
