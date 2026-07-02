#ifndef MB_LATENCY_DIAGNOSTICS_H
#define MB_LATENCY_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    MB_LATENCY_RX = 0,
    MB_LATENCY_TX = 1,
} MbLatencyDirection;

char *mb_latency_diagnostics_default_path(void);
void mb_latency_diagnostics_configure(bool enabled, unsigned interval_ms);
bool mb_latency_diagnostics_enabled(void);
void mb_latency_diagnostics_record(MbLatencyDirection direction,
                                   uint64_t queued_ns,
                                   uint64_t consume_started_ns,
                                   uint64_t consume_finished_ns);

G_END_DECLS

#endif /* MB_LATENCY_DIAGNOSTICS_H */
