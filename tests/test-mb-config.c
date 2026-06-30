#include "mb-config.h"

#include <glib.h>
#include <glib/gstdio.h>

static void write_file_checked(const char *path, const char *content) {
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, content, -1, &error));
    g_assert_no_error(error);
}

static char *make_config_dir(void) {
    GError *error = NULL;
    char *dir = g_dir_make_tmp("mb-config-dir-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(dir);

    char *devices_dir = g_build_filename(dir, "devices.d", NULL);
    g_assert_cmpint(g_mkdir(devices_dir, 0700), ==, 0);
    g_free(devices_dir);

    return dir;
}

static void write_daemon_config(const char *dir, const char *content) {
    char *path = g_build_filename(dir, "daemon.ini", NULL);
    write_file_checked(path, content);
    g_free(path);
}

static void write_device_config(const char *dir, const char *filename, const char *content) {
    char *devices_dir = g_build_filename(dir, "devices.d", NULL);
    char *path = g_build_filename(devices_dir, filename, NULL);
    write_file_checked(path, content);
    g_free(path);
    g_free(devices_dir);
}

static void cleanup_config_dir(const char *dir, const char *first, const char *second) {
    char *daemon_ini = g_build_filename(dir, "daemon.ini", NULL);
    char *devices_dir = g_build_filename(dir, "devices.d", NULL);

    g_unlink(daemon_ini);

    if (first) {
        char *path = g_build_filename(devices_dir, first, NULL);
        g_unlink(path);
        g_free(path);
    }

    if (second) {
        char *path = g_build_filename(devices_dir, second, NULL);
        g_unlink(path);
        g_free(path);
    }

    g_rmdir(devices_dir);
    g_rmdir(dir);

    g_free(daemon_ini);
    g_free(devices_dir);
}

static void test_config_dir_defaults_are_applied(void) {
    char *dir = make_config_dir();

    write_daemon_config(dir,
        "[daemon]\n"
        "client_name=midi-ble-rt\n");

    write_device_config(dir, "device.ini",
        "[device]\n"
        "id=device-a\n"
        "enabled=true\n"
        "address=11:22:33:44:55:66\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load_dir(&cfg, dir));

    g_assert_cmpstr(cfg.address, ==, "");
    g_assert_cmpstr(cfg.name, ==, "");
    g_assert_false(cfg.pair);
    g_assert_true(cfg.trust);
    g_assert_true(cfg.reconnect_on_link_loss);
    g_assert_cmpstr(cfg.service_uuid, ==, "03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    g_assert_cmpstr(cfg.io_uuid, ==, "7772e5db-3868-4112-a1a9-f2669d106bf3");
    g_assert_cmpstr(cfg.io_uuid_alias, ==, "");
    g_assert_cmpstr(cfg.alsa_client_name, ==, "midi-ble-rt");
    g_assert_cmpstr(cfg.alsa_port_name, ==, "BLE-MIDI");
    g_assert_true(cfg.enable_tx);
    g_assert_true(cfg.stats_enabled);
    g_assert_cmpuint(cfg.stats_interval_ms, ==, 1000);

    g_assert_cmpuint(mb_config_device_count(&cfg), ==, 1);
    const MbDeviceConfig *device = mb_config_get_device(&cfg, 0);
    g_assert_nonnull(device);
    g_assert_cmpstr(device->id, ==, "device-a");
    g_assert_cmpstr(device->address, ==, "11:22:33:44:55:66");
    g_assert_cmpstr(device->profile, ==, "standard_ble_midi");
    g_assert_cmpstr(device->alsa_port_name, ==, "BLE-MIDI");
    g_assert_false(device->connect_on_start);
    g_assert_false(device->pair);
    g_assert_true(device->trust);
    g_assert_true(device->reconnect_on_link_loss);
    g_assert_true(device->enable_tx);

    mb_config_clear(&cfg);
    cleanup_config_dir(dir, "device.ini", NULL);
    g_free(dir);
}

static void test_config_dir_loads_devices_d(void) {
    char *dir = make_config_dir();

    write_daemon_config(dir,
        "[daemon]\n"
        "client_name=midi-ble-rt\n"
        "[defaults]\n"
        "pair=false\n"
        "trust=true\n"
        "reconnect_on_link_loss=true\n"
        "enable_tx=true\n");

    write_device_config(dir, "00-first.ini",
        "[device]\n"
        "id=first\n"
        "enabled=true\n"
        "address=11:22:33:44:55:01\n"
        "connect_on_start=true\n");

    write_device_config(dir, "01-second.ini",
        "[device]\n"
        "id=second\n"
        "enabled=true\n"
        "address=11:22:33:44:55:02\n"
        "connect_on_start=false\n");

    MbConfig cfg = {0};
    g_assert_true(mb_config_load_dir(&cfg, dir));

    g_assert_cmpuint(mb_config_device_count(&cfg), ==, 2);
    g_assert_cmpstr(mb_config_get_device(&cfg, 0)->id, ==, "first");
    g_assert_cmpstr(mb_config_get_device(&cfg, 1)->id, ==, "second");

    mb_config_clear(&cfg);
    cleanup_config_dir(dir, "00-first.ini", "01-second.ini");
    g_free(dir);
}

static void test_config_dir_missing_daemon_fails(void) {
    char *dir = make_config_dir();

    MbConfig cfg = {0};
    g_assert_false(mb_config_load_dir(&cfg, dir));
    mb_config_clear(&cfg);

    cleanup_config_dir(dir, NULL, NULL);
    g_free(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-config/config-dir-defaults", test_config_dir_defaults_are_applied);
    g_test_add_func("/mb-config/config-dir-devices-d", test_config_dir_loads_devices_d);
    g_test_add_func("/mb-config/config-dir-missing-daemon", test_config_dir_missing_daemon_fails);

    return g_test_run();
}
