#include "mb-stats.h"
#include "mb-timeouts.h"

#include "mb-paths.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

G_LOCK_DEFINE_STATIC(observed_queue_depth);
static unsigned observed_rx_queue_high_watermark;
static unsigned observed_tx_queue_high_watermark;

static uint64_t ns_to_ms(uint64_t ns) {
    return ns / 1000000ULL;
}

static uint64_t elapsed_ms(uint64_t now_ns, uint64_t then_ns) {
    if (then_ns == 0 || now_ns < then_ns)
        return 0;
    return ns_to_ms(now_ns - then_ns);
}

static void add_u64_saturating(uint64_t *value, uint64_t add) {
    if (!value)
        return;
    if (UINT64_MAX - *value < add)
        *value = UINT64_MAX;
    else
        *value += add;
}

static double rate_per_sec(uint64_t count, uint64_t window_ns) {
    if (window_ns < 100000000ULL)
        return 0.0;
    return ((double)count * 1000000000.0) / (double)window_ns;
}

static void reset_window(MbSessionStats *s, uint64_t now_ns) {
    if (!s)
        return;
    s->rx_packets = 0;
    s->tx_packets = 0;
    s->rx_bytes = 0;
    s->tx_bytes = 0;
    s->rx_drops = 0;
    s->tx_drops = 0;
    s->rx_queue_high_watermark = 0;
    s->tx_queue_high_watermark = 0;
    s->window_started_ns = now_ns;
}

static void update_gap(uint64_t *last_ns,
                       uint64_t *avg_ns,
                       uint64_t *max_ns,
                       uint64_t now_ns) {
    if (*last_ns != 0 && now_ns >= *last_ns) {
        uint64_t gap = now_ns - *last_ns;
        if (*avg_ns == 0)
            *avg_ns = gap;
        else
            *avg_ns = ((*avg_ns * 7ULL) + gap) / 8ULL;
        if (gap > *max_ns)
            *max_ns = gap;
    }
    *last_ns = now_ns;
}

void mb_stats_init(MbStats *stats, bool enabled, unsigned interval_ms) {
    if (!stats)
        return;

    memset(stats, 0, sizeof(*stats));
    stats->enabled = enabled;
    stats->interval_ms = interval_ms ? interval_ms : MB_STATS_DEFAULT_INTERVAL_MS;
    stats->started_ns = (uint64_t)g_get_monotonic_time() * 1000ULL;
    stats->session.window_started_ns = stats->started_ns;
    if (enabled)
        stats->path = mb_stats_default_path();
}

void mb_stats_clear(MbStats *stats) {
    if (!stats)
        return;
    g_free(stats->path);
    memset(stats, 0, sizeof(*stats));
}

void mb_stats_rx_packet(MbStats *stats, size_t bytes, uint64_t now_ns) {
    if (!stats || !stats->enabled)
        return;
    add_u64_saturating(&stats->session.rx_packets, 1);
    add_u64_saturating(&stats->session.rx_bytes, (uint64_t)bytes);
    update_gap(&stats->session.last_rx_ns,
               &stats->session.rx_gap_avg_ns,
               &stats->session.rx_gap_max_ns,
               now_ns);
}

void mb_stats_tx_packet(MbStats *stats, size_t bytes, uint64_t now_ns) {
    if (!stats || !stats->enabled)
        return;
    add_u64_saturating(&stats->session.tx_packets, 1);
    add_u64_saturating(&stats->session.tx_bytes, (uint64_t)bytes);
    update_gap(&stats->session.last_tx_ns,
               &stats->session.tx_gap_avg_ns,
               &stats->session.tx_gap_max_ns,
               now_ns);
}

void mb_stats_rx_drop(MbStats *stats) {
    if (!stats || !stats->enabled)
        return;
    add_u64_saturating(&stats->session.rx_drops, 1);
}

void mb_stats_tx_drop(MbStats *stats) {
    if (!stats || !stats->enabled)
        return;
    add_u64_saturating(&stats->session.tx_drops, 1);
}

void mb_stats_queue_depth(MbStats *stats, unsigned rx_queue_depth, unsigned tx_queue_depth) {
    if (!stats || !stats->enabled)
        return;
    if (rx_queue_depth > stats->session.rx_queue_high_watermark)
        stats->session.rx_queue_high_watermark = rx_queue_depth;
    if (tx_queue_depth > stats->session.tx_queue_high_watermark)
        stats->session.tx_queue_high_watermark = tx_queue_depth;
}

void mb_stats_observe_runtime_queue_depth(bool tx, unsigned queue_depth) {
    G_LOCK(observed_queue_depth);
    if (tx) {
        if (queue_depth > observed_tx_queue_high_watermark)
            observed_tx_queue_high_watermark = queue_depth;
    } else {
        if (queue_depth > observed_rx_queue_high_watermark)
            observed_rx_queue_high_watermark = queue_depth;
    }
    G_UNLOCK(observed_queue_depth);
}

static void consume_observed_runtime_queue_depth(MbStats *stats) {
    if (!stats || !stats->enabled)
        return;

    G_LOCK(observed_queue_depth);
    if (observed_rx_queue_high_watermark > stats->session.rx_queue_high_watermark)
        stats->session.rx_queue_high_watermark = observed_rx_queue_high_watermark;
    if (observed_tx_queue_high_watermark > stats->session.tx_queue_high_watermark)
        stats->session.tx_queue_high_watermark = observed_tx_queue_high_watermark;
    observed_rx_queue_high_watermark = 0;
    observed_tx_queue_high_watermark = 0;
    G_UNLOCK(observed_queue_depth);
}

char *mb_stats_default_path(void) {
    return mb_runtime_stats_default_path();
}

bool mb_stats_export_tsv(MbStats *stats,
                         const char *label,
                         const char *address,
                         const char *state,
                         int alsa_rx_client_id,
                         int alsa_rx_port_id,
                         int alsa_tx_client_id,
                         int alsa_tx_port_id,
                         unsigned rx_queue_depth,
                         unsigned tx_queue_depth,
                         uint64_t now_ns,
                         GError **error) {
    if (!stats || !stats->enabled || !stats->path)
        return true;

    mb_stats_queue_depth(stats, rx_queue_depth, tx_queue_depth);
    consume_observed_runtime_queue_depth(stats);

    char *dir = g_path_get_dirname(stats->path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "failed to create stats directory: %s", dir);
        g_free(dir);
        reset_window(&stats->session, now_ns);
        return false;
    }
    g_free(dir);

    MbSessionStats *s = &stats->session;
    uint64_t uptime_ms = elapsed_ms(now_ns, stats->started_ns);
    uint64_t last_rx_ms = elapsed_ms(now_ns, s->last_rx_ns);
    uint64_t last_tx_ms = elapsed_ms(now_ns, s->last_tx_ns);
    uint64_t window_ns = 0;
    if (s->window_started_ns != 0 && now_ns >= s->window_started_ns)
        window_ns = now_ns - s->window_started_ns;
    uint64_t window_ms = ns_to_ms(window_ns);

    char *content = g_strdup_printf(
        "v4\n"
        "label\taddress\tstate\talsa_rx_client_id\talsa_rx_port_id\talsa_tx_client_id\talsa_tx_port_id\tuptime_ms\twindow_ms\trx_packets\ttx_packets\trx_bytes\ttx_bytes\trx_drops\ttx_drops\trx_packets_per_sec\ttx_packets_per_sec\trx_bytes_per_sec\ttx_bytes_per_sec\trx_drops_per_sec\ttx_drops_per_sec\tlast_rx_ms\tlast_tx_ms\trx_gap_avg_ms\trx_gap_max_ms\ttx_gap_avg_ms\ttx_gap_max_ms\trx_queue_depth\ttx_queue_depth\trx_queue_high_watermark\ttx_queue_high_watermark\n"
        "%s\t%s\t%s\t%d\t%d\t%d\t%d\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%u\t%u\t%u\t%u\n",
        label && *label ? label : "-",
        address && *address ? address : "-",
        state && *state ? state : "-",
        alsa_rx_client_id,
        alsa_rx_port_id,
        alsa_tx_client_id,
        alsa_tx_port_id,
        uptime_ms,
        window_ms,
        s->rx_packets,
        s->tx_packets,
        s->rx_bytes,
        s->tx_bytes,
        s->rx_drops,
        s->tx_drops,
        rate_per_sec(s->rx_packets, window_ns),
        rate_per_sec(s->tx_packets, window_ns),
        rate_per_sec(s->rx_bytes, window_ns),
        rate_per_sec(s->tx_bytes, window_ns),
        rate_per_sec(s->rx_drops, window_ns),
        rate_per_sec(s->tx_drops, window_ns),
        last_rx_ms,
        last_tx_ms,
        ns_to_ms(s->rx_gap_avg_ns),
        ns_to_ms(s->rx_gap_max_ns),
        ns_to_ms(s->tx_gap_avg_ns),
        ns_to_ms(s->tx_gap_max_ns),
        rx_queue_depth,
        tx_queue_depth,
        s->rx_queue_high_watermark,
        s->tx_queue_high_watermark);

    bool ok = g_file_set_contents(stats->path, content, -1, error);
    reset_window(&stats->session, now_ns);
    g_free(content);
    return ok;
}
