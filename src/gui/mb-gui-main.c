#include "mb-gui-window.h"
#include "mb-version.h"

#include <adwaita.h>
#include <glib.h>
#include <stdbool.h>
#include <string.h>

static bool is_version_arg(const char *arg) {
    return g_strcmp0(arg, "-v") == 0 ||
           g_strcmp0(arg, "--version") == 0;
}

static bool is_verbose_version_arg(const char *arg) {
    return g_strcmp0(arg, "-vv") == 0 ||
           g_strcmp0(arg, "--version-verbose") == 0;
}

static void print_version(bool verbose) {
    g_print("midi-ble-rt-gui %s\n", MB_VERSION);
    if (!verbose)
        return;

    g_print("project: midi-ble-rt\n");
    g_print("role: gtk4-gui\n");
    g_print("features: gtk4 libadwaita control-socket\n");
}

static void activate_cb(GApplication *app, gpointer user_data) {
    (void)user_data;
    GtkWindow *window = mb_gui_window_new(ADW_APPLICATION(app));
    gtk_window_present(window);
}

int main(int argc, char **argv) {
    if (argc == 2 && is_version_arg(argv[1])) {
        print_version(false);
        return 0;
    }

    if (argc == 2 && is_verbose_version_arg(argv[1])) {
        print_version(true);
        return 0;
    }

    g_autoptr(AdwApplication) app = adw_application_new("io.github.mwprado.midi-ble-rt-gui",
                                                        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate_cb), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
