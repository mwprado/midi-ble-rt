/*
 * midi-ble-rtstats
 *
 * Human-readable viewer for midi-ble-rtd session statistics.
 * The daemon still exports the machine-readable TSV file.
 */

#include "mb-stats.h"

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **header;
    char **value;
    gsize n_header;
    gsize n_value;
} StatsTable;

static void usage(const char *argv0) {
    g_printerr(
        "Usage:\n"
        "  %s [--path PATH]\n"
        "  %s top [--path PATH] [--interval MS]\n"
        "\n"
        "Default path:\n"
        "  /run/user/$UID/midi-ble-rt/stats.tsv\n",
        argv0, argv0);
}

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
    g_strfreev(t->header);
    g_strfreev(t->value);
    memset(t, 0, sizeof(*t));
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

    if (!lines || g_strcmp0(lines[0], "v1") != 0 || !lines[1] || !lines[2]) {
        g_printerr("Unsupported or incomplete stats file: %s\n", path);
        g_strfreev(lines);
        return false;
    }

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

static uint64_t u64_field(const StatsTable *t, const char *key) {
    const char *s = stats_get(t, key);
    if (!s || g_strcmp0(s, "-") == 0)
        return 0;
    return g_ascii_strtoull(s, NULL, 10);
}

static void print_stats_aligned(const char *path, const StatsTable *t) {
    const char *label = stats_get(t, "label");
    const char *address = stats_get(t, "address");
    const char *state = stats_get(t, "state");

    g_print("Stats file: %s\n", path);
    g_print("Session:    %s\n", label);
    g_print("Address:    %s\n", address);
    g_print("State:      %s\n", state);
    g_print("Uptime:     %s ms\n", stats_get(t, "uptime_ms"));
    g_print("\n");

    g_print("%-8s %12s %12s %12s %12s %12s\n",
            "DIR", "PACKETS", "BYTES", "DROPS", "LAST_MS", "QDEPTH");
    g_print("%-8s %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12s %12s\n",
            "RX",
            u64_field(t, "rx_packets"),
            u64_field(t, "rx_bytes"),
            u64_field(t, "rx_drops"),
            stats_get(t, "last_rx_ms"),
            stats_get(t, "rx_queue_depth"));
    g_print("%-8s %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12" G_GUINT64_FORMAT " %12s %12s\n",
            "TX",
            u64_field(t, "tx_packets"),
            u64_field(t, "tx_bytes"),
            u64_field(t, "tx_drops"),
            stats_get(t, "last_tx_ms"),
            stats_get(t, "tx_queue_depth"));
    g_print("\n");

    g_print("%-8s %12s %12s\n", "DIR", "GAP_AVG_MS", "GAP_MAX_MS");
    g_print("%-8s %12s %12s\n", "RX", stats_get(t, "rx_gap_avg_ms"), stats_get(t, "rx_gap_max_ms"));
    g_print("%-8s %12s %12s\n", "TX", stats_get(t, "tx_gap_avg_ms"), stats_get(t, "tx_gap_max_ms"));
}

static int print_once(const char *path) {
    StatsTable t;
    if (!read_stats_table(path, &t))
        return 1;
    print_stats_aligned(path, &t);
    stats_table_clear(&t);
    return 0;
}

int main(int argc, char **argv) {
    bool watch = false;
    unsigned interval_ms = 1000;
    char *path = mb_stats_default_path();

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "top") == 0 || g_strcmp0(argv[i], "--watch") == 0) {
            watch = true;
        } else if (g_strcmp0(argv[i], "--path") == 0 && i + 1 < argc) {
            g_free(path);
            path = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_ms = (unsigned)atoi(argv[++i]);
            if (interval_ms < 100)
                interval_ms = 100;
            if (interval_ms > 60000)
                interval_ms = 60000;
        } else if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0) {
            usage(argv[0]);
            g_free(path);
            return 0;
        } else {
            usage(argv[0]);
            g_free(path);
            return 2;
        }
    }

    if (!watch) {
        int rc = print_once(path);
        g_free(path);
        return rc;
    }

    for (;;) {
        g_print("\033[H\033[J");
        (void)print_once(path);
        g_usleep((gulong)interval_ms * 1000UL);
    }
}
