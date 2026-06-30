#include "mb-paths.h"

#include <glib.h>

static void test_runtime_default_dir_uses_project_leaf(void) {
    char *dir = mb_runtime_default_dir();

    g_assert_nonnull(dir);
    g_assert_true(g_str_has_suffix(dir, G_DIR_SEPARATOR_S MB_RUNTIME_DIR_NAME));

    g_free(dir);
}

static void test_runtime_stats_path_uses_owned_leaf(void) {
    char *path = mb_runtime_stats_default_path();

    g_assert_nonnull(path);
    g_assert_true(g_str_has_suffix(path,
                                   G_DIR_SEPARATOR_S MB_RUNTIME_DIR_NAME
                                   G_DIR_SEPARATOR_S MB_STATS_FILE_NAME));

    g_free(path);
}

static void test_runtime_control_socket_path_uses_owned_leaf(void) {
    char *path = mb_runtime_control_socket_default_path();

    g_assert_nonnull(path);
    g_assert_true(g_str_has_suffix(path,
                                   G_DIR_SEPARATOR_S MB_RUNTIME_DIR_NAME
                                   G_DIR_SEPARATOR_S MB_CONTROL_SOCKET_NAME));

    g_free(path);
}

static void test_runtime_build_path_handles_custom_leaf(void) {
    char *path = mb_runtime_build_path("custom.leaf");

    g_assert_nonnull(path);
    g_assert_true(g_str_has_suffix(path,
                                   G_DIR_SEPARATOR_S MB_RUNTIME_DIR_NAME
                                   G_DIR_SEPARATOR_S "custom.leaf"));

    g_free(path);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-paths/runtime-default-dir", test_runtime_default_dir_uses_project_leaf);
    g_test_add_func("/mb-paths/stats-path", test_runtime_stats_path_uses_owned_leaf);
    g_test_add_func("/mb-paths/control-socket-path", test_runtime_control_socket_path_uses_owned_leaf);
    g_test_add_func("/mb-paths/build-custom-path", test_runtime_build_path_handles_custom_leaf);

    return g_test_run();
}
