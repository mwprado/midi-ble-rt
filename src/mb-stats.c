#include "mb-stats.h"
#include "mb-timeouts.h"

#include "mb-paths.h"

#include <stdint.h>
#include <string.h>

static void add_u64_saturating(uint64_t *value, uint64_t add) {
    if (!value)
        return;
    if (UINT64_MAX - *value < add)
        *value = UINT64_MAX;
    else
        *value += add;
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

char *mb_stats_default_path(void) {
    return mb_runtime_stats_default_path();
}
