#include "mb-ble-midi.h"

#include <glib.h>

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
    g_test_add_func("/mb-ble-midi/uuid-constants", test_uuid_constants);

    return g_test_run();
}
