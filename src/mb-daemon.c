/*
 * midi-ble-rtd process entry point.
 *
 * Keep the public daemon binary stable and delegate lifecycle policy to the
 * orchestrator layer.  This keeps main() small and makes debugging split cleanly
 * between process startup and runtime/session orchestration.
 */

#include "mb-orchestrator.h"

#include <glib.h>
#include <glib-unix.h>
#include <signal.h>

#include "mb-bluez.h"
#include "mb-config.h"
#include "mb-session.h"

static const char *printable_string(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

static gboolean quit_main_loop(gpointer user_data) {
    GMainLoop *loop = user_data;
    if (loop)
        g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static bool session_has_bluez_device_path(const MbSession *session) {
    return session &&
           session->device_path &&
           *session->device_path &&
           !g_str_has_prefix(session->device_path, "config:");
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
            session->auto_reconnect = device->reconnect_on_link_loss;
        g_free(path);
    }
}

static bool resolve_config_dir_bluez_devices(const MbConfig *cfg, MbDaemon *daemon) {
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        g_printerr("BlueZ discovery unavailable: could not connect to system bus: %s\n",
                   error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    unsigned found = 0;
    for (unsigned i = 0; i < mb_config_device_count(cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(cfg, i);
        if (!device || !device->address || !*device->address)
            continue;

        char *bluez_path = mb_bluez_find_device_path_by_address(bus, device->address);
        if (!bluez_path)
            continue;

        MbSession *session = mb_daemon_ensure_session(daemon,
                                                      bluez_path,
                                                      device->address,
                                                      device->name);
        if (session) {
            session->auto_reconnect = device->reconnect_on_link_loss;
            found++;
        }
        g_free(bluez_path);
    }

    g_object_unref(bus);
    return found > 0;
}

static void try_config_dir_connect_devices(const MbConfig *cfg, MbDaemon *daemon) {
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        g_printerr("Connect skeleton unavailable: could not connect to system bus: %s\n",
                   error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    for (unsigned i = 0; i < mb_config_device_count(cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(cfg, i);
        if (!device || !device->connect_on_start)
            continue;

        MbSession *session = mb_daemon_find_session_by_address(daemon, device->address);
        if (!session || !session_has_bluez_device_path(session)) {
            g_print("Connect skipped for %s: BlueZ Device1 not found.\n",
                    printable_string(device->id, printable_string(device->address, "device")));
            continue;
        }

        mb_session_handle_event(session, MB_EV_CMD_CONNECT);

        bool connected = false;
        if (mb_bluez_get_device_bool_property(bus, session->device_path, "Connected", &connected) && connected) {
            mb_session_handle_event(session, MB_EV_BLUEZ_CONNECTED);
            g_print("Connect skeleton already connected for %s.\n",
                    printable_string(device->id, printable_string(device->address, "device")));
            continue;
        }

        if (device->pair && !mb_bluez_pair_device(bus, session->device_path)) {
            mb_session_handle_event(session, MB_EV_BLUEZ_CONNECT_FAILED);
            continue;
        }

        if (device->trust)
            mb_bluez_set_device_trusted(bus, session->device_path);

        if (!mb_bluez_connect_device(bus, session->device_path)) {
            mb_session_handle_event(session, MB_EV_BLUEZ_CONNECT_FAILED);
            continue;
        }

        mb_session_handle_event(session, MB_EV_BLUEZ_CONNECTED);
        g_print("Connect skeleton ok for %s.\n",
                printable_string(device->id, printable_string(device->address, "device")));
    }

    g_object_unref(bus);
}

static void print_config_dir_devices(const MbConfig *cfg,
                                     const MbDaemon *daemon,
                                     const char *config_dir) {
    g_print("midi-ble-rtd\n");
    g_print("Runtime: config-directory connect skeleton\n");
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
        g_print("  connect_on_start: %s\n", device->connect_on_start ? "yes" : "no");
        g_print("  pair:           %s\n", device->pair ? "yes" : "no");
        g_print("  trust:          %s\n", device->trust ? "yes" : "no");
        g_print("  reconnect_on_link_loss: %s\n", device->reconnect_on_link_loss ? "yes" : "no");
        g_print("  enable_tx:      %s\n", device->enable_tx ? "yes" : "no");
        g_print("  ALSA port:      %s\n", printable_string(device->alsa_port_name, "BLE-MIDI"));
        if (session) {
            g_print("  bluez:          %s\n", session_has_bluez_device_path(session) ? "found" : "not found");
            g_print("  session path:   %s\n", printable_string(session->device_path, "(none)"));
            g_print("  session state:  %s\n", mb_session_state_name(session->state));
            g_print("  session error:  %s\n", mb_error_name(session->error));
        } else {
            g_print("  bluez:          not found\n");
            g_print("  session:        missing\n");
        }
    }

    g_print("\n");
    g_print("Note: directory configs currently connect Device1 only.\n");
    g_print("GATT binding, ALSA ports and streaming remain on the single-device file config path.\n");
}

static int run_config_directory_mode(const char *config_dir) {
    MbConfig cfg = {0};
    if (!mb_config_load_dir(&cfg, config_dir)) {
        mb_config_clear(&cfg);
        return 1;
    }

    MbDaemon daemon = {0};
    build_config_dir_sessions(&cfg, &daemon);
    resolve_config_dir_bluez_devices(&cfg, &daemon);
    try_config_dir_connect_devices(&cfg, &daemon);
    print_config_dir_devices(&cfg, &daemon, config_dir);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    guint sigint_source = g_unix_signal_add(SIGINT, quit_main_loop, loop);
    guint sigterm_source = g_unix_signal_add(SIGTERM, quit_main_loop, loop);

    g_print("\nDaemon loop: running. Press Ctrl-C to exit.\n");
    g_main_loop_run(loop);
    g_print("Daemon loop: stopping.\n");

    if (sigint_source)
        g_source_remove(sigint_source);
    if (sigterm_source)
        g_source_remove(sigterm_source);
    g_main_loop_unref(loop);

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
