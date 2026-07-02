#include "mb-config.h"
#include "mb-latency-diagnostics.h"

static bool mb_daemon_config_load_dir_with_latency(MbConfig *cfg, const char *dir_path);

#define mb_config_load_dir mb_daemon_config_load_dir_with_latency
#include "mb-daemon.c"
#undef mb_config_load_dir

static bool mb_daemon_config_load_dir_with_latency(MbConfig *cfg, const char *dir_path) {
    bool ok = mb_config_load_dir(cfg, dir_path);
    if (!ok) {
        mb_latency_diagnostics_configure(false, 0);
        return false;
    }

    mb_latency_diagnostics_configure(cfg->stats_latency_diagnostics,
                                     cfg->stats_interval_ms);
    return true;
}
