#ifndef MB_PATHS_H
#define MB_PATHS_H

#include <glib.h>

G_BEGIN_DECLS

#define MB_RUNTIME_DIR_NAME "midi-ble-rt"
#define MB_CONTROL_SOCKET_NAME "control.sock"
#define MB_STATS_FILE_NAME "stats.tsv"

char *mb_runtime_default_dir(void);
char *mb_runtime_build_path(const char *leaf_name);
char *mb_runtime_control_socket_default_path(void);
char *mb_runtime_stats_default_path(void);

G_END_DECLS

#endif /* MB_PATHS_H */
