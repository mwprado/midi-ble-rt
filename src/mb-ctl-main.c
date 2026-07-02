#include "mb-latency-diagnostics.h"
#include "mb-timeouts.h"

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int midi_ble_rtctl_stats_main(int argc, char **argv);

static void latency_usage(const char *argv0, const char *cmd) {
    g_printerr(
        "Usage:\n"
        "  %s %s [--path PATH]\n"
        "  %s latency-top [--path PATH] [--interval MS]\n",
        argv0, cmd, argv0);
}

static int latency_print_file(const char *path) {
    char *content = NULL;
    gsize len = 0;
    GError *error = NULL;

    if (!g_file_get_contents(path, &content, &len, &error)) {
        g_printerr("Could not read latency diagnostics file: %s\n", path);
        g_printerr("Reason: %s\n", error ? error->message : g_strerror(errno));
        g_printerr("Start midi-ble-rtd with [stats] latency_diagnostics = yes first.\n");
        g_clear_error(&error);
        return 1;
    }

    if (len > 0)
        fputs(content, stdout);
    g_free(content);
    return 0;
}

static int latency_command(int argc, char **argv, bool watch) {
    char *path = mb_latency_diagnostics_default_path();
    unsigned interval_ms = MB_STATS_CTL_DEFAULT_INTERVAL_MS;

    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--path") == 0 && i + 1 < argc) {
            g_free(path);
            path = g_strdup(argv[++i]);
        } else if (watch && g_strcmp0(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_ms = (unsigned)atoi(argv[++i]);
            if (interval_ms < MB_STATS_CTL_MIN_INTERVAL_MS)
                interval_ms = MB_STATS_CTL_MIN_INTERVAL_MS;
            if (interval_ms > MB_STATS_CTL_MAX_INTERVAL_MS)
                interval_ms = MB_STATS_CTL_MAX_INTERVAL_MS;
        } else {
            latency_usage(argv[0], watch ? "latency-top" : "latency");
            g_free(path);
            return 2;
        }
    }

    if (!watch) {
        int rc = latency_print_file(path);
        g_free(path);
        return rc;
    }

    for (;;) {
        g_print("\033[H\033[J");
        (void)latency_print_file(path);
        g_usleep((gulong)interval_ms * 1000UL);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && g_strcmp0(argv[1], "latency") == 0)
        return latency_command(argc, argv, false);

    if (argc >= 2 && g_strcmp0(argv[1], "latency-top") == 0)
        return latency_command(argc, argv, true);

    return midi_ble_rtctl_stats_main(argc, argv);
}
