/*
 * midi-ble-rtd process entry point.
 *
 * Keep the public daemon binary stable and delegate lifecycle policy to the
 * orchestrator layer.  This keeps main() small and makes debugging split cleanly
 * between process startup and runtime/session orchestration.
 */

#include "mb-orchestrator.h"

#include <glib.h>

#include "mb-config.h"
#include "mb-session.h"

static const char *printable_string(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

static char *config_device_path(const MbDeviceConfig *device) {
    const char *id = printable_string(device->id, NULL);
    if (id)
        return g_strdup_printf("config:%s", id);

    return g_strdup_printf("config:%s", printable_string(device->address, "device"));
}

static void build_config_dir_sessions(const MbConfig *cfg, MbDaemon *daemon) {
    mb_daemon_init(daemon);

    for (unsigned i = 0; i < mb_config_device_count(cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(cfg, i);
        if (!device)
            continue;

        char *path = config_device_path(device);
        MbSession *session = mb_daemon_ensure_session(daemon,
                                                      path,
                                                      device->address,
                                                      device->name);
        if (session)
            session->auto_reconnect = device->auto_reconnect;
        g_free(path);
    }
}

static void print_config_dir_devices(const MbConfig *cfg,
                                     const MbDaemon *daemon,
                                     const char *config_dir) {
    g_print("midi-ble-rtd\n");
    g_print("Runtime: config-directory session skeleton\n");
    g_print("Config: %s\n", config_dir);
    g_print("ALSA client: %s\n", printable_string(cfg->alsa_client_name, "midi-ble-rt"));
    g_print("Service UUID: %s\n", printable_string(cfg->service_uuid, ""));
    g_print("I/O UUID: %s\n", printable_string(cfg->io_uuid, ""));
    g_print("I/O alias: %s\n", printable_string(cfg->io_uuid_alias, ""));
    g_print("Stats: %s\n", cfg->stats_enabled ? "enabled" : "disabled");
    g_print("Configured devices: %u\n", mb_config_device_count(cfg));
    g_print("Skeleton sessions: %zu\n", mb_daemon_session_count(daemon));

    for (unsigned i = 0; i < mb_config_device_count(cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(cfg, i);
        if (!device)
            continue;

        MbSession *session = mb_daemon_find_session_by_address((MbDaemon *)daemon,
                                                               device->address);

        g_print("\n");
        g_print("Device[%u]: %s\n", i, printable_string(device->id, "(unnamed)"));
        g_print("  enabled:        %s\n", device->enabled ? "yes" : "no");
        g_print("  address:        %s\n", printable_string(device->address, "(none)"));
        g_print("  name:           %s\n", printable_string(device->name, "(none)"));
        g_print("  profile:        %s\n", printable_string(device->profile, "standard_ble_midi"));
        g_print("  autoconnect:    %s\n", device->autoconnect ? "yes" : "no");
        g_print("  pair:           %s\n", device->pair ? "yes" : "no");
        g_print("  trust:          %s\n", device->trust ? "yes" : "no");
        g_print("  auto_reconnect: %s\n", device->auto_reconnect ? "yes" : "no");
        g_print("  enable_tx:      %s\n", device->enable_tx ? "yes" : "no");
        g_print("  ALSA port:      %s\n", printable_string(device->alsa_port_name, "BLE-MIDI"));
        if (session) {
            g_print("  session path:   %s\n", printable_string(session->device_path, "(none)"));
            g_print("  session state:  %s\n", mb_session_state_name(session->state));
            g_print("  session error:  %s\n", mb_error_name(session->error));
        } else {
            g_print("  session:        missing\n");
        }
    }

    g_print("\n");
    g_print("Note: directory configs currently build IDLE session skeletons only.\n");
    g_print("BlueZ/GATT discovery, ALSA ports and streaming remain on the single-device file config path.\n");
}

static int run_config_directory_mode(const char *config_dir) {
    MbConfig cfg = {0};
    if (!mb_config_load_dir(&cfg, config_dir)) {
        mb_config_clear(&cfg);
        return 1;
    }

    MbDaemon daemon = {0};
    build_config_dir_sessions(&cfg, &daemon);
    print_config_dir_devices(&cfg, &daemon, config_dir);
    mb_daemon_clear(&daemon);
    mb_config_clear(&cfg);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && g_strcmp0(argv[1], "--config") == 0) {
        if (g_file_test(argv[2], G_FILE_TEST_IS_DIR))
            return run_config_directory_mode(argv[2]);

        return mb_orchestrator_main(argc, argv);
    }

    if (argc == 3 && g_strcmp0(argv[1], "--config-dir") == 0)
        return run_config_directory_mode(argv[2]);

    return mb_orchestrator_main(argc, argv);
}
