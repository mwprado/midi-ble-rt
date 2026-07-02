#include "mb-ble-midi.h"

#include <glib.h>

#include <string.h>

typedef struct {
    uint8_t bytes[256];
    size_t len;
} ByteSink;

static void sink_emit_byte(uint8_t byte, void *user_data) {
    ByteSink *sink = user_data;
    g_assert_cmpuint(sink->len, <, sizeof(sink->bytes));
    sink->bytes[sink->len++] = byte;
}

static void sink_assert_bytes(const ByteSink *sink, const uint8_t *expected, size_t len) {
    g_assert_cmpuint(sink->len, ==, len);
    for (size_t i = 0; i < len; i++)
        g_assert_cmphex(sink->bytes[i], ==, expected[i]);
}

static void test_status_data_lengths(void) {
    g_assert_cmpint(mb_ble_midi_status_data_len(0x90), ==, 2);
    g_assert_cmpint(mb_ble_midi_status_data_len(0x80), ==, 2);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xB0), ==, 2);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xC0), ==, 1);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xD0), ==, 1);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xE0), ==, 2);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xF1), ==, 1);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xF2), ==, 2);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xF6), ==, 0);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xF8), ==, 0);
    g_assert_cmpint(mb_ble_midi_status_data_len(0xF0), ==, -2);
    g_assert_cmpint(mb_ble_midi_status_data_len(0x40), ==, -1);
}

static void test_packet_header_validation(void) {
    const uint8_t valid[] = {0x80};
    const uint8_t invalid[] = {0x00};

    g_assert_true(mb_ble_midi_packet_has_valid_header(valid, sizeof(valid)));
    g_assert_false(mb_ble_midi_packet_has_valid_header(invalid, sizeof(invalid)));
    g_assert_false(mb_ble_midi_packet_has_valid_header(NULL, 1));
    g_assert_false(mb_ble_midi_packet_has_valid_header(valid, 0));
}

static void test_make_packet(void) {
    const uint8_t midi[] = {0x90, 0x40, 0x7f};
    uint8_t out[8] = {0};
    size_t out_len = 0;

    g_assert_true(mb_ble_midi_make_packet(0x1234, midi, sizeof(midi), out, sizeof(out), &out_len));
    g_assert_cmpuint(out_len, ==, 5);
    g_assert_cmphex(out[0], ==, 0xa4);
    g_assert_cmphex(out[1], ==, 0xb4);
    g_assert_cmphex(out[2], ==, 0x90);
    g_assert_cmphex(out[3], ==, 0x40);
    g_assert_cmphex(out[4], ==, 0x7f);
}

static void test_make_packet_rejects_invalid_inputs(void) {
    const uint8_t midi[] = {0x90, 0x40, 0x7f};
    uint8_t out[4] = {0};
    size_t out_len = 123;

    g_assert_false(mb_ble_midi_make_packet(0, midi, sizeof(midi), out, sizeof(out), &out_len));
    g_assert_cmpuint(out_len, ==, 0);
    g_assert_false(mb_ble_midi_make_packet(0, NULL, sizeof(midi), out, sizeof(out), &out_len));
    g_assert_false(mb_ble_midi_make_packet(0, midi, 0, out, sizeof(out), &out_len));
    g_assert_false(mb_ble_midi_make_packet(0, midi, sizeof(midi), NULL, sizeof(out), &out_len));
}

static void test_decode_complete_note_on(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t packet[] = {0x80, 0x80, 0x90, 0x3c, 0x7f};
    const uint8_t expected[] = {0x90, 0x3c, 0x7f};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, packet, sizeof(packet), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    sink_assert_bytes(&sink, expected, sizeof(expected));
}

static void test_decode_fragmented_note_on(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t p1[] = {0x80, 0x80, 0x90, 0x3c};
    const uint8_t p2[] = {0x80, 0x81, 0x7f};
    const uint8_t expected[] = {0x90, 0x3c, 0x7f};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p1, sizeof(p1), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    g_assert_cmpuint(sink.len, ==, 0);

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p2, sizeof(p2), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    sink_assert_bytes(&sink, expected, sizeof(expected));
}

static void test_decode_fragmented_running_status(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t p1[] = {0x80, 0x80, 0x90, 0x3c, 0x7f};
    const uint8_t p2[] = {0x80, 0x81, 0x40};
    const uint8_t p3[] = {0x80, 0x82, 0x7f};
    const uint8_t expected[] = {0x90, 0x3c, 0x7f, 0x90, 0x40, 0x7f};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p1, sizeof(p1), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p2, sizeof(p2), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p3, sizeof(p3), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    sink_assert_bytes(&sink, expected, sizeof(expected));
}

static void test_decode_realtime_inside_fragmented_note(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t p1[] = {0x80, 0x80, 0x90, 0x3c};
    const uint8_t p2[] = {0x80, 0x81, 0xf8, 0x7f};
    const uint8_t expected[] = {0xf8, 0x90, 0x3c, 0x7f};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p1, sizeof(p1), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p2, sizeof(p2), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    sink_assert_bytes(&sink, expected, sizeof(expected));
}

static void test_decode_fragmented_sysex(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t p1[] = {0x80, 0x80, 0xf0, 0x41, 0x10};
    const uint8_t p2[] = {0x80, 0x81, 0x42, 0x12, 0xf7};
    const uint8_t expected[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0xf7};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p1, sizeof(p1), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p2, sizeof(p2), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    sink_assert_bytes(&sink, expected, sizeof(expected));
}

static void test_decode_orphan_data_resyncs(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t packet[] = {0x80, 0x80, 0x3c, 0x7f};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, packet, sizeof(packet), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_RESYNC);
    g_assert_cmpuint(sink.len, ==, 0);
}

static void test_decode_reset_discards_pending(void) {
    MbBleMidiDecoderState state = {0};
    ByteSink sink = {0};
    const uint8_t p1[] = {0x80, 0x80, 0x90, 0x3c};
    const uint8_t p2[] = {0x80, 0x81, 0x7f};

    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p1, sizeof(p1), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_OK);
    mb_ble_midi_decoder_reset(&state);
    g_assert_cmpint(mb_ble_midi_decode_packet(&state, p2, sizeof(p2), sink_emit_byte, &sink),
                    ==, MB_BLE_MIDI_DECODE_RESYNC);
    g_assert_cmpuint(sink.len, ==, 0);
}

static void test_uuid_constants(void) {
    g_assert_cmpstr(MB_BLE_MIDI_SERVICE_UUID, ==, "03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    g_assert_cmpstr(MB_BLE_MIDI_IO_UUID, ==, "7772e5db-3868-4112-a1a9-f2669d106bf3");
    g_assert_cmpstr(MB_BLE_MIDI_ROLAND_IO_UUID_ALIAS, ==, "00006bf3-0000-1000-8000-00805f9b34fb");
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-ble-midi/status-data-lengths", test_status_data_lengths);
    g_test_add_func("/mb-ble-midi/header-validation", test_packet_header_validation);
    g_test_add_func("/mb-ble-midi/make-packet", test_make_packet);
    g_test_add_func("/mb-ble-midi/make-packet-invalid", test_make_packet_rejects_invalid_inputs);
    g_test_add_func("/mb-ble-midi/decode-complete-note-on", test_decode_complete_note_on);
    g_test_add_func("/mb-ble-midi/decode-fragmented-note-on", test_decode_fragmented_note_on);
    g_test_add_func("/mb-ble-midi/decode-fragmented-running-status", test_decode_fragmented_running_status);
    g_test_add_func("/mb-ble-midi/decode-realtime-inside-fragmented-note", test_decode_realtime_inside_fragmented_note);
    g_test_add_func("/mb-ble-midi/decode-fragmented-sysex", test_decode_fragmented_sysex);
    g_test_add_func("/mb-ble-midi/decode-orphan-data-resyncs", test_decode_orphan_data_resyncs);
    g_test_add_func("/mb-ble-midi/decode-reset-discards-pending", test_decode_reset_discards_pending);
    g_test_add_func("/mb-ble-midi/uuid-constants", test_uuid_constants);

    return g_test_run();
}
