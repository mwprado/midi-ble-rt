#ifndef MB_STATS_H
#define MB_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    /* Window counters. They are reset after each stats export. */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_drops;
    uint64_t tx_drops;
    uint64_t window_started_ns;
    unsigned rx_queue_high_watermark;
    unsigned tx_queue_high_watermark;

    /* Timing diagnostics since daemon start. */
    uint64_t last_rx_ns;
    uint64_t last_tx_ns;
    uint64_t rx_gap_avg_ns;
    uint64_t rx_gap_max_ns;
    uint64_t tx_gap_avg_ns;
    uint64_t tx_gap_max_ns;
} MbSessionStats;

typedef struct {
    bool enabled;
    unsigned interval_ms;
    uint64_t started_ns;
    char *path;
    MbSessionStats session;
} MbStats;

void mb_stats_init(MbStats *stats, bool enabled, unsigned interval_ms);
void mb_stats_clear(MbStats *stats);

void mb_stats_rx_packet(MbStats *stats, size_t bytes, uint64_t now_ns);
void mb_stats_tx_packet(MbStats *stats, size_t bytes, uint64_t now_ns);
void mb_stats_rx_drop(MbStats *stats);
void mb_stats_tx_drop(MbStats *stats);
void mb_stats_queue_depth(MbStats *stats, unsigned rx_queue_depth, unsigned tx_queue_depth);
void mb_stats_observe_runtime_queue_depth(bool tx, unsigned queue_depth);

char *mb_stats_default_path(void);
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
                         GError **error);

G_END_DECLS

#endif /* MB_STATS_H */
