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

static void print_config_dir_devices(const MbConfig *cfg, const char *config_dir) {
    g_print("midi-ble-rtd\n");
    g_print("Runtime: config-dir loader\n");
    g_print("Config dir: %s\n", config_dir);
    g_print("ALSA client: %s\n", cfg->alsa_client_name ? cfg->alsa_client_name : "midi-ble-rt");
    g_print("Service UUID: %s\n", cfg->service_uuid ? cfg->service_uuid : "");
    g_print("I/O UUID: %s\n", cfg->io_uuid ? cfg->io_uuid : "");
    g_print("I/O alias: %s\n", cfg->io_uuid_alias ? cfg->io_uuid_alias : "");
    g_print("Stats: %s\n", cfg->stats_enabled ? "enabled" : "disabled");
    g_print("Devices: %u\n", mb_config_device_count(cfg));

    for (unsigned i = 0; i < mb_config_device_count(cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(cfg, i);
        if (!device)
            continue;

        g_print("\n");
        g_print("Device[%u]: %s\n", i, device->id && *device->id ? device->id : "(unnamed)");
        g_print("  enabled:        %s\n", device->enabled ? "yes" : "no");
        g_print("  address:        %s\n", device->address && *device->address ? device->address : "(none)");
        g_print("  name:           %s\n", device->name && *device->name ? device->name : "(none)");
        g_print("  profile:        %s\n", device->profile && *device->profile ? device->profile : "standard_ble_midi");
        g_print("  autoconnect:    %s\n", device->autoconnect ? "yes" : "no");
        g_print("  pair:           %s\n", device->pair ? "yes" : "no");
        g_print("  trust:          %s\n", device->trust ? "yes" : "no");
        g_print("  auto_reconnect: %s\n", device->auto_reconnect ? "yes" : "no");
        g_print("  enable_tx:      %s\n", device->enable_tx ? "yes" : "no");
        g_print("  ALSA port:      %s\n", device->alsa_port_name && *device->alsa_port_name ? device->alsa_port_name : "BLE-MIDI");
    }

    g_print("\n");
    g_print("Note: --config-dir currently validates and lists devices only.\n");
    g_print("Multi-device streaming will be enabled in the orchestrator in a later cut.\n");
}

static int run_config_dir_mode(const char *config_dir) {
    MbConfig cfg = {0};
    if (!mb_config_load_dir(&cfg, config_dir)) {
        mb_config_clear(&cfg);
        return 1;
    }

    print_config_dir_devices(&cfg, config_dir);
    mb_config_clear(&cfg);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && g_strcmp0(argv[1], "--config-dir") == 0)
        return run_config_dir_mode(argv[2]);

    return mb_orchestrator_main(argc, argv);
}
