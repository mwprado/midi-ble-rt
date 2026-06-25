/*
 * stats/top front-end for midi-ble-rtctl.
 *
 * CMake renames the original midi-ble-rtctl.c main() to
 * midi_ble_rtctl_bluez_main().  This file provides the public main(), handles
 * local stats commands, and delegates all BlueZ commands to the original CLI.
 */

#include "mb-stats.h"

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int midi_ble_rtctl_bluez_main(int argc, char **argv);

typedef struct {
    char *version;
    char **header;
    char **value;
    gsize n_header;
    gsize n_value;
} StatsTable;

static gsize strv_len(char **v) {
    gsize n = 0;
    if (!v)
        return 0;
    while (v[n])
        n++;
    return n;
}

static void stats_table_clear(StatsTable *t) {
    if (!t)
        return;
    g_free(t->version);
    g_strfreev(t->header);
    g_strfreev(t->value);
    memset(t, 0, sizeof(*t));
}

static bool stats_is_v2(const StatsTable *t) {
    return t && g_strcmp0(t->version, "v2") == 0;
}

static const char *stats_get(const StatsTable *t, const char *key) {
    if (!t || !key)
        return "-";
    for (gsize i = 0; i < t->n_header && i < t->n_value; i++) {
        if (g_strcmp0(t->header[i], key) == 0)
            return t->value[i] && *t->value[i] ? t->value[i] : "-";
    }
    return "-";
}

static guint64 stats_u64(const StatsTable *t, const char *key) {
    const char *s = stats_get(t, key);
    if (!s || g_strcmp0(s, "-") == 0)
        return 0;
    return g_ascii_strtoull(s, NULL, 10);
}

static const char *stats_rate(const StatsTable *t, const char *key) {
    const char *s = stats_get(t, key);
    return s && *s ? s : "0.000";
}

static bool read_stats_table(const char *path, StatsTable *out) {
    char *content = NULL;
    GError *error = NULL;

    memset(out, 0, sizeof(*out));

    if (!g_file_get_contents(path, &content, NULL, &error)) {
        g_printerr("Could not read stats file: %s\n", path);
        g_printerr("Reason: %s\n", error ? error->message : "unknown error");
        g_printerr("Start midi-ble-rtd with stats enabled first.\n");
        g_clear_error(&error);
        return false;
    }

    char **lines = g_strsplit(content, "\n", 0);
    g_free(content);

    bool supported = lines &&
                     (g_strcmp0(lines[0], "v1") == 0 ||
                      g_strcmp0(lines[0], "v2") == 0) &&
                     lines[1] && lines[2];
    if (!supported) {
        g_printerr("Unsupported or incomplete stats file: %s\n", path);
        if (lines && lines[0])
            g_printerr("Stats version found: %s\n", lines[0]);
        g_strfreev(lines);
        return false;
    }

    out->version = g_strdup(lines[0]);
    out->header = g_strsplit(lines[1], "\t", 0);
    out->value = g_strsplit(lines[2], "\t", 0);
    out->n_header = strv_len(out->header);
    out->n_value = strv_len(out->value);
    g_strfreev(lines);

    if (out->n_header == 0 || out->n_value == 0) {
        g_printerr("Empty stats table: %s\n", path);
        stats_table_clear(out);
        return false;
    }

    return true;
}

static void print_stats_aligned(const char *path, const StatsTable *t) {
    g_print("Stats file: %s\n", path);
    g_print("Format:     %s\n", t->version ? t->version : "-");
    g_print("Session:    %s\n", stats_get(t, "label"));
    g_print("Address:    %s\n", stats_get(t, "address"));
    g_print("State:      %s\n", stats_get(t, "state"));
    g_print("Uptime:     %s ms\n", stats_get(t, "uptime_ms"));
    if (stats_is_v2(t))
        g_print("Window:     %s ms\n", stats_get(t, "window_ms"));
    g_print("\n");

    if (stats_is_v2(t)) {
        g_print("%-8s %12s %12s %12s %12s %12s %12s %12s\n",
                "DIR", "PKT_WIN", "B_WIN", "DROP_WIN", "PKT/s", "B/s", "DROP/s", "QDEPTH");
        g_print("%-8s %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12s %12s %12s %12s\n",
                "RX",
                stats_u64(t, "rx_packets"),
                stats_u64(t, "rx_bytes"),
                stats_u64(t, "rx_drops"),
                stats_rate(t, "rx_packets_per_sec"),
                stats_rate(t, "rx_bytes_per_sec"),
                stats_rate(t, "rx_drops_per_sec"),
                stats_get(t, "rx_queue_depth"));
        g_print("%-8s %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12s %12s %12s %12s\n",
                "TX",
                stats_u64(t, "tx_packets"),
                stats_u64(t, "tx_bytes"),
                stats_u64(t, "tx_drops"),
                stats_rate(t, "tx_packets_per_sec"),
                stats_rate(t, "tx_bytes_per_sec"),
                stats_rate(t, "tx_drops_per_sec"),
                stats_get(t, "tx_queue_depth"));
    } else {
        g_print("%-8s %12s %12s %12s %12s %12s\n",
                "DIR", "PACKETS", "BYTES", "DROPS", "LAST_MS", "QDEPTH");
        g_print("%-8s %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12s %12s\n",
                "RX",
                stats_u64(t, "rx_packets"),
                stats_u64(t, "rx_bytes"),
                stats_u64(t, "rx_drops"),
                stats_get(t, "last_rx_ms"),
                stats_get(t, "rx_queue_depth"));
        g_print("%-8s %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12s %12s\n",
                "TX",
                stats_u64(t, "tx_packets"),
                stats_u64(t, "tx_bytes"),
                stats_u64(t, "tx_drops"),
                stats_get(t, "last_tx_ms"),
                stats_get(t, "tx_queue_depth"));
    }
    g_print("\n");

    g_print("%-8s %12s %12s %12s\n", "DIR", "LAST_MS", "GAP_AVG_MS", "GAP_MAX_MS");
    g_print("%-8s %12s %12s %12s\n",
            "RX",
            stats_get(t, "last_rx_ms"),
            stats_get(t, "rx_gap_avg_ms"),
            stats_get(t, "rx_gap_max_ms"));
    g_print("%-8s %12s %12s %12s\n",
            "TX",
            stats_get(t, "last_tx_ms"),
            stats_get(t, "tx_gap_avg_ms"),
            stats_get(t, "tx_gap_max_ms"));
}

static int cmd_stats_print(const char *path) {
    StatsTable t;
    if (!read_stats_table(path, &t))
        return 1;
    print_stats_aligned(path, &t);
    stats_table_clear(&t);
    return 0;
}

static void stats_usage(const char *argv0, const char *cmd) {
    g_printerr(
        "Usage:\n"
        "  %s %s [--path PATH]\n"
        "  %s top [--path PATH] [--interval MS]\n",
        argv0, cmd, argv0);
}

static int cmd_stats_like(int argc, char **argv, bool watch) {
    char *path = mb_stats_default_path();
    unsigned interval_ms = 1000;

    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--path") == 0 && i + 1 < argc) {
            g_free(path);
            path = g_strdup(argv[++i]);
        } else if (watch && g_strcmp0(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_ms = (unsigned)atoi(argv[++i]);
            if (interval_ms < 100)
                interval_ms = 100;
            if (interval_ms > 60000)
                interval_ms = 60000;
        } else {
            stats_usage(argv[0], watch ? "top" : "stats");
            g_free(path);
            return 2;
        }
    }

    if (!watch) {
        int rc = cmd_stats_print(path);
        g_free(path);
        return rc;
    }

    for (;;) {
        g_print("\033[H\033[J");
        (void)cmd_stats_print(path);
        g_usleep((gulong)interval_ms * 1000UL);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && g_strcmp0(argv[1], "stats") == 0)
        return cmd_stats_like(argc, argv, false);
    if (argc >= 2 && g_strcmp0(argv[1], "top") == 0)
        return cmd_stats_like(argc, argv, true);

    return midi_ble_rtctl_bluez_main(argc, argv);
}
