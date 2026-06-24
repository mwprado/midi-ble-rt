#include "mb-config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

static char *write_temp_config(const char *content) {
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("mb-config-test-XXXXXX.ini", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    g_assert_true(g_file_set_contents(path, content, -1, &error));
    g_assert_no_error(error);
    return path;
}

static void test_defaults_are_applied(void) {
    char *path = write_temp_config("[device]\naddress=AA:BB:CC:DD:EE:FF\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load(&cfg, path));

    g_assert_cmpstr(cfg.address, ==, "AA:BB:CC:DD:EE:FF");
    g_assert_cmpstr(cfg.name, ==, "");
    g_assert_false(cfg.pair);
    g_assert_true(cfg.trust);
    g_assert_true(cfg.auto_reconnect);
    g_assert_cmpstr(cfg.service_uuid, ==, "03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    g_assert_cmpstr(cfg.io_uuid, ==, "7772e5db-3868-4112-a1a9-f2669d106bf3");
    g_assert_cmpstr(cfg.io_uuid_alias, ==, "00006bf3-0000-1000-8000-00805f9b34fb");
    g_assert_true(cfg.require_notify);
    g_assert_true(cfg.require_write_without_response);
    g_assert_cmpstr(cfg.alsa_client_name, ==, "midi-ble-rt");
    g_assert_cmpstr(cfg.alsa_port_name, ==, "BLE-MIDI In");
    g_assert_false(cfg.print_ble_packets);
    g_assert_false(cfg.print_midi_events);
    g_assert_true(cfg.enable_tx);

    mb_config_clear(&cfg);
    g_unlink(path);
    g_free(path);
}

static void test_explicit_values_override_defaults(void) {
    char *path = write_temp_config(
        "[device]\n"
        "address=11:22:33:44:55:66\n"
        "name=GO:KEYS\n"
        "pair=true\n"
        "trust=false\n"
        "auto_reconnect=false\n"
        "[gatt]\n"
        "service_uuid=service-test\n"
        "io_uuid=io-test\n"
        "io_uuid_alias=alias-test\n"
        "require_notify=false\n"
        "require_write_without_response=false\n"
        "[alsa]\n"
        "client_name=test-client\n"
        "port_name=test-port\n"
        "[debug]\n"
        "print_ble_packets=true\n"
        "print_midi_events=true\n"
        "[midi]\n"
        "enable_tx=false\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load(&cfg, path));

    g_assert_cmpstr(cfg.address, ==, "11:22:33:44:55:66");
    g_assert_cmpstr(cfg.name, ==, "GO:KEYS");
    g_assert_true(cfg.pair);
    g_assert_false(cfg.trust);
    g_assert_false(cfg.auto_reconnect);
    g_assert_cmpstr(cfg.service_uuid, ==, "service-test");
    g_assert_cmpstr(cfg.io_uuid, ==, "io-test");
    g_assert_cmpstr(cfg.io_uuid_alias, ==, "alias-test");
    g_assert_false(cfg.require_notify);
    g_assert_false(cfg.require_write_without_response);
    g_assert_cmpstr(cfg.alsa_client_name, ==, "test-client");
    g_assert_cmpstr(cfg.alsa_port_name, ==, "test-port");
    g_assert_true(cfg.print_ble_packets);
    g_assert_true(cfg.print_midi_events);
    g_assert_false(cfg.enable_tx);

    mb_config_clear(&cfg);
    g_unlink(path);
    g_free(path);
}

static void test_missing_file_fails(void) {
    MbConfig cfg = {0};
    g_assert_false(mb_config_load(&cfg, "/tmp/midi-ble-rt-missing-config.ini"));
    mb_config_clear(&cfg);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-config/defaults", test_defaults_are_applied);
    g_test_add_func("/mb-config/explicit-values", test_explicit_values_override_defaults);
    g_test_add_func("/mb-config/missing-file", test_missing_file_fails);

    return g_test_run();
}
