#include "mb-latency-diagnostics.h"
#include "mb-timeouts.h"
#include "mb-version.h"

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int midi_ble_rtctl_stats_main(int argc, char **argv);

static bool is_help_arg(const char *s) {
    return g_strcmp0(s, "--help") == 0 || g_strcmp0(s, "-h") == 0;
}

static bool is_version_arg(const char *s) {
    return g_strcmp0(s, "-v") == 0 || g_strcmp0(s, "--version") == 0;
}

static bool is_verbose_version_arg(const char *s) {
    return g_strcmp0(s, "-vv") == 0 || g_strcmp0(s, "--version-verbose") == 0;
}

static const char *ctl_argv0(void) {
    return "midi-ble-rtctl";
}

static void ctl_print_version(bool verbose) {
    g_print("midi-ble-rtctl %s\n", MB_VERSION);
    if (!verbose)
        return;

    g_print("project: midi-ble-rt\n");
    g_print("role: ctl\n");
    g_print("features: bluez-control daemon-control stats latency\n");
}

static void ctl_help_global(void) {
    const char *argv0 = ctl_argv0();
    g_print(
        "midi-ble-rtctl - control and diagnostics CLI for midi-ble-rt\n"
        "\n"
        "Usage:\n"
        "  %s COMMAND [ARGS...]\n"
        "  %s --help\n"
        "  %s -v\n"
        "  %s -vv\n"
        "  %s help [COMMAND]\n"
        "\n"
        "BlueZ/device commands:\n"
        "  list                 List known BlueZ devices\n"
        "  scan                 Run BlueZ discovery, then list devices\n"
        "  info                 Show one device's BlueZ properties\n"
        "  probe                Connect if needed and inspect BLE-MIDI GATT objects\n"
        "  pair                 Pair a device through BlueZ Device1.Pair\n"
        "  trust                Set BlueZ Device1.Trusted=true\n"
        "  untrust              Set BlueZ Device1.Trusted=false\n"
        "  connect              Prepare a BLE-MIDI device using BlueZ\n"
        "  configure            Write a local midi-ble-rtd config file\n"
        "  disconnect           Disconnect a BlueZ device\n"
        "  forget               Remove a device from BlueZ cache/pairing records\n"
        "\n"
        "Daemon control commands:\n"
        "  daemon-ping          Ping a running midi-ble-rtd instance\n"
        "  daemon-status        Print daemon runtime status, including RTKit flags\n"
        "  daemon-list          List configured daemon sessions\n"
        "  daemon-connect       Ask daemon to connect a configured device\n"
        "  daemon-disconnect    Ask daemon to disconnect a configured device\n"
        "  daemon-recheck       Ask daemon to recheck a configured device\n"
        "\n"
        "Diagnostics commands:\n"
        "  stats                Print stats.tsv snapshot\n"
        "  top                  Watch stats.tsv continuously\n"
        "  latency              Print latency.tsv diagnostics snapshot\n"
        "  latency-top          Watch latency.tsv diagnostics continuously\n"
        "\n"
        "Version:\n"
        "  -v, --version        Print version\n"
        "  -vv                  Print version and compiled feature summary\n"
        "\n"
        "Help:\n"
        "  %s help configure\n"
        "  %s connect --help\n"
        "  %s help daemon-status\n"
        "  %s daemon-status --help\n"
        "  %s help latency\n"
        "  %s latency-top --help\n"
        "\n"
        "Typical GO:KEYS flow:\n"
        "  %s scan --timeout 10 --midi-only\n"
        "  %s connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config\n"
        "  %s probe CB:81:F4:62:FF:07\n"
        "  midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini\n"
        "\n",
        argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0);
}

static void daemon_status_help_command(const char *argv0) {
    g_print(
        "Usage:\n"
        "  %s daemon-status\n"
        "\n"
        "Description:\n"
        "  Prints one runtime status line from the running midi-ble-rtd instance.\n"
        "  The command uses the daemon control socket and does not change daemon state.\n"
        "\n"
        "Output fields:\n"
        "  devices\n"
        "      Number of configured device sessions known to the daemon.\n"
        "\n"
        "  streaming\n"
        "      Number of configured device sessions currently in STREAMING state.\n"
        "\n"
        "  alsa_tx_thread\n"
        "      Shared ALSA Sequencer TX thread state: running or stopped.\n"
        "\n"
        "  rx_workers, tx_workers\n"
        "      Number of per-device runtime worker threads currently allocated.\n"
        "\n"
        "  rtkit\n"
        "      RTKit scheduling requested by daemon configuration: on or off.\n"
        "\n"
        "  rtkit_priority\n"
        "      Configured RTKit realtime priority after daemon-side clamping.\n"
        "\n"
        "  rtkit_rx, rtkit_tx\n"
        "      Whether RTKit is enabled for RX and TX runtime worker classes: on or off.\n"
        "\n"
        "  lifecycle_busy\n"
        "      Whether the daemon lifecycle/control-plane queue is currently executing a command.\n"
        "\n"
        "  lifecycle_queue\n"
        "      Number of pending lifecycle/control-plane commands.\n"
        "\n"
        "Example:\n"
        "  OK STATUS devices=1 streaming=1 alsa_tx_thread=running rx_workers=1 tx_workers=1 rtkit=on rtkit_priority=1 rtkit_rx=on rtkit_tx=off lifecycle_busy=no lifecycle_queue=0\n"
        "\n",
        argv0);
}

static void latency_usage(const char *argv0, const char *cmd) {
    g_printerr(
        "Usage:\n"
        "  %s %s [--path PATH]\n"
        "  %s latency-top [--path PATH] [--interval MS]\n",
        argv0, cmd, argv0);
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

    if (argc == 2 && is_version_arg(argv[1])) {
        ctl_print_version(false);
        g_free(display_argv);
        return 0;
    }

    if (argc == 2 && is_verbose_version_arg(argv[1])) {
        ctl_print_version(true);
        g_free(display_argv);
        return 0;
    }

    if (argc == 3 && g_strcmp0(argv[1], "help") == 0 &&
        g_strcmp0(argv[2], "daemon-status") == 0) {
        daemon_status_help_command(ctl_argv0());
        g_free(display_argv);
        return 0;
    }

    if (argc == 3 && g_strcmp0(argv[1], "daemon-status") == 0 && is_help_arg(argv[2])) {
        daemon_status_help_command(ctl_argv0());
        g_free(display_argv);
        return 0;
    }

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
        ctl_help_global();
        g_free(display_argv);
        return 0;
    }

    int rc = midi_ble_rtctl_stats_main(argc, display_argv);
    g_free(display_argv);
    return rc;
}
