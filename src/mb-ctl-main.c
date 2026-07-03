#include "mb-latency-diagnostics.h"
#include "mb-timeouts.h"

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int midi_ble_rtctl_stats_main(int argc, char **argv);

static bool is_help_arg(const char *s) {
    return g_strcmp0(s, "--help") == 0 || g_strcmp0(s, "-h") == 0;
}

static const char *ctl_argv0(void) {
    return "midi-ble-rtctl";
}

static void latency_usage(const char *argv0, const char *cmd) {
    g_printerr(
        "Usage:\n"
        "  %s %s [--path PATH]\n"
        "  %s latency-top [--path PATH] [--interval MS]\n",
        argv0, cmd, argv0);
}

static void latency_help_global(const char *argv0) {
    g_print(
        "\n"
        "Latency diagnostics commands:\n"
        "  latency      Print latency.tsv diagnostics snapshot\n"
        "  latency-top  Watch latency.tsv diagnostics continuously\n"
        "\n"
        "Help:\n"
        "  %s help latency\n"
        "  %s help latency-top\n"
        "\n",
        argv0, argv0);
}

static void latency_help_command(const char *argv0, const char *cmd) {
    if (g_strcmp0(cmd, "latency-top") == 0) {
        g_print(
            "Usage:\n"
            "  %s latency-top [--path PATH] [--interval MS]\n"
            "\n"
            "Description:\n"
            "  Repeatedly prints the live latency diagnostics file.\n"
            "  Start midi-ble-rtd with [stats] latency_diagnostics = yes first.\n"
            "\n"
            "Options:\n"
            "  --path PATH\n"
            "      Read a non-default latency.tsv file.\n"
            "\n"
            "  --interval MS\n"
            "      Refresh interval. Values are clamped to the ctl stats limits.\n"
            "\n",
            argv0);
        return;
    }

    g_print(
        "Usage:\n"
        "  %s latency [--path PATH]\n"
        "\n"
        "Description:\n"
        "  Prints the live latency diagnostics file once.\n"
        "  Start midi-ble-rtd with [stats] latency_diagnostics = yes first.\n"
        "\n"
        "Options:\n"
        "  --path PATH\n"
        "      Read a non-default latency.tsv file.\n"
        "\n",
        argv0);
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
    const char *argv0 = ctl_argv0();

    for (int i = 2; i < argc; i++) {
        if (is_help_arg(argv[i])) {
            latency_help_command(argv0, watch ? "latency-top" : "latency");
            g_free(path);
            return 0;
        } else if (g_strcmp0(argv[i], "--path") == 0 && i + 1 < argc) {
            g_free(path);
            path = g_strdup(argv[++i]);
        } else if (watch && g_strcmp0(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_ms = (unsigned)atoi(argv[++i]);
            if (interval_ms < MB_STATS_CTL_MIN_INTERVAL_MS)
                interval_ms = MB_STATS_CTL_MIN_INTERVAL_MS;
            if (interval_ms > MB_STATS_CTL_MAX_INTERVAL_MS)
                interval_ms = MB_STATS_CTL_MAX_INTERVAL_MS;
        } else {
            latency_usage(argv0, watch ? "latency-top" : "latency");
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
    char **display_argv = g_new0(char *, (gsize)argc + 1);
    for (int i = 0; i < argc; i++)
        display_argv[i] = argv[i];
    if (argc > 0)
        display_argv[0] = (char *)ctl_argv0();

    if (argc == 3 && g_strcmp0(argv[1], "help") == 0 &&
        (g_strcmp0(argv[2], "latency") == 0 || g_strcmp0(argv[2], "latency-top") == 0)) {
        latency_help_command(ctl_argv0(), argv[2]);
        g_free(display_argv);
        return 0;
    }

    if (argc >= 2 && g_strcmp0(argv[1], "latency") == 0) {
        g_free(display_argv);
        return latency_command(argc, argv, false);
    }

    if (argc >= 2 && g_strcmp0(argv[1], "latency-top") == 0) {
        g_free(display_argv);
        return latency_command(argc, argv, true);
    }

    if ((argc == 2 && is_help_arg(argv[1])) ||
        (argc == 2 && g_strcmp0(argv[1], "help") == 0)) {
        int rc = midi_ble_rtctl_stats_main(argc, display_argv);
        latency_help_global(ctl_argv0());
        g_free(display_argv);
        return rc;
    }

    int rc = midi_ble_rtctl_stats_main(argc, display_argv);
    g_free(display_argv);
    return rc;
}
