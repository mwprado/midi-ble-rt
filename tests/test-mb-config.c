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

static void write_file_checked(const char *path, const char *content) {
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, content, -1, &error));
    g_assert_no_error(error);
}

static void test_defaults_are_applied(void) {
    char *path = write_temp_config("[device]\naddress=AA:BB:CC:DD:EE:FF\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load(&cfg, path));

    g_assert_cmpstr(cfg.address, ==, "AA:BB:CC:DD:EE:FF");
    g_assert_cmpstr(cfg.name, ==, "");
    g_assert_false(cfg.pair);
    g_assert_true(cfg.trust);
    g_assert_true(cfg.reconnect_on_link_loss);
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

    g_assert_cmpuint(mb_config_device_count(&cfg), ==, 1);
    const MbDeviceConfig *device = mb_config_get_device(&cfg, 0);
    g_assert_nonnull(device);
    g_assert_cmpstr(device->address, ==, "AA:BB:CC:DD:EE:FF");
    g_assert_cmpstr(device->profile, ==, "standard_ble_midi");
    g_assert_true(device->connect_on_start);
    g_assert_true(device->autoconnect);

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
        "reconnect_on_link_loss=false\n"
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
    g_assert_false(cfg.reconnect_on_link_loss);
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

    g_assert_cmpuint(mb_config_device_count(&cfg), ==, 1);
    const MbDeviceConfig *device = mb_config_get_device(&cfg, 0);
    g_assert_nonnull(device);
    g_assert_cmpstr(device->address, ==, "11:22:33:44:55:66");
    g_assert_cmpstr(device->name, ==, "GO:KEYS");
    g_assert_false(device->trust);
    g_assert_false(device->reconnect_on_link_loss);
    g_assert_false(device->auto_reconnect);
    g_assert_false(device->enable_tx);

    mb_config_clear(&cfg);
    g_unlink(path);
    g_free(path);
}

static void test_config_dir_loads_devices_d(void) {
    GError *error = NULL;
    char *dir = g_dir_make_tmp("mb-config-dir-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(dir);

    char *devices_dir = g_build_filename(dir, "devices.d", NULL);
    g_assert_cmpint(g_mkdir(devices_dir, 0700), ==, 0);

    char *daemon_ini = g_build_filename(dir, "daemon.ini", NULL);
    char *gokeys_ini = g_build_filename(devices_dir, "roland-gokeys.ini", NULL);
    char *widi_ini = g_build_filename(devices_dir, "widi-master.ini", NULL);
    char *disabled_ini = g_build_filename(devices_dir, "disabled.ini", NULL);

    write_file_checked(daemon_ini,
        "[daemon]\n"
        "client_name=midi-ble-rt\n"
        "[defaults]\n"
        "pair=false\n"
        "trust=true\n"
        "reconnect_on_link_loss=true\n"
        "enable_tx=true\n"
        "[stats]\n"
        "enabled=true\n"
        "interval_ms=1000\n");

    write_file_checked(gokeys_ini,
        "[device]\n"
        "id=roland-gokeys\n"
        "enabled=true\n"
        "address=CB:81:F4:62:FF:07\n"
        "name=Roland GO:KEYS\n"
        "profile=roland_gokeys\n"
        "connect_on_start=true\n"
        "alsa_port_name=Roland GO:KEYS BLE-MIDI\n"
        "[policy]\n"
        "pair=false\n"
        "trust=true\n"
        "reconnect_on_link_loss=true\n"
        "[midi]\n"
        "enable_tx=true\n");

    write_file_checked(widi_ini,
        "[device]\n"
        "id=widi-master\n"
        "enabled=true\n"
        "address=AA:BB:CC:DD:EE:FF\n"
        "profile=standard_ble_midi\n"
        "connect_on_start=false\n"
        "alsa_port_name=WIDI Master BLE-MIDI\n");

    write_file_checked(disabled_ini,
        "[device]\n"
        "id=disabled\n"
        "enabled=false\n"
        "address=00:00:00:00:00:00\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load_dir(&cfg, dir));

    g_assert_cmpuint(mb_config_device_count(&cfg), ==, 2);

    const MbDeviceConfig *first = mb_config_get_device(&cfg, 0);
    const MbDeviceConfig *second = mb_config_get_device(&cfg, 1);
    g_assert_nonnull(first);
    g_assert_nonnull(second);

    const MbDeviceConfig *gokeys = NULL;
    const MbDeviceConfig *widi = NULL;
    for (unsigned i = 0; i < mb_config_device_count(&cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(&cfg, i);
        if (g_strcmp0(device->id, "roland-gokeys") == 0)
            gokeys = device;
        else if (g_strcmp0(device->id, "widi-master") == 0)
            widi = device;
    }

    g_assert_nonnull(gokeys);
    g_assert_cmpstr(gokeys->address, ==, "CB:81:F4:62:FF:07");
    g_assert_cmpstr(gokeys->profile, ==, "roland_gokeys");
    g_assert_true(gokeys->connect_on_start);
    g_assert_true(gokeys->autoconnect);
    g_assert_true(gokeys->reconnect_on_link_loss);
    g_assert_true(gokeys->auto_reconnect);
    g_assert_true(gokeys->enable_tx);

    g_assert_nonnull(widi);
    g_assert_cmpstr(widi->address, ==, "AA:BB:CC:DD:EE:FF");
    g_assert_cmpstr(widi->profile, ==, "standard_ble_midi");
    g_assert_false(widi->connect_on_start);
    g_assert_false(widi->autoconnect);

    mb_config_clear(&cfg);

    g_unlink(daemon_ini);
    g_unlink(gokeys_ini);
    g_unlink(widi_ini);
    g_unlink(disabled_ini);
    g_rmdir(devices_dir);
    g_rmdir(dir);
    g_free(daemon_ini);
    g_free(gokeys_ini);
    g_free(widi_ini);
    g_free(disabled_ini);
    g_free(devices_dir);
    g_free(dir);
}

static void test_config_dir_ignores_duplicate_ids(void) {
    GError *error = NULL;
    char *dir = g_dir_make_tmp("mb-config-dup-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(dir);

    char *devices_dir = g_build_filename(dir, "devices.d", NULL);
    g_assert_cmpint(g_mkdir(devices_dir, 0700), ==, 0);

    char *daemon_ini = g_build_filename(dir, "daemon.ini", NULL);
    char *first_ini = g_build_filename(devices_dir, "00-first.ini", NULL);
    char *duplicate_ini = g_build_filename(devices_dir, "01-duplicate.ini", NULL);

    write_file_checked(daemon_ini,
        "[daemon]\n"
        "client_name=midi-ble-rt\n"
        "[defaults]\n"
        "trust=true\n"
        "auto_reconnect=true\n");

    write_file_checked(first_ini,
        "[device]\n"
        "id=dup-device\n"
        "enabled=true\n"
        "address=AA:BB:CC:DD:EE:01\n"
        "name=first\n");

    write_file_checked(duplicate_ini,
        "[device]\n"
        "id=dup-device\n"
        "enabled=true\n"
        "address=AA:BB:CC:DD:EE:02\n"
        "name=duplicate\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load_dir(&cfg, dir));

    g_assert_cmpuint(mb_config_device_count(&cfg), ==, 1);
    const MbDeviceConfig *device = mb_config_get_device(&cfg, 0);
    g_assert_nonnull(device);
    g_assert_cmpstr(device->id, ==, "dup-device");
    g_assert_cmpstr(device->address, ==, "AA:BB:CC:DD:EE:01");

    mb_config_clear(&cfg);

    g_unlink(daemon_ini);
    g_unlink(first_ini);
    g_unlink(duplicate_ini);
    g_rmdir(devices_dir);
    g_rmdir(dir);
    g_free(daemon_ini);
    g_free(first_ini);
    g_free(duplicate_ini);
    g_free(devices_dir);
    g_free(dir);
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
    g_test_add_func("/mb-config/config-dir-devices-d", test_config_dir_loads_devices_d);
    g_test_add_func("/mb-config/config-dir-duplicate-ids", test_config_dir_ignores_duplicate_ids);
    g_test_add_func("/mb-config/missing-file", test_missing_file_fails);

    return g_test_run();
}
