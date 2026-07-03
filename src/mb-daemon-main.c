#include "mb-version.h"

#include <glib.h>
#include <stdbool.h>

int midi_ble_rtd_runtime_main(int argc, char **argv);

static bool is_version_arg(const char *s) {
    return g_strcmp0(s, "-v") == 0 || g_strcmp0(s, "--version") == 0;
}

static bool is_verbose_version_arg(const char *s) {
    return g_strcmp0(s, "-vv") == 0 || g_strcmp0(s, "--version-verbose") == 0;
}

static void print_version(bool verbose) {
    g_print("midi-ble-rtd %s\n", MB_VERSION);
    if (!verbose)
        return;

    g_print("project: midi-ble-rt\n");
    g_print("role: daemon\n");
    g_print("runtime: config-directory multi-session\n");
    g_print("features: bluez-gatt alsa-sequencer stats latency rtkit\n");
    g_print("realtime-default: rtkit=off realtime_rx=yes realtime_tx=no rt_priority=1\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && is_version_arg(argv[1])) {
        print_version(false);
        return 0;
    }

    if (argc == 2 && is_verbose_version_arg(argv[1])) {
        print_version(true);
        return 0;
    }

    char **display_argv = g_new0(char *, (gsize)argc + 1);
    for (int i = 0; i < argc; i++)
        display_argv[i] = argv[i];
    if (argc > 0)
        display_argv[0] = (char *)"midi-ble-rtd";

    int rc = midi_ble_rtd_runtime_main(argc, display_argv);
    g_free(display_argv);
    return rc;
}
