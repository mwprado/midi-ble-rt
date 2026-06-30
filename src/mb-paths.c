#include "mb-paths.h"

char *mb_runtime_default_dir(void) {
    const char *runtime_dir = g_get_user_runtime_dir();
    if (!runtime_dir || !*runtime_dir)
        runtime_dir = g_get_tmp_dir();

    return g_build_filename(runtime_dir, MB_RUNTIME_DIR_NAME, NULL);
}

char *mb_runtime_build_path(const char *leaf_name) {
    char *dir = mb_runtime_default_dir();
    char *path = g_build_filename(dir, leaf_name, NULL);
    g_free(dir);
    return path;
}

char *mb_runtime_control_socket_default_path(void) {
    return mb_runtime_build_path(MB_CONTROL_SOCKET_NAME);
}

char *mb_runtime_stats_default_path(void) {
    return mb_runtime_build_path(MB_STATS_FILE_NAME);
}
