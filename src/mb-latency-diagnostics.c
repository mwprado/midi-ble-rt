#include "mb-latency-diagnostics.h"

#include "mb-paths.h"
#include "mb-timeouts.h"

#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MB_LATENCY_BUCKETS 256u

typedef struct {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t bucket[MB_LATENCY_BUCKETS];
} MbLatencyAccumulator;

typedef struct {
    bool enabled;
    unsigned interval_ms;
    uint64_t started_ns;
    uint64_t window_started_ns;
    uint64_t last_export_ns;
    char *path;

    MbLatencyAccumulator rx_queue;
    MbLatencyAccumulator rx_total;
    MbLatencyAccumulator tx_queue;
    MbLatencyAccumulator tx_total;
} MbLatencyState;

G_LOCK_DEFINE_STATIC(latency_state);
static MbLatencyState latency_state;
static gint latency_enabled_flag;

static uint64_t latency_now_ns(void) {
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static uint64_t latency_ns_to_ms_u64(uint64_t ns) {
    return ns / 1000000ULL;
}

static double latency_ns_to_ms_double(uint64_t ns) {
    return ((double)ns) / 1000000.0;
}

static void latency_accumulator_reset(MbLatencyAccumulator *acc) {
    if (acc)
        memset(acc, 0, sizeof(*acc));
}

static void latency_accumulator_observe(MbLatencyAccumulator *acc, uint64_t latency_ns) {
    if (!acc)
        return;

    if (acc->count != UINT64_MAX)
        acc->count++;

    if (UINT64_MAX - acc->total_ns < latency_ns)
        acc->total_ns = UINT64_MAX;
    else
        acc->total_ns += latency_ns;

    if (latency_ns > acc->max_ns)
        acc->max_ns = latency_ns;

    uint64_t bucket = latency_ns_to_ms_u64(latency_ns);
    if (bucket >= MB_LATENCY_BUCKETS)
        bucket = MB_LATENCY_BUCKETS - 1u;
    acc->bucket[bucket]++;
}

static double latency_accumulator_avg_ms(const MbLatencyAccumulator *acc) {
    if (!acc || acc->count == 0)
        return 0.0;
    return latency_ns_to_ms_double(acc->total_ns) / (double)acc->count;
}

static unsigned latency_accumulator_percentile_ms(const MbLatencyAccumulator *acc, unsigned percentile) {
    if (!acc || acc->count == 0)
        return 0;

    if (percentile > 100)
        percentile = 100;

    uint64_t target = (acc->count * percentile + 99u) / 100u;
    if (target == 0)
        target = 1;

    uint64_t seen = 0;
    for (unsigned i = 0; i < MB_LATENCY_BUCKETS; i++) {
        seen += acc->bucket[i];
        if (seen >= target)
            return i;
    }

    return MB_LATENCY_BUCKETS - 1u;
}

static const char *latency_direction_name(MbLatencyDirection direction) {
    return direction == MB_LATENCY_TX ? "TX" : "RX";
}

static void latency_append_row(GString *content,
                               MbLatencyDirection direction,
                               const char *metric,
                               const MbLatencyAccumulator *acc,
                               uint64_t uptime_ms,
                               uint64_t window_ms) {
    g_string_append_printf(
        content,
        "global\t%s\t%s\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%" G_GUINT64_FORMAT "\t%.3f\t%u\t%u\t%.3f\n",
        latency_direction_name(direction),
        metric,
        uptime_ms,
        window_ms,
        acc ? acc->count : 0,
        latency_accumulator_avg_ms(acc),
        latency_accumulator_percentile_ms(acc, 95),
        latency_accumulator_percentile_ms(acc, 99),
        acc ? latency_ns_to_ms_double(acc->max_ns) : 0.0);
}

static void latency_reset_window_locked(uint64_t now_ns) {
    latency_accumulator_reset(&latency_state.rx_queue);
    latency_accumulator_reset(&latency_state.rx_total);
    latency_accumulator_reset(&latency_state.tx_queue);
    latency_accumulator_reset(&latency_state.tx_total);
    latency_state.window_started_ns = now_ns;
    latency_state.last_export_ns = now_ns;
}

static char *latency_build_tsv_locked(uint64_t now_ns) {
    uint64_t uptime_ms = 0;
    uint64_t window_ms = 0;

    if (latency_state.started_ns != 0 && now_ns >= latency_state.started_ns)
        uptime_ms = latency_ns_to_ms_u64(now_ns - latency_state.started_ns);
    if (latency_state.window_started_ns != 0 && now_ns >= latency_state.window_started_ns)
        window_ms = latency_ns_to_ms_u64(now_ns - latency_state.window_started_ns);

    GString *content = g_string_new(
        "v1\n"
        "scope\tdir\tmetric\tuptime_ms\twindow_ms\tcount\tavg_ms\tp95_ms\tp99_ms\tmax_ms\n");

    latency_append_row(content, MB_LATENCY_RX, "queue", &latency_state.rx_queue, uptime_ms, window_ms);
    latency_append_row(content, MB_LATENCY_RX, "total", &latency_state.rx_total, uptime_ms, window_ms);
    latency_append_row(content, MB_LATENCY_TX, "queue", &latency_state.tx_queue, uptime_ms, window_ms);
    latency_append_row(content, MB_LATENCY_TX, "total", &latency_state.tx_total, uptime_ms, window_ms);

    return g_string_free(content, FALSE);
}

static void latency_write_file(const char *path, const char *content) {
    if (!path || !content)
        return;

    char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_printerr("Latency diagnostics export failed: could not create %s.\n", dir);
        g_free(dir);
        return;
    }
    g_free(dir);

    GError *error = NULL;
    if (!g_file_set_contents(path, content, -1, &error)) {
        g_printerr("Latency diagnostics export failed: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

char *mb_latency_diagnostics_default_path(void) {
    return mb_runtime_latency_default_path();
}

void mb_latency_diagnostics_configure(bool enabled, unsigned interval_ms) {
    if (!enabled)
        g_atomic_int_set(&latency_enabled_flag, 0);

    uint64_t now_ns = latency_now_ns();

    G_LOCK(latency_state);
    g_free(latency_state.path);
    memset(&latency_state, 0, sizeof(latency_state));

    latency_state.enabled = enabled;
    latency_state.interval_ms = interval_ms ? interval_ms : MB_STATS_DEFAULT_INTERVAL_MS;
    latency_state.started_ns = now_ns;
    latency_state.window_started_ns = now_ns;
    latency_state.last_export_ns = now_ns;
    if (enabled)
        latency_state.path = mb_latency_diagnostics_default_path();
    G_UNLOCK(latency_state);

    if (enabled)
        g_atomic_int_set(&latency_enabled_flag, 1);
}

bool mb_latency_diagnostics_enabled(void) {
    return g_atomic_int_get(&latency_enabled_flag) != 0;
}

void mb_latency_diagnostics_record(MbLatencyDirection direction,
                                   uint64_t queued_ns,
                                   uint64_t consume_started_ns,
                                   uint64_t consume_finished_ns) {
    if (!mb_latency_diagnostics_enabled())
        return;

    if (queued_ns == 0 || consume_started_ns < queued_ns || consume_finished_ns < consume_started_ns)
        return;

    uint64_t queue_ns = consume_started_ns - queued_ns;
    uint64_t total_ns = consume_finished_ns - queued_ns;
    char *content = NULL;
    char *path = NULL;

    G_LOCK(latency_state);
    if (latency_state.enabled && latency_state.path) {
        if (direction == MB_LATENCY_TX) {
            latency_accumulator_observe(&latency_state.tx_queue, queue_ns);
            latency_accumulator_observe(&latency_state.tx_total, total_ns);
        } else {
            latency_accumulator_observe(&latency_state.rx_queue, queue_ns);
            latency_accumulator_observe(&latency_state.rx_total, total_ns);
        }

        uint64_t interval_ns = (uint64_t)latency_state.interval_ms * 1000000ULL;
        if (interval_ns == 0)
            interval_ns = (uint64_t)MB_STATS_DEFAULT_INTERVAL_MS * 1000000ULL;

        if (consume_finished_ns >= latency_state.last_export_ns &&
            consume_finished_ns - latency_state.last_export_ns >= interval_ns) {
            content = latency_build_tsv_locked(consume_finished_ns);
            path = g_strdup(latency_state.path);
            latency_reset_window_locked(consume_finished_ns);
        }
    }
    G_UNLOCK(latency_state);

    if (content && path)
        latency_write_file(path, content);

    g_free(content);
    g_free(path);
}
