#include "mb-gnome-window.h"
#include "mb-ui-facade.h"
#include "mb-daemon-observer.h"
#include "mb-bluez-observer.h"

#include <gio/gio.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    AdwApplicationWindow *window;
    MbUiFacade *facade;
    MbUiSnapshot *snapshot;

    bool refresh_in_flight;
    bool scan_in_flight;
    bool command_in_flight;

    GtkWidget *service_dot;
    GtkWidget *service_label;
    GtkWidget *daemon_switch;
    GtkWidget *daemon_switch_label;
    GtkWidget *daemon_spinner;
    bool daemon_transition_in_flight;
    bool daemon_requested_active;
    bool daemon_functional;
    bool bluez_available;
    bool bluez_powered_known;
    bool bluez_powered;
    bool bluez_discovering;
    guint daemon_transition_source_id;
    MbDaemonObserver *daemon_observer;
    MbBluezObserver *bluez_observer;
    GtkWidget *footer_devices_label;
    GtkWidget *footer_connection_label;
    GtkWidget *last_scan_label;

    GtkWidget *scan_button;
    GtkWidget *sidebar_list;
    GtkWidget *device_box;
    GtkWidget *empty_box;

    GtkWidget *device_icon;
    GtkWidget *device_name;
    GtkWidget *device_kind;
    GtkWidget *device_state;

    GtkWidget *connect_button;
    GtkWidget *disconnect_button;
    GtkWidget *refresh_button;
    GtkWidget *forget_button;

    GtkWidget *config_card;
    GtkWidget *details_card;

    GtkWidget *auto_reconnect_switch;
    GtkWidget *scan_on_open_switch;
    GtkWidget *clock_filter_switch;
    GtkWidget *profile_combo;
    GtkWidget *timeout_combo;

    char *selected_id;

    /*
     * Daemon D-Bus runtime notifications.
     *
     * No periodic polling: the GUI performs one initial ListDevices() read and
     * then updates from daemon-emitted signals.
     */
    GDBusConnection *daemon_dbus_bus;
    guint daemon_dbus_device_changed_sub_id;
    guint daemon_dbus_state_changed_sub_id;
    guint daemon_dbus_device_removed_sub_id;
} MbGnomeWindowState;
static void show_scan_pair_dialog(MbGnomeWindowState *state);
static void daemon_switch_notify_active_cb(GObject *object,
                                           GParamSpec *pspec,
                                           gpointer user_data);
static bool start_daemon_from_gui(MbGnomeWindowState *state, GError **error);
static bool stop_daemon_from_gui(MbGnomeWindowState *state, GError **error);
static void update_daemon_switch_state(MbGnomeWindowState *state);
static void show_error_dialog(MbGnomeWindowState *state,
                              const char *title,
                              const char *message);
static void forget_clicked_cb(GtkButton *button, gpointer user_data);
static void forget_clicked_cb(GtkButton *button, gpointer user_data);
static void daemon_root_observer_apply(MbGnomeWindowState *state);
static void daemon_root_observer_set_functional(MbGnomeWindowState *state,
                                                bool daemon_functional);

typedef struct {
    MbGnomeWindowState *state;
    GtkWidget *window;
    GtkWidget *list;
    GtkWidget *status_label;
    MbUiSnapshot *snapshot;
    char *selected_device_id;
    bool busy;
} ScanPairDialog;

static void scan_pair_dialog_free(ScanPairDialog *dialog) {
    if (!dialog)
        return;

    if (dialog->snapshot)
        mb_ui_snapshot_free(dialog->snapshot);

    g_clear_pointer(&dialog->selected_device_id, g_free);
    g_free(dialog);
}
typedef struct {
    MbGnomeWindowState *state;
    char *verb;
    char *device_id;
} DeviceCommandTask;

typedef struct {
    MbGnomeWindowState *state;
    bool requested_active;
} DaemonCommandTask;

typedef struct {
    MbGnomeWindowState *state;
    unsigned timeout_seconds;
} ScanTask;

static void mb_gnome_window_refresh(MbGnomeWindowState *state);
static void update_scan_button_state(MbGnomeWindowState *state);
static void update_action_sensitivity(MbGnomeWindowState *state, const MbUiDevice *device);
static void update_main_panel(MbGnomeWindowState *state);
static void bluez_observer_state_changed_cb(bool available,
                                           bool powered_known,
                                           bool powered,
                                           bool discovering,
                                           const char *adapter_path,
                                           void *user_data);
static void daemon_dbus_observer_start(MbGnomeWindowState *state);

static const char *safe(const char *s, const char *fallback) {
    return s && *s ? s : fallback;
}

static bool state_is_streaming(const char *state) {
    return g_ascii_strcasecmp(safe(state, ""), "STREAMING") == 0;
}

static bool state_is_error(const char *state) {
    return g_ascii_strcasecmp(safe(state, ""), "ERROR") == 0;
}


static GtkWidget *label_new(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    if (css_class)
        gtk_widget_add_css_class(label, css_class);
    return label;
}

static void set_label(GtkWidget *label, const char *text) {
    if (GTK_IS_LABEL(label))
        gtk_label_set_text(GTK_LABEL(label), text ? text : "");
}


static void button_set_icon_text(GtkWidget *button,
                                 const char *icon_name,
                                 const char *text) {
    if (!GTK_IS_BUTTON(button))
        return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name ? icon_name : "insert-link-symbolic");
    GtkWidget *label = gtk_label_new(text ? text : "");

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(button), box);
}

static bool device_is_unpaired_for_gui(const MbUiDevice *device) {
    if (!device)
        return false;

    /*
     * Important:
     *
     * In the main imported-instruments snapshot, paired=false may only mean
     * "BlueZ pairing state was not overlaid yet".  It is not proof that the
     * device is unpaired.
     *
     * Only the explicit daemon/UI state UNPAIRED should force the user-facing
     * "Parear novamente" status in the main window.
     */
    return g_ascii_strcasecmp(safe(device->state, ""), "UNPAIRED") == 0;
}

static const char *main_device_status_text(const MbUiDevice *device) {
    if (!device)
        return "Desconhecido";

    if (!device->imported)
        return "Ausente na lista";

    if (device_is_unpaired_for_gui(device))
        return "Parear novamente";

    if (state_is_streaming(device->state))
        return "Conectado";

    return "Funcional";
}


static GtkWidget *icon_button_new(const char *icon_name, const char *text) {
    GtkWidget *button = gtk_button_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *label = gtk_label_new(text);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(button), box);

    return button;
}


static void draw_musical_keyboard_icon_cb(GtkDrawingArea *area,
                                          cairo_t *cr,
                                          int width,
                                          int height,
                                          gpointer user_data) {
    (void)area;
    (void)user_data;

    double w = width;
    double h = height;

    cairo_save(cr);

    /* Corpo do mini teclado/synth */
    double body_x = w * 0.18;
    double body_y = h * 0.28;
    double body_w = w * 0.64;
    double body_h = h * 0.44;
    double radius = w * 0.06;

    cairo_new_path(cr);
    cairo_arc(cr, body_x + radius, body_y + radius, radius, G_PI, 1.5 * G_PI);
    cairo_arc(cr, body_x + body_w - radius, body_y + radius, radius, 1.5 * G_PI, 0);
    cairo_arc(cr, body_x + body_w - radius, body_y + body_h - radius, radius, 0, 0.5 * G_PI);
    cairo_arc(cr, body_x + radius, body_y + body_h - radius, radius, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);

    cairo_set_source_rgb(cr, 0.12, 0.32, 0.64);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.02, 0.08, 0.18, 0.35);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    /* Painel superior */
    cairo_set_source_rgb(cr, 0.82, 0.90, 1.0);
    cairo_rectangle(cr, body_x + body_w * 0.16, body_y + body_h * 0.16, body_w * 0.36, body_h * 0.10);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.82, 0.90, 1.0);
    cairo_arc(cr, body_x + body_w * 0.08, body_y + body_h * 0.21, body_w * 0.025, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_arc(cr, body_x + body_w * 0.70, body_y + body_h * 0.21, body_w * 0.025, 0, 2 * G_PI);
    cairo_fill(cr);

    /* Teclas brancas */
    double key_x = body_x + body_w * 0.13;
    double key_y = body_y + body_h * 0.42;
    double key_w = body_w * 0.74 / 7.0;
    double key_h = body_h * 0.42;

    for (int i = 0; i < 7; i++) {
        cairo_rectangle(cr, key_x + i * key_w, key_y, key_w - 1.0, key_h);
        cairo_set_source_rgb(cr, 0.96, 0.97, 0.98);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.02, 0.02, 0.02, 0.22);
        cairo_set_line_width(cr, 0.7);
        cairo_stroke(cr);
    }

    /* Teclas pretas: padrão 2 + 3 */
    int black_positions[] = {0, 1, 3, 4, 5};
    for (guint i = 0; i < G_N_ELEMENTS(black_positions); i++) {
        int pos = black_positions[i];
        double bx = key_x + (pos + 0.68) * key_w;
        double by = key_y;
        double bw = key_w * 0.48;
        double bh = key_h * 0.58;

        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_set_source_rgb(cr, 0.03, 0.04, 0.06);
        cairo_fill(cr);
    }

    cairo_restore(cr);
}

static GtkWidget *musical_keyboard_icon_new(const char *css_class, int size) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "circle-icon");
    if (css_class)
        gtk_widget_add_css_class(box, css_class);

    GtkWidget *drawing = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing, size, size);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing),
                                   draw_musical_keyboard_icon_cb,
                                   NULL,
                                   NULL);
    gtk_widget_set_halign(drawing, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(drawing, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), drawing);
    return box;
}


static GtkWidget *config_row_new(const char *icon_name,
                                 const char *title,
                                 const char *subtitle,
                                 GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(row, "config-row");

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_add_css_class(icon, "config-icon");
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), icon);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget *title_label = label_new(title, "config-title");
    GtkWidget *subtitle_label = label_new(subtitle, "config-subtitle");
    gtk_label_set_wrap(GTK_LABEL(subtitle_label), TRUE);

    gtk_box_append(GTK_BOX(text), title_label);
    gtk_box_append(GTK_BOX(text), subtitle_label);
    gtk_box_append(GTK_BOX(row), text);

    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), control);
    }

    return row;
}

static GtkWidget *status_footer_item(GtkWidget **dot_out, GtkWidget **label_out, const char *label_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "footer-item");

    GtkWidget *dot = gtk_label_new("●");
    gtk_widget_add_css_class(dot, "ok-text");

    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_add_css_class(label, "footer-text");

    gtk_box_append(GTK_BOX(box), dot);
    gtk_box_append(GTK_BOX(box), label);

    if (dot_out)
        *dot_out = dot;
    if (label_out)
        *label_out = label;

    return box;
}

static const MbUiDevice *selected_device(MbGnomeWindowState *state) {
    if (!state || !state->snapshot || !state->selected_id)
        return NULL;

    return mb_ui_snapshot_find_device(state->snapshot, state->selected_id);
}


static bool start_daemon_from_gui(MbGnomeWindowState *state, GError **error) {
    (void)state;

    g_printerr("[midi-ble-rt-gui] service start: systemctl --user start midi-ble-rtd.service\n");

    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         error,
                                         "systemctl",
                                         "--user",
                                         "start",
                                         "midi-ble-rtd.service",
                                         NULL);

    if (!proc)
        return false;

    bool ok = g_subprocess_wait_check(proc, NULL, error);
    g_object_unref(proc);
    return ok;
}


static bool stop_daemon_from_gui(MbGnomeWindowState *state, GError **error) {
    (void)state;

    g_printerr("[midi-ble-rt-gui] service stop: systemctl --user stop midi-ble-rtd.service\n");

    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         error,
                                         "systemctl",
                                         "--user",
                                         "stop",
                                         "midi-ble-rtd.service",
                                         NULL);

    if (!proc)
        return false;

    bool ok = g_subprocess_wait_check(proc, NULL, error);
    g_object_unref(proc);
    return ok;
}


static void update_daemon_switch_state(MbGnomeWindowState *state) {
    if (!state || !state->daemon_switch)
        return;

    bool visual_active = state->daemon_transition_in_flight
                             ? state->daemon_requested_active
                             : state->daemon_functional;

    g_signal_handlers_block_by_func(state->daemon_switch,
                                    G_CALLBACK(daemon_switch_notify_active_cb),
                                    state);

    gtk_switch_set_active(GTK_SWITCH(state->daemon_switch), visual_active);
    gtk_widget_set_sensitive(state->daemon_switch, !state->daemon_transition_in_flight);

    g_signal_handlers_unblock_by_func(state->daemon_switch,
                                      G_CALLBACK(daemon_switch_notify_active_cb),
                                      state);

    if (state->daemon_spinner) {
        gtk_widget_set_visible(state->daemon_spinner, state->daemon_transition_in_flight);

        if (state->daemon_transition_in_flight)
            gtk_spinner_start(GTK_SPINNER(state->daemon_spinner));
        else
            gtk_spinner_stop(GTK_SPINNER(state->daemon_spinner));
    }

    if (state->daemon_switch_label) {
        if (state->daemon_transition_in_flight) {
            set_label(state->daemon_switch_label,
                      state->daemon_requested_active
                          ? "Iniciando Serviço MIDI-BLE…"
                          : "Desligando Serviço MIDI-BLE…");
        } else {
            set_label(state->daemon_switch_label,
                      state->daemon_functional
                          ? "Serviço MIDI-BLE ativado"
                          : "Serviço MIDI-BLE desativado");
        }
    }
}


static void daemon_command_task_thread(GTask *task,
                                       gpointer source_object,
                                       gpointer task_data,
                                       GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    DaemonCommandTask *cmd = task_data;
    GError *error = NULL;
    bool ok = false;

    if (cmd->requested_active)
        ok = start_daemon_from_gui(cmd->state, &error);
    else
        ok = stop_daemon_from_gui(cmd->state, &error);

    if (!ok)
        g_task_return_error(task,
                            error ? error
                                  : g_error_new(G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                "falha ao alterar estado do daemon"));
    else
        g_task_return_boolean(task, TRUE);
}

static void daemon_command_task_done(GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data) {
    (void)source_object;

    MbGnomeWindowState *state = user_data;
    GError *error = NULL;

    if (!state)
        return;

    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        state->daemon_transition_in_flight = false;
        state->daemon_requested_active = state->daemon_functional;
        update_daemon_switch_state(state);

        show_error_dialog(state,
                          "Falha no Serviço MIDI-BLE",
                          error && error->message ? error->message : "Erro desconhecido");
        g_clear_error(&error);
        return;
    }

    if (state->daemon_observer) {
        GError *observer_error = NULL;

        if (!mb_daemon_observer_refresh(state->daemon_observer, &observer_error)) {
            g_printerr("[midi-ble-rt-gui] daemon observer refresh failed: %s\n",
                       observer_error && observer_error->message ? observer_error->message : "unknown error");
            g_clear_error(&observer_error);
        }
    }

    state->daemon_transition_in_flight = false;
    update_daemon_switch_state(state);
    mb_gnome_window_refresh(state);
}

static void daemon_switch_notify_active_cb(GObject *object,
                                           GParamSpec *pspec,
                                           gpointer user_data) {
    (void)pspec;

    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    if (state->daemon_transition_in_flight)
        return;

    bool requested_active = gtk_switch_get_active(GTK_SWITCH(object));
    if (requested_active == state->daemon_functional)
        return;

    state->daemon_transition_in_flight = true;
    state->daemon_requested_active = requested_active;
    update_daemon_switch_state(state);

    DaemonCommandTask *cmd = g_new0(DaemonCommandTask, 1);
    cmd->state = state;
    cmd->requested_active = requested_active;

    GTask *task = g_task_new(G_OBJECT(state->window),
                             NULL,
                             daemon_command_task_done,
                             state);
    g_task_set_task_data(task, cmd, g_free);
    g_task_run_in_thread(task, daemon_command_task_thread);
    g_object_unref(task);
}



static bool daemon_effective_functional(MbGnomeWindowState *state,
                                        bool systemd_active) {
    bool dbus_online = state &&
                       state->snapshot &&
                       state->snapshot->status.online;

    return dbus_online || systemd_active;
}

static void daemon_root_observer_apply(MbGnomeWindowState *state) {
    if (!state)
        return;

    /*
     * Root observer rule:
     *
     * systemd only tells whether the daemon process is available.
     * Device/catalog/runtime state comes from the daemon D-Bus API.
     */
    update_scan_button_state(state);

    if (state->sidebar_list)
        gtk_widget_set_sensitive(state->sidebar_list, TRUE);

    update_daemon_switch_state(state);

    /*
     * Daemon state changes must immediately update runtime actions.
     * The catalog remains visible, but Connect/Disconnect depend on daemon.
     */
    update_main_panel(state);
}

static void daemon_root_observer_set_functional(MbGnomeWindowState *state,
                                                bool daemon_functional) {
    if (!state)
        return;

    bool changed = state->daemon_functional != daemon_functional;
    state->daemon_functional = daemon_functional;

    /*
     * Initial-state signaling rule:
     * even if the value did not change, propagate it because widgets may have
     * been created before the first daemon-status result.
     */
    g_printerr("[midi-ble-rt-gui] daemon observer signal: functional=%d changed=%d\n",
               state->daemon_functional,
               changed);

    daemon_root_observer_apply(state);
}


static void daemon_observer_state_changed_cb(bool active,
                                             const char *active_state,
                                             const char *sub_state,
                                             void *user_data) {
    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    g_printerr("[midi-ble-rt-gui] daemon observer signal: active=%d active_state=%s sub_state=%s\n",
               active,
               safe(active_state, "unknown"),
               safe(sub_state, "unknown"));

    bool functional = daemon_effective_functional(state, active);

    daemon_root_observer_set_functional(state, functional);
    update_daemon_switch_state(state);

    if (functional && !state->daemon_transition_in_flight)
        mb_gnome_window_refresh(state);
}


static bool bluez_can_scan_for_gui(const MbGnomeWindowState *state) {
    /*
     * The historical name remains for now, but the policy no longer comes from
     * BlueZ.  Scan availability is daemon-owned: the GUI may scan only when the
     * midi-ble-rtd D-Bus API is expected to be available.
     */
    return state && state->daemon_functional;
}

static void update_scan_button_state(MbGnomeWindowState *state) {
    if (!state || !state->scan_button)
        return;

    const char *label = "Adicionar instrumento";

    if (state->scan_in_flight)
        label = "Procurando instrumentos…";
    else if (!state->daemon_functional)
        label = "Serviço MIDI-BLE inativo";

    button_set_icon_text(state->scan_button, "edit-find-symbolic", label);

    gtk_widget_set_sensitive(state->scan_button,
                             bluez_can_scan_for_gui(state) &&
                             !state->scan_in_flight &&
                             !state->command_in_flight);
}

static void bluez_observer_state_changed_cb(bool available,
                                           bool powered_known,
                                           bool powered,
                                           bool discovering,
                                           const char *adapter_path,
                                           void *user_data) {
    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    state->bluez_available = available;
    state->bluez_powered_known = powered_known;
    state->bluez_powered = powered;
    state->bluez_discovering = discovering;

    g_printerr("[midi-ble-rt-gui] BlueZ observer: available=%d powered_known=%d powered=%d discovering=%d adapter=%s\n",
               available,
               powered_known,
               powered,
               discovering,
               adapter_path && *adapter_path ? adapter_path : "-");

    update_scan_button_state(state);
}


static void daemon_dbus_signal_cb(GDBusConnection *connection,
                                  const char *sender_name,
                                  const char *object_path,
                                  const char *interface_name,
                                  const char *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;

    MbGnomeWindowState *state = user_data;
    if (!state)
        return;

    g_printerr("[midi-ble-rt-gui] daemon D-Bus signal: %s\n",
               signal_name && *signal_name ? signal_name : "-");

    if (g_strcmp0(signal_name, "DeviceRemoved") == 0 && parameters) {
        const char *removed_id = NULL;
        g_variant_get(parameters, "(&s)", &removed_id);

        if (removed_id &&
            state->selected_id &&
            g_strcmp0(state->selected_id, removed_id) == 0) {
            g_clear_pointer(&state->selected_id, g_free);
            update_action_sensitivity(state, NULL);
        }
    }

    /*
     * Event-driven update:
     * The daemon is the state authority.  On every device signal we perform a
     * single fresh ListDevices() read.  There is no periodic refresh loop.
     */
    mb_gnome_window_refresh(state);
}

static void daemon_dbus_observer_start(MbGnomeWindowState *state) {
    if (!state || state->daemon_dbus_bus)
        return;

    GError *error = NULL;
    state->daemon_dbus_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!state->daemon_dbus_bus) {
        g_printerr("[midi-ble-rt-gui] daemon D-Bus observer unavailable: %s\n",
                   error && error->message ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }

    state->daemon_dbus_device_changed_sub_id =
        g_dbus_connection_signal_subscribe(state->daemon_dbus_bus,
                                           "org.midi_ble_rt.Daemon",
                                           "org.midi_ble_rt.Daemon1",
                                           "DeviceChanged",
                                           "/org/midi_ble_rt/Daemon",
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           daemon_dbus_signal_cb,
                                           state,
                                           NULL);

    state->daemon_dbus_state_changed_sub_id =
        g_dbus_connection_signal_subscribe(state->daemon_dbus_bus,
                                           "org.midi_ble_rt.Daemon",
                                           "org.midi_ble_rt.Daemon1",
                                           "DeviceStateChanged",
                                           "/org/midi_ble_rt/Daemon",
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           daemon_dbus_signal_cb,
                                           state,
                                           NULL);

    state->daemon_dbus_device_removed_sub_id =
        g_dbus_connection_signal_subscribe(state->daemon_dbus_bus,
                                           "org.midi_ble_rt.Daemon",
                                           "org.midi_ble_rt.Daemon1",
                                           "DeviceRemoved",
                                           "/org/midi_ble_rt/Daemon",
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           daemon_dbus_signal_cb,
                                           state,
                                           NULL);

    g_printerr("[midi-ble-rt-gui] daemon D-Bus observer started: DeviceChanged=%u DeviceStateChanged=%u\n",
               state->daemon_dbus_device_changed_sub_id,
               state->daemon_dbus_state_changed_sub_id);
}


static void update_action_sensitivity(MbGnomeWindowState *state, const MbUiDevice *device) {
    if (!state)
        return;

    bool has_selection = device != NULL;
    bool busy = state->command_in_flight;
    bool streaming = has_selection && state_is_streaming(device->state);

    /*
     * Observer rule:
     *
     * - Main list availability comes from the persisted catalog.
     * - Basic panel and local actions come from list selection.
     * - Runtime connect/disconnect additionally requires daemon_functional.
     */
    if (state->connect_button) {
        button_set_icon_text(state->connect_button,
                             streaming ? "edit-clear-symbolic" : "insert-link-symbolic",
                             streaming ? "Desconectar" : "Conectar");

        gtk_widget_set_sensitive(state->connect_button,
                                 state->daemon_functional &&
                                 has_selection &&
                                 !busy);
    }

    if (state->disconnect_button)
        gtk_widget_set_visible(state->disconnect_button, FALSE);

    if (state->refresh_button)
        gtk_widget_set_sensitive(state->refresh_button, FALSE);

    if (state->forget_button)
        gtk_widget_set_sensitive(state->forget_button,
                                 has_selection &&
                                 !busy);

    update_scan_button_state(state);
}


static bool device_is_visible_in_main_list(const MbUiDevice *device);
static void update_main_panel(MbGnomeWindowState *state) {
    bool online = state->daemon_functional;
    const MbUiDevice *device = selected_device(state);

    /*
     * Panels and action buttons are only meaningful for the selected row.
     * rebuild_sidebar() guarantees a default selection when imported devices
     * exist.  If there is no selected row, keep the main panel inactive.
     */
    if (!device_is_visible_in_main_list(device))
        device = NULL;

    set_label(state->service_label, online ? "Serviço MIDI-BLE ativo" : "Serviço MIDI-BLE inativo");
    gtk_widget_remove_css_class(state->service_dot, "ok-text");
    gtk_widget_remove_css_class(state->service_dot, "err-text");
    gtk_widget_add_css_class(state->service_dot, online ? "ok-text" : "err-text");

    guint count = state->snapshot && state->snapshot->devices ? state->snapshot->devices->len : 0;

    update_scan_button_state(state);

    char devices_text[96];
    g_snprintf(devices_text,
               sizeof(devices_text),
               "%u instrumento%s conhecido%s",
               count,
               count == 1 ? "" : "s",
               count == 1 ? "" : "s");
    set_label(state->footer_devices_label, devices_text);

    if (!device) {
        gtk_widget_set_visible(state->empty_box, TRUE);
        gtk_widget_set_visible(state->device_box, FALSE);
        set_label(state->footer_connection_label, "Nenhum conectado");

        if (state->config_card)
            gtk_widget_set_sensitive(state->config_card, FALSE);

        if (state->details_card)
            gtk_widget_set_sensitive(state->details_card, FALSE);

        update_action_sensitivity(state, NULL);
        update_daemon_switch_state(state);
        return;
    }

    g_free(state->selected_id);
    state->selected_id = g_strdup(device->id);

    gtk_widget_set_visible(state->empty_box, FALSE);
    gtk_widget_set_visible(state->device_box, TRUE);

    set_label(state->device_name, safe(device->name, "Dispositivo BLE-MIDI"));
    set_label(state->device_kind, "Instrumento BLE-MIDI importado");
    set_label(state->device_state, main_device_status_text(device));

    gtk_widget_remove_css_class(state->device_state, "ok-text");
    gtk_widget_remove_css_class(state->device_state, "err-text");
    gtk_widget_remove_css_class(state->device_state, "muted-text");
    gtk_widget_add_css_class(state->device_state,
                             device_is_unpaired_for_gui(device) || state_is_error(device->state)
                                 ? "err-text"
                                 : "ok-text");

    set_label(state->footer_connection_label, state_is_streaming(device->state) ? "Conectado" : "Nenhum conectado");

    bool has_selection = device != NULL;

    if (state->config_card)
        gtk_widget_set_sensitive(state->config_card, has_selection);

    if (state->details_card)
        gtk_widget_set_sensitive(state->details_card, has_selection);

    update_action_sensitivity(state, device);
    update_daemon_switch_state(state);
    update_action_sensitivity(state, selected_device(state));
}


static bool device_is_visible_in_main_list(const MbUiDevice *device) {
    return device != NULL;
}


static void device_row_delete_clicked_cb(GtkButton *button, gpointer user_data) {
    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    const char *device_id = g_object_get_data(G_OBJECT(button), "device-id");
    if (!device_id || !*device_id)
        return;

    /*
     * The row button owns the row identity.  Select that catalog item and
     * reuse the existing confirmation flow.
     */
    g_free(state->selected_id);
    state->selected_id = g_strdup(device_id);
    update_main_panel(state);

    forget_clicked_cb(button, state);
}

static GtkWidget *device_row_new(MbGnomeWindowState *state, const MbUiDevice *device) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(row, "device-row");

    GtkWidget *circle = musical_keyboard_icon_new("small-circle", 42);
    gtk_box_append(GTK_BOX(row), circle);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget *name = label_new(safe(device->name, "Dispositivo BLE-MIDI"), "device-row-name");

    GtkWidget *status_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    GtkWidget *dot = gtk_label_new("●");
    gtk_widget_add_css_class(dot,
                             device_is_unpaired_for_gui(device) || state_is_error(device->state)
                                 ? "err-text"
                                 : "ok-text");
    GtkWidget *state_label = gtk_label_new(main_device_status_text(device));
    gtk_widget_add_css_class(state_label, "device-row-status");
    gtk_box_append(GTK_BOX(status_line), dot);
    gtk_box_append(GTK_BOX(status_line), state_label);

    gtk_box_append(GTK_BOX(text), name);
    gtk_box_append(GTK_BOX(text), status_line);

    GtkWidget *delete_button = gtk_button_new_from_icon_name("edit-delete-symbolic");
    gtk_widget_add_css_class(delete_button, "flat");
    gtk_widget_add_css_class(delete_button, "circular");
    gtk_widget_add_css_class(delete_button, "destructive-action");
    gtk_widget_set_tooltip_text(delete_button, "Excluir ou esquecer instrumento");
    gtk_widget_set_valign(delete_button, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(delete_button, state && !state->command_in_flight);

    if (device && device->id)
        g_object_set_data_full(G_OBJECT(delete_button),
                               "device-id",
                               g_strdup(device->id),
                               g_free);

    g_signal_connect(delete_button,
                     "clicked",
                     G_CALLBACK(device_row_delete_clicked_cb),
                     state);

    gtk_box_append(GTK_BOX(row), text);
    gtk_box_append(GTK_BOX(row), delete_button);
    return row;
}

static void rebuild_sidebar(MbGnomeWindowState *state) {
    g_printerr("[midi-ble-rt-gui] sidebar: rebuild devices=%u selected=%s daemon=%d\n",
               state && state->snapshot && state->snapshot->devices ? state->snapshot->devices->len : 0,
               state && state->selected_id ? state->selected_id : "-",
               state ? state->daemon_functional : 0);

    GtkWidget *child = gtk_widget_get_first_child(state->sidebar_list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(state->sidebar_list), child);
        child = next;
    }

    if (!state->snapshot || !state->snapshot->devices) {
        update_main_panel(state);
        return;
    }

    GtkListBoxRow *first_visible_row = NULL;
    GtkListBoxRow *selected_row = NULL;

    for (guint i = 0; i < state->snapshot->devices->len; i++) {
        const MbUiDevice *device = g_ptr_array_index(state->snapshot->devices, i);

        if (!device_is_visible_in_main_list(device))
            continue;

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), device_row_new(state, device));
        g_object_set_data_full(G_OBJECT(row), "device-id", g_strdup(device->id), g_free);
        gtk_list_box_append(GTK_LIST_BOX(state->sidebar_list), row);

        if (!first_visible_row)
            first_visible_row = GTK_LIST_BOX_ROW(row);

        if (state->selected_id && g_strcmp0(state->selected_id, device->id) == 0)
            selected_row = GTK_LIST_BOX_ROW(row);
    }

    /*
     * Selection contract:
     * if the main list has at least one imported instrument, exactly one
     * instrument should be selected by default.  Buttons and panels always
     * refer to the selected instrument only.
     */
    if (!selected_row && first_visible_row) {
        const char *first_id = g_object_get_data(G_OBJECT(first_visible_row), "device-id");
        g_free(state->selected_id);
        state->selected_id = g_strdup(first_id);
        selected_row = first_visible_row;
    }

    if (selected_row)
        gtk_list_box_select_row(GTK_LIST_BOX(state->sidebar_list), selected_row);

    update_main_panel(state);
}

static void show_error_dialog(MbGnomeWindowState *state, const char *title, const char *message) {
    const char *safe_title = title ? title : "Erro";
    const char *safe_message = message ? message : "Erro desconhecido";

    /*
     * Diagnostic policy:
     * every error shown to the musician must also be emitted on stderr.
     * This keeps GUI testing and field diagnosis simple when launched
     * from a terminal.
     */
    g_printerr("[midi-ble-rt-gui] ERROR: %s: %s\n",
               safe_title,
               safe_message);

    AdwDialog *dialog = adw_alert_dialog_new(safe_title, safe_message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Fechar");
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_dialog_present(dialog, GTK_WIDGET(state->window));
}

static void show_diagnostics_dialog(MbGnomeWindowState *state) {
    const MbUiDevice *device = selected_device(state);
    MbUiSnapshot *snapshot = state->snapshot;

    char *message = g_strdup_printf(
        "Serviço MIDI-BLE: %s\n"
        "Instrumentos importados configurados: %u\n"
        "Instrumentos importados conectados: %u\n"
        "Fila de ciclo de vida: %u\n"
        "\n"
        "Dispositivo\n"
        "Nome: %s\n"
        "ID: %s\n"
        "Endereço: %s\n"
        "Estado técnico: %s\n"
        "Porta ALSA: %d\n"
        "\n"
        "Tempo real\n"
        "RTKit: %s\n"
        "RTKit RX: %s\n"
        "RTKit TX: %s\n"
        "Prioridade: %u\n"
        "RX workers: %u\n"
        "TX workers: %u\n",
        snapshot && snapshot->status.online ? "ativo" : "inativo",
        snapshot ? snapshot->status.devices : 0,
        snapshot ? snapshot->status.streaming : 0,
        snapshot ? snapshot->status.lifecycle_queue : 0,
        device ? safe(device->name, "-") : "-",
        device ? safe(device->id, "-") : "-",
        device ? safe(device->address, "-") : "-",
        device ? safe(device->state, "-") : "-",
        device ? device->alsa_port : -1,
        snapshot ? safe(snapshot->status.rtkit, "-") : "-",
        snapshot ? safe(snapshot->status.rtkit_rx, "-") : "-",
        snapshot ? safe(snapshot->status.rtkit_tx, "-") : "-",
        snapshot ? snapshot->status.rtkit_priority : 0,
        snapshot ? snapshot->status.rx_workers : 0,
        snapshot ? snapshot->status.tx_workers : 0);

    AdwDialog *dialog = adw_alert_dialog_new("Detalhes técnicos", message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Fechar");
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_dialog_present(dialog, GTK_WIDGET(state->window));
    g_free(message);
}

static void refresh_task_thread(GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    MbGnomeWindowState *state = task_data;
    MbUiSnapshot *snapshot = mb_ui_facade_get_snapshot(state->facade);
    g_task_return_pointer(task, snapshot, (GDestroyNotify)mb_ui_snapshot_free);
}

static void refresh_task_done(GObject *source_object,
                              GAsyncResult *result,
                              gpointer user_data) {
    (void)source_object;

    MbGnomeWindowState *state = user_data;
    GError *error = NULL;
    MbUiSnapshot *snapshot = g_task_propagate_pointer(G_TASK(result), &error);

    state->refresh_in_flight = false;

    if (error) {
        if (state->snapshot)
            mb_ui_snapshot_free(state->snapshot);

        state->snapshot = mb_ui_snapshot_new();
        state->snapshot->status.online = false;
        state->snapshot->last_error = g_strdup(error->message);
        g_clear_error(&error);
    } else if (snapshot) {
        if (state->snapshot)
            mb_ui_snapshot_free(state->snapshot);

        state->snapshot = snapshot;
    }

    bool daemon_online = state->snapshot && state->snapshot->status.online;
    bool previous_functional = state->daemon_functional;

    if (!state->daemon_transition_in_flight)
        state->daemon_functional = daemon_online;

    g_printerr("[midi-ble-rt-gui] daemon snapshot: previous_functional=%d daemon_online=%d effective=%d\n",
               previous_functional,
               daemon_online,
               state->daemon_functional);

    set_label(state->last_scan_label, "Catálogo atualizado agora");
    rebuild_sidebar(state);
}


static void mb_gnome_window_refresh(MbGnomeWindowState *state) {
    if (!state || state->refresh_in_flight)
        return;

    state->refresh_in_flight = true;

    GTask *task = g_task_new(G_OBJECT(state->window), NULL, refresh_task_done, state);
    g_task_set_task_data(task, state, NULL);
    g_task_run_in_thread(task, refresh_task_thread);
    g_object_unref(task);
}


static bool device_is_scan_only_for_dialog(const MbUiDevice *device) {
    /*
     * The scan dialog receives a scan-specific snapshot.
     * Everything visible there is eligible to be shown for pairing.
     */
    return device != NULL;
}


static void scan_pair_dialog_set_status(ScanPairDialog *dialog,
                                        const char *text,
                                        bool error) {
    if (!dialog || !dialog->status_label)
        return;

    gtk_widget_remove_css_class(dialog->status_label, "err-text");
    gtk_widget_remove_css_class(dialog->status_label, "muted-text");
    gtk_widget_remove_css_class(dialog->status_label, "ok-text");

    gtk_widget_add_css_class(dialog->status_label, error ? "err-text" : "muted-text");

    /*
     * Keep this label short. Long backend/BlueZ errors belong in stderr/logs,
     * otherwise the action dialog changes size and looks unstable.
     */
    gtk_label_set_ellipsize(GTK_LABEL(dialog->status_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(dialog->status_label), 48);
    gtk_label_set_single_line_mode(GTK_LABEL(dialog->status_label), TRUE);

    set_label(dialog->status_label, text);
}


static void scan_pair_dialog_set_busy(ScanPairDialog *dialog, bool busy) {
    if (!dialog)
        return;

    dialog->busy = busy;

    if (dialog->list)
        gtk_widget_set_sensitive(dialog->list, !busy);
}


static void scan_pair_row_selected(GtkListBox *box,
                                   GtkListBoxRow *row,
                                   gpointer user_data) {
    (void)box;

    ScanPairDialog *dialog = user_data;

    if (!dialog)
        return;

    g_clear_pointer(&dialog->selected_device_id, g_free);

    if (row) {
        const char *device_id = g_object_get_data(G_OBJECT(row), "device-id");
        if (device_id)
            dialog->selected_device_id = g_strdup(device_id);
    }

    scan_pair_dialog_set_busy(dialog, dialog->busy);
}

static void scan_pair_dialog_row_pair_import_clicked(GtkButton *button,
                                                     gpointer user_data);

static GtkWidget *scan_pair_device_row_new(ScanPairDialog *dialog,
                                             const MbUiDevice *device) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(row, "device-row");

    GtkWidget *circle = musical_keyboard_icon_new("small-circle", 42);
    gtk_box_append(GTK_BOX(row), circle);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(text, TRUE);

    /*
     * Pairing/import dialog must stay musician-facing and minimal.
     * Show only:
     *   - visible instrument id/name
     *   - Bluetooth address
     *
     * Pairing/imported state is expressed by the single action button.
     * Technical details belong to the main details panel.
     */
    const char *title = safe(device && device->name ? device->name : NULL,
                             device && device->id ? device->id : "Dispositivo BLE-MIDI");
    const char *address = safe(device && device->address ? device->address : NULL, "Endereço não informado");

    GtkWidget *name = label_new(title, "device-row-name");
    GtkWidget *addr = label_new(address, "device-row-status");

    gtk_box_append(GTK_BOX(text), name);
    gtk_box_append(GTK_BOX(text), addr);
    gtk_box_append(GTK_BOX(row), text);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);

    GtkWidget *pair_import_button = gtk_button_new_with_label("Parear e Importar");
    gtk_widget_add_css_class(pair_import_button, "suggested-action");

    bool imported = device && device->imported;
    bool paired = device && device->paired;
    bool ready = imported && paired;

    gtk_widget_set_sensitive(pair_import_button, device && !ready);

    if (ready)
        gtk_button_set_label(GTK_BUTTON(pair_import_button), "Já pronto");
    else if (imported && !paired)
        gtk_button_set_label(GTK_BUTTON(pair_import_button), "Parear");
    else if (!imported && paired)
        gtk_button_set_label(GTK_BUTTON(pair_import_button), "Importar");
    else
        gtk_button_set_label(GTK_BUTTON(pair_import_button), "Parear e Importar");

    if (device && device->id) {
        g_object_set_data_full(G_OBJECT(pair_import_button), "device-id", g_strdup(device->id), g_free);
        g_object_set_data_full(G_OBJECT(row), "device-id", g_strdup(device->id), g_free);
    }

    g_signal_connect(pair_import_button,
                     "clicked",
                     G_CALLBACK(scan_pair_dialog_row_pair_import_clicked),
                     dialog);

    gtk_box_append(GTK_BOX(actions), pair_import_button);
    gtk_box_append(GTK_BOX(row), actions);

    return row;
}


static void scan_pair_dialog_rebuild_list(ScanPairDialog *dialog) {
    if (!dialog || !dialog->list)
        return;

    GtkWidget *child = gtk_widget_get_first_child(dialog->list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(dialog->list), child);
        child = next;
    }

    g_clear_pointer(&dialog->selected_device_id, g_free);

    guint count = 0;

    if (dialog->snapshot && dialog->snapshot->devices) {
        for (guint i = 0; i < dialog->snapshot->devices->len; i++) {
            const MbUiDevice *device = g_ptr_array_index(dialog->snapshot->devices, i);

            if (!device_is_scan_only_for_dialog(device))
                continue;

            gtk_list_box_append(GTK_LIST_BOX(dialog->list), scan_pair_device_row_new(dialog, device));
            count++;
        }
    }

    if (count == 0) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = label_new("Nenhum instrumento novo importado.", "muted-text");
        gtk_widget_set_margin_top(label, 12);
        gtk_widget_set_margin_bottom(label, 12);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(dialog->list), row);
    }

    scan_pair_dialog_set_busy(dialog, dialog->busy);
}

typedef struct {
    ScanPairDialog *dialog;
    int timeout_seconds;
    bool refresh_main_after_done;
} ScanPairTask;

static void scan_pair_task_thread(GTask *task,
                                  gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    ScanPairTask *scan = task_data;
    GError *error = NULL;

    if (!mb_ui_facade_scan_devices(scan->dialog->state->facade,
                                   scan->timeout_seconds,
                                   &error)) {
        g_task_return_error(task, error);
        return;
    }

    g_task_return_boolean(task, TRUE);
}

static void scan_pair_task_done(GObject *source,
                                GAsyncResult *result,
                                gpointer user_data) {
    (void)source;

    ScanPairDialog *dialog = user_data;
    ScanPairTask *scan = g_task_get_task_data(G_TASK(result));
    GError *error = NULL;

    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        g_printerr("[midi-ble-rt-gui] ERROR: scan dialog failed: %s\n",
                   error && error->message ? error->message : "Erro desconhecido");

        if (scan && scan->refresh_main_after_done) {
            scan_pair_dialog_set_status(dialog, "Instrumento importado. A porta ALSA foi criada pelo serviço.", false);
            if (dialog->state)
                mb_gnome_window_refresh(dialog->state);
        } else {
            scan_pair_dialog_set_status(dialog, "Não foi possível procurar instrumentos", true);
        }

        g_clear_error(&error);
        scan_pair_dialog_set_busy(dialog, false);
        return;
    }

    if (dialog->snapshot)
        mb_ui_snapshot_free(dialog->snapshot);

    dialog->snapshot = mb_ui_facade_get_scan_snapshot(dialog->state->facade);

    if (dialog->status_label) {
        if (scan && scan->refresh_main_after_done)
            set_label(dialog->status_label, "Instrumento importado. A porta ALSA foi criada pelo serviço.");
        else
            set_label(dialog->status_label, "Escolha um instrumento");
    }

    scan_pair_dialog_set_busy(dialog, false);
    scan_pair_dialog_rebuild_list(dialog);

    if (scan && scan->refresh_main_after_done && dialog->state)
        mb_gnome_window_refresh(dialog->state);
}


static void scan_pair_dialog_start_scan(ScanPairDialog *dialog) {
    if (!dialog || dialog->busy)
        return;

    scan_pair_dialog_set_busy(dialog, true);

    if (dialog->status_label)
        set_label(dialog->status_label, "Procurando instrumentos…");

    ScanPairTask *scan = g_new0(ScanPairTask, 1);
    scan->dialog = dialog;
    scan->timeout_seconds = 10;
    scan->refresh_main_after_done = false;

    GTask *task = g_task_new(G_OBJECT(dialog->window), NULL, scan_pair_task_done, dialog);
    g_task_set_task_data(task, scan, g_free);
    g_task_run_in_thread(task, scan_pair_task_thread);
    g_object_unref(task);
}


typedef struct {
    ScanPairDialog *dialog;
    char *device_id;
} ImportTask;

static void import_task_free(ImportTask *task) {
    if (!task)
        return;

    g_free(task->device_id);
    g_free(task);
}

static void import_task_thread(GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    ImportTask *import = task_data;
    GError *error = NULL;
    bool paired = false;

    if (import && import->dialog && import->dialog->snapshot && import->dialog->snapshot->devices) {
        const MbUiDevice *device = mb_ui_snapshot_find_device(import->dialog->snapshot,
                                                              import->device_id);
        paired = device && device->paired;
    }

    /*
     * One visible button, two internal paths:
     * - already paired: only import/trust/configure if needed
     * - not paired: pair/trust/configure if needed
     */
    bool ok = false;
    if (paired) {
        ok = mb_ui_facade_import_scanned_device(import->dialog->state->facade,
                                                import->device_id,
                                                &error);
    } else {
        ok = mb_ui_facade_pair_scanned_device(import->dialog->state->facade,
                                              import->device_id,
                                              &error);
    }

    if (!ok) {
        g_task_return_error(task, error);
        return;
    }

    g_task_return_boolean(task, TRUE);
}

static void import_task_done(GObject *source,
                             GAsyncResult *result,
                             gpointer user_data) {
    (void)source;

    ScanPairDialog *dialog = user_data;
    GError *error = NULL;

    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        g_printerr("[midi-ble-rt-gui] ERROR: import failed: %s\n",
                   error && error->message ? error->message : "Erro desconhecido");

        scan_pair_dialog_set_status(dialog, "Não foi possível preparar o instrumento. Tente novamente", true);

        g_clear_error(&error);
        scan_pair_dialog_set_busy(dialog, false);
        return;
    }

    g_printerr("[midi-ble-rt-gui] import: device enrolled successfully\n");

    if (dialog->snapshot)
        mb_ui_snapshot_free(dialog->snapshot);

    dialog->snapshot = mb_ui_facade_get_scan_snapshot(dialog->state->facade);

    scan_pair_dialog_set_busy(dialog, false);
    scan_pair_dialog_rebuild_list(dialog);

    scan_pair_dialog_set_status(dialog,
                                "Instrumento importado. A porta ALSA foi criada pelo serviço.",
                                false);

    if (dialog->state)
        mb_gnome_window_refresh(dialog->state);
}

static void scan_pair_dialog_row_pair_import_clicked(GtkButton *button,
                                                     gpointer user_data) {
    ScanPairDialog *dialog = user_data;

    if (!dialog || dialog->busy)
        return;

    const char *device_id = g_object_get_data(G_OBJECT(button), "device-id");
    if (!device_id || !*device_id)
        return;

    scan_pair_dialog_set_busy(dialog, true);

    scan_pair_dialog_set_status(dialog, "Preparando instrumento…", false);

    ImportTask *import = g_new0(ImportTask, 1);
    import->dialog = dialog;
    import->device_id = g_strdup(device_id);

    GTask *task = g_task_new(G_OBJECT(dialog->window), NULL, import_task_done, dialog);
    g_task_set_task_data(task, import, (GDestroyNotify)import_task_free);
    g_task_run_in_thread(task, import_task_thread);
    g_object_unref(task);
}


static void scan_pair_dialog_destroy_cb(GtkWidget *widget,
                                        gpointer user_data) {
    (void)widget;

    ScanPairDialog *dialog = user_data;
    if (dialog && dialog->state)
        mb_gnome_window_refresh(dialog->state);

    scan_pair_dialog_free(dialog);
}

static void show_scan_pair_dialog(MbGnomeWindowState *state) {
    if (!state)
        return;

    ScanPairDialog *dialog = g_new0(ScanPairDialog, 1);
    dialog->state = state;

    dialog->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog->window), "Adicionar instrumento");
    gtk_window_set_modal(GTK_WINDOW(dialog->window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog->window), GTK_WINDOW(state->window));
    gtk_window_set_default_size(GTK_WINDOW(dialog->window), 520, 420);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(dialog->window), root);

    GtkWidget *title = label_new("Adicionar instrumento BLE-MIDI", "page-title");
    gtk_box_append(GTK_BOX(root), title);

    dialog->status_label = label_new("Procurando instrumentos…", "muted-text");
    gtk_box_append(GTK_BOX(root), dialog->status_label);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_size_request(scroller, -1, 220);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(root), scroller);

    dialog->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dialog->list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), dialog->list);

    g_signal_connect(dialog->list,
                     "row-selected",
                     G_CALLBACK(scan_pair_row_selected),
                     dialog);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(root), actions);

    GtkWidget *close = gtk_button_new_with_label("Fechar");
    gtk_widget_add_css_class(close, "suggested-action");
    gtk_box_append(GTK_BOX(actions), close);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), dialog->window);

    g_signal_connect(dialog->window,
                     "destroy",
                     G_CALLBACK(scan_pair_dialog_destroy_cb),
                     dialog);

    gtk_window_present(GTK_WINDOW(dialog->window));

    scan_pair_dialog_start_scan(dialog);
}


static void scan_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;

    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    if (!state->bluez_available || !state->bluez_powered) {
        show_error_dialog(state,
                          "Bluetooth indisponível",
                          "Ligue o Bluetooth antes de buscar instrumentos BLE-MIDI.");
        return;
    }

    if (state->bluez_discovering)
        return;

    show_scan_pair_dialog(state);
}


static void diagnostics_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    show_diagnostics_dialog(user_data);
}

static void row_selected_cb(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;

    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    if (!row)
        return;

    const char *device_id = g_object_get_data(G_OBJECT(row), "device-id");
    if (!device_id)
        return;

    g_free(state->selected_id);
    state->selected_id = g_strdup(device_id);
    update_main_panel(state);
}

static void device_command_task_free(DeviceCommandTask *task) {
    if (!task)
        return;
    g_free(task->verb);
    g_free(task->device_id);
    g_free(task);
}

static void device_command_task_thread(GTask *task,
                                       gpointer source_object,
                                       gpointer task_data,
                                       GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    DeviceCommandTask *cmd = task_data;
    GError *error = NULL;
    bool ok = false;

    if (g_strcmp0(cmd->verb, "connect") == 0) {
        MbUiDeviceConfig config;
        mb_ui_device_config_defaults(&config);
        config.connect_on_start = gtk_switch_get_active(GTK_SWITCH(cmd->state->auto_reconnect_switch));
        config.reconnect_on_link_loss = gtk_switch_get_active(GTK_SWITCH(cmd->state->auto_reconnect_switch));
        config.enable_tx = true;
        ok = mb_ui_facade_connect_with_config(cmd->state->facade, cmd->device_id, &config, &error);
    } else if (g_strcmp0(cmd->verb, "disconnect") == 0) {
        ok = mb_ui_facade_disconnect(cmd->state->facade, cmd->device_id, &error);
    } else if (g_strcmp0(cmd->verb, "refresh") == 0) {
        ok = mb_ui_facade_refresh_device(cmd->state->facade, cmd->device_id, &error);
    } else if (g_strcmp0(cmd->verb, "remove") == 0) {
        ok = mb_ui_facade_remove_imported_device(cmd->state->facade, cmd->device_id, &error);
    } else if (g_strcmp0(cmd->verb, "forget") == 0) {
        ok = mb_ui_facade_forget_device(cmd->state->facade, cmd->device_id, &error);
    } else {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "comando desconhecido");
    }

    if (!ok)
        g_task_return_error(task, error ? error : g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "falha no comando"));
    else
        g_task_return_boolean(task, TRUE);
}

static void device_command_done(GObject *source_object,
                                GAsyncResult *result,
                                gpointer user_data) {
    (void)source_object;

    MbGnomeWindowState *state = user_data;
    GError *error = NULL;

    state->command_in_flight = false;

    DeviceCommandTask *cmd = g_task_get_task_data(G_TASK(result));
    bool was_remove = cmd && g_strcmp0(cmd->verb, "remove") == 0;
    bool was_forget = cmd && g_strcmp0(cmd->verb, "forget") == 0;

    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        g_printerr("[midi-ble-rt-gui] ERROR: device command failed: %s\n",
                   error && error->message ? error->message : "Erro desconhecido");

        update_main_panel(state);
        show_error_dialog(state, "Falha no Serviço MIDI-BLE", error ? error->message : "Erro desconhecido");
        g_clear_error(&error);
        return;
    }

    if (was_remove || was_forget)
        g_clear_pointer(&state->selected_id, g_free);

    mb_gnome_window_refresh(state);
}

static void device_command(MbGnomeWindowState *state, const char *verb) {
    if (!state || state->command_in_flight)
        return;

     const MbUiDevice *device = selected_device(state);
    if (!device) {
        show_error_dialog(state, "Nenhum instrumento conhecido", "Selecione um instrumento BLE-MIDI primeiro.");
        return;
    }

    DeviceCommandTask *cmd = g_new0(DeviceCommandTask, 1);
    cmd->state = state;
    cmd->verb = g_strdup(verb);
    cmd->device_id = g_strdup(device->id);

    state->command_in_flight = true;
    update_action_sensitivity(state, device);

    GTask *task = g_task_new(G_OBJECT(state->window), NULL, device_command_done, state);
    g_task_set_task_data(task, cmd, (GDestroyNotify)device_command_task_free);
    g_task_run_in_thread(task, device_command_task_thread);
    g_object_unref(task);
}

static void connect_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;

    MbGnomeWindowState *state = user_data;
    const MbUiDevice *device = selected_device(state);

    if (device && state_is_streaming(device->state))
        device_command(state, "disconnect");
    else
        device_command(state, "connect");
}


static void forget_confirm_response_cb(AdwAlertDialog *dialog,
                                       const char *response,
                                       gpointer user_data) {
    (void)dialog;

    MbGnomeWindowState *state = user_data;

    if (g_strcmp0(response, "remove") == 0) {
        device_command(state, "remove");
        return;
    }

    if (g_strcmp0(response, "forget") == 0) {
        device_command(state, "forget");
        return;
    }
}

static void forget_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;

    MbGnomeWindowState *state = user_data;
    const MbUiDevice *device = selected_device(state);

    if (!device) {
        show_error_dialog(state,
                          "Nenhum instrumento selecionado",
                          "Selecione um instrumento BLE-MIDI primeiro.");
        return;
    }

    char *body = g_strdup_printf(
        "Escolha como remover “%s”.\n\n"
        "Excluir remove apenas o instrumento do catálogo local MIDI-BLE-RT. "
        "Excluir e esquecer também remove o pareamento/cache do BlueZ.",
        safe(device->name, "Dispositivo BLE-MIDI"));

    AdwDialog *dialog = adw_alert_dialog_new("Excluir instrumento?", body);

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancelar");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "remove", "Excluir");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "forget", "Excluir e esquecer");

    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "forget",
                                             ADW_RESPONSE_DESTRUCTIVE);

    g_signal_connect(dialog,
                     "response",
                     G_CALLBACK(forget_confirm_response_cb),
                     state);

    adw_dialog_present(dialog, GTK_WIDGET(state->window));
    g_free(body);
}

static void install_css(void) {
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".app-bg { background: @window_bg_color; }"
        ".sidebar { background: alpha(@card_bg_color, 0.72); border-right: 1px solid alpha(@window_fg_color, 0.10); padding: 22px; }"
        ".main { padding: 26px 34px; }"
        ".bottom-bar { border-top: 1px solid alpha(@window_fg_color, 0.10); padding: 10px 22px; background: alpha(@card_bg_color, 0.60); }"
        ".section-title { font-size: 18px; font-weight: 800; }"
        ".scan-button { min-height: 54px; border-radius: 13px; font-weight: 800; }"
        ".device-list-card { background: alpha(@card_bg_color, 0.65); border-radius: 15px; }"
        ".device-row { padding: 14px 16px; border-radius: 14px; }"
        ".device-row-name { font-weight: 800; font-size: 15px; }"
        ".device-row-status { color: alpha(@window_fg_color, 0.70); }"
        ".circle-icon { min-width: 92px; min-height: 92px; border-radius: 999px; background: alpha(#3584e4, 0.12); }"
        ".small-circle { min-width: 46px; min-height: 46px; background: alpha(#1c71d8, 0.95); color: white; }"
        ".hero-card { background: @card_bg_color; border-radius: 18px; padding: 28px 32px; box-shadow: 0 1px 5px alpha(black, 0.10); }"
        ".config-card { background: @card_bg_color; border-radius: 18px; padding: 22px; box-shadow: 0 1px 5px alpha(black, 0.10); }"
        ".details-card { background: @card_bg_color; border-radius: 15px; padding: 14px 18px; box-shadow: 0 1px 4px alpha(black, 0.08); }"
        ".device-name { font-size: 28px; font-weight: 900; }"
        ".device-kind { font-size: 15px; color: alpha(@window_fg_color, 0.62); }"
        ".muted-text { color: alpha(@window_fg_color, 0.58); }"
        ".ok-text { color: #1a9c39; font-weight: 700; }"
        ".err-text { color: #c01c28; font-weight: 700; }"
        ".config-title { font-size: 17px; font-weight: 800; }"
        ".config-row { padding: 9px 0; }"
        ".config-icon { opacity: 0.78; }"
        ".config-subtitle { color: alpha(@window_fg_color, 0.58); font-size: 12px; }"
        ".inline-status { margin-top: 6px; }"
        ".footer-text { color: alpha(@window_fg_color, 0.70); }"
        ".footer-item { margin-right: 18px; }"
    );

    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void state_free(MbGnomeWindowState *state) {
    if (!state)
        return;


    if (state->daemon_transition_source_id)
        g_source_remove(state->daemon_transition_source_id);

    mb_daemon_observer_free(state->daemon_observer);

    mb_ui_snapshot_free(state->snapshot);
    mb_ui_facade_free(state->facade);
    g_free(state->selected_id);
    g_free(state);
}

GtkWindow *mb_gnome_window_new(AdwApplication *application) {
    install_css();

    MbGnomeWindowState *state = g_new0(MbGnomeWindowState, 1);
    state->facade = mb_ui_facade_new();

    state->window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
    gtk_window_set_title(GTK_WINDOW(state->window), "MIDI-BLE-RT");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1180, 760);
    g_object_set_data_full(G_OBJECT(state->window), "mb-gnome-window-state", state, (GDestroyNotify)state_free);

    GtkWidget *toolbar = adw_toolbar_view_new();
    gtk_widget_add_css_class(toolbar, "app-bg");

    GtkWidget *header = adw_header_bar_new();
    GtkWidget *title = adw_window_title_new("MIDI-BLE-RT", "Conexão BLE-MIDI simples");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);

    GtkWidget *menu = gtk_button_new_from_icon_name("open-menu-symbolic");
    GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings, "Detalhes técnicos");

    adw_header_bar_pack_start(ADW_HEADER_BAR(header), menu);

    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings);

    GtkWidget *daemon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(daemon_box, "daemon-switch-row");
    gtk_widget_set_valign(daemon_box, GTK_ALIGN_CENTER);

    state->daemon_spinner = gtk_spinner_new();
    gtk_widget_set_visible(state->daemon_spinner, FALSE);
    gtk_widget_set_valign(state->daemon_spinner, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(daemon_box), state->daemon_spinner);

    state->daemon_switch_label = label_new("Serviço MIDI-BLE desativado", "muted-text");
    gtk_box_append(GTK_BOX(daemon_box), state->daemon_switch_label);

    state->daemon_switch = gtk_switch_new();
    gtk_widget_set_valign(state->daemon_switch, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(daemon_box), state->daemon_switch);

    g_signal_connect(state->daemon_switch,
                     "notify::active",
                     G_CALLBACK(daemon_switch_notify_active_cb),
                     state);

    adw_header_bar_pack_end(ADW_HEADER_BAR(header), daemon_box);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(content, TRUE);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 335, -1);

    state->scan_button = icon_button_new("edit-find-symbolic", "Adicionar instrumento");
    gtk_widget_add_css_class(state->scan_button, "suggested-action");
    gtk_widget_add_css_class(state->scan_button, "scan-button");
    gtk_widget_set_sensitive(state->scan_button, FALSE);
    gtk_widget_set_can_target(state->scan_button, TRUE);
    gtk_box_append(GTK_BOX(sidebar), state->scan_button);

    g_signal_connect(state->scan_button, "clicked", G_CALLBACK(scan_clicked_cb), state);

    GtkWidget *sidebar_title = label_new("Instrumentos conhecidos", "section-title");
    gtk_box_append(GTK_BOX(sidebar), sidebar_title);

    GtkWidget *list_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(list_card, "device-list-card");
    state->sidebar_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->sidebar_list), GTK_SELECTION_SINGLE);
    GtkWidget *sidebar_scroller = gtk_scrolled_window_new();

    gtk_widget_set_vexpand(sidebar_scroller, TRUE);

    gtk_widget_set_hexpand(sidebar_scroller, TRUE);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroller),

                                   GTK_POLICY_NEVER,

                                   GTK_POLICY_AUTOMATIC);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroller),

                                  state->sidebar_list);

    gtk_box_append(GTK_BOX(list_card), sidebar_scroller);
    gtk_box_append(GTK_BOX(sidebar), list_card);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(sidebar), spacer);


    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(main, "main");
    gtk_widget_set_hexpand(main, TRUE);
    gtk_widget_set_vexpand(main, TRUE);

    state->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_valign(state->empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(state->empty_box, GTK_ALIGN_CENTER);

    GtkWidget *empty_icon = musical_keyboard_icon_new(NULL, 88);
    GtkWidget *empty_title = label_new("Nenhum instrumento conhecido", "device-name");
    gtk_label_set_xalign(GTK_LABEL(empty_title), 0.5f);
    GtkWidget *empty_hint = label_new("Adicione um instrumento BLE-MIDI para começar.", "muted-text");
    gtk_label_set_xalign(GTK_LABEL(empty_hint), 0.5f);

    gtk_box_append(GTK_BOX(state->empty_box), empty_icon);
    gtk_box_append(GTK_BOX(state->empty_box), empty_title);
    gtk_box_append(GTK_BOX(state->empty_box), empty_hint);

    state->device_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(hero, "hero-card");

    GtkWidget *hero_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    state->device_icon = musical_keyboard_icon_new(NULL, 88);
    gtk_box_append(GTK_BOX(hero_top), state->device_icon);

    GtkWidget *hero_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(hero_text, TRUE);

    state->device_name = label_new("Roland GO:KEYS", "device-name");
    state->device_kind = label_new("Instrumento BLE-MIDI importado", "device-kind");
    state->device_state = label_new("Pronto para conectar", "ok-text");

    gtk_box_append(GTK_BOX(hero_text), state->device_name);
    gtk_box_append(GTK_BOX(hero_text), state->device_kind);
    gtk_box_append(GTK_BOX(hero_text), state->device_state);
    gtk_box_append(GTK_BOX(hero_top), hero_text);

    gtk_box_append(GTK_BOX(hero), hero_top);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(hero), separator);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);

    state->connect_button = icon_button_new("insert-link-symbolic", "Conectar");
    gtk_widget_add_css_class(state->connect_button, "suggested-action");
    gtk_widget_set_hexpand(state->connect_button, TRUE);

    state->disconnect_button = NULL;

    state->refresh_button = NULL;

    state->forget_button = icon_button_new("edit-delete-symbolic", "Excluir");
    gtk_widget_add_css_class(state->forget_button, "destructive-action");
    gtk_widget_set_hexpand(state->forget_button, TRUE);

    gtk_box_append(GTK_BOX(actions), state->connect_button);
    gtk_box_append(GTK_BOX(hero), actions);

    GtkWidget *config = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(config, "config-card");
    state->config_card = config;

    GtkWidget *config_title = label_new("Configuração", "config-title");
    gtk_box_append(GTK_BOX(config), config_title);

    state->auto_reconnect_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->auto_reconnect_switch), TRUE);
    gtk_box_append(GTK_BOX(config),
                   config_row_new("view-refresh-symbolic",
                                  "Reconectar automaticamente",
                                  "Reconecta quando o instrumento estiver disponível",
                                  state->auto_reconnect_switch));

    state->scan_on_open_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->scan_on_open_switch), FALSE);
    gtk_box_append(GTK_BOX(config),
                   config_row_new("edit-find-symbolic",
                                  "Escanear ao abrir",
                                  "Inicia a busca por instrumentos ao abrir o aplicativo",
                                  state->scan_on_open_switch));

    const char *profile_items[] = {
        "Automático",
        "BLE-MIDI padrão",
        NULL,
    };
    GtkStringList *profile_model = gtk_string_list_new(profile_items);
    state->profile_combo = gtk_drop_down_new(G_LIST_MODEL(profile_model), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->profile_combo), 0);
    gtk_widget_set_size_request(state->profile_combo, 150, -1);
    gtk_box_append(GTK_BOX(config),
                   config_row_new("avatar-default-symbolic",
                                  "Perfil do instrumento",
                                  "Define o perfil BLE-MIDI a ser usado",
                                  state->profile_combo));

    const char *timeout_items[] = {
        "10 s",
        "20 s",
        "30 s",
        NULL,
    };
    GtkStringList *timeout_model = gtk_string_list_new(timeout_items);
    state->timeout_combo = gtk_drop_down_new(G_LIST_MODEL(timeout_model), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->timeout_combo), 0);
    gtk_widget_set_size_request(state->timeout_combo, 150, -1);
    gtk_box_append(GTK_BOX(config),
                   config_row_new("alarm-symbolic",
                                  "Timeout de varredura",
                                  "Tempo de busca por instrumentos",
                                  state->timeout_combo));

    state->clock_filter_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->clock_filter_switch), FALSE);
    gtk_box_append(GTK_BOX(config),
                   config_row_new("media-playlist-shuffle-symbolic",
                                  "Filtro de clock MIDI",
                                  "Suaviza variações de clock recebidas",
                                  state->clock_filter_switch));

    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(details, "details-card");
    state->details_card = details;

    GtkWidget *details_icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
    GtkWidget *details_label = label_new("Detalhes técnicos", "config-title");
    GtkWidget *details_arrow = gtk_image_new_from_icon_name("pan-down-symbolic");
    gtk_widget_set_hexpand(details_label, TRUE);

    gtk_box_append(GTK_BOX(details), details_icon);
    gtk_box_append(GTK_BOX(details), details_label);
    gtk_box_append(GTK_BOX(details), details_arrow);

    gtk_box_append(GTK_BOX(state->device_box), hero);
    gtk_box_append(GTK_BOX(state->device_box), config);
    gtk_box_append(GTK_BOX(state->device_box), details);

    gtk_box_append(GTK_BOX(main), state->empty_box);
    gtk_box_append(GTK_BOX(main), state->device_box);

    gtk_box_append(GTK_BOX(content), sidebar);
    gtk_box_append(GTK_BOX(content), main);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(footer, "bottom-bar");

    GtkWidget *svc = status_footer_item(&state->service_dot, &state->service_label, "Serviço MIDI-BLE inativo");
    GtkWidget *sep1 = gtk_label_new("•");
    GtkWidget *devices = status_footer_item(NULL, &state->footer_devices_label, "0 instrumentos importados");
    GtkWidget *sep2 = gtk_label_new("•");
    GtkWidget *conn = status_footer_item(NULL, &state->footer_connection_label, "Nenhum conectado");

    gtk_box_append(GTK_BOX(footer), svc);
    gtk_box_append(GTK_BOX(footer), sep1);
    gtk_box_append(GTK_BOX(footer), devices);
    gtk_box_append(GTK_BOX(footer), sep2);
    gtk_box_append(GTK_BOX(footer), conn);

    gtk_box_append(GTK_BOX(outer), content);
    gtk_box_append(GTK_BOX(outer), footer);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), outer);
    adw_application_window_set_content(state->window, toolbar);

    g_signal_connect(settings, "clicked", G_CALLBACK(diagnostics_clicked_cb), state);
    g_signal_connect(state->sidebar_list, "row-selected", G_CALLBACK(row_selected_cb), state);
    g_signal_connect(state->connect_button, "clicked", G_CALLBACK(connect_clicked_cb), state);
    if (state->forget_button)
        g_signal_connect(state->forget_button, "clicked", G_CALLBACK(forget_clicked_cb), state);

    gtk_widget_set_visible(state->device_box, FALSE);
    gtk_widget_set_sensitive(state->sidebar_list, FALSE);

    /*
     * Initial visual state. The daemon observer will become the root state for
     * scan/list enablement in the next step.
     */
    state->daemon_functional = false;
    gtk_switch_set_active(GTK_SWITCH(state->daemon_switch), FALSE);
    set_label(state->daemon_switch_label, "Verificando Serviço MIDI-BLE");
    update_action_sensitivity(state, NULL);

    /*
     * Signal the initial daemon state to every dependent component.
     */
    daemon_root_observer_set_functional(state, false);
    update_scan_button_state(state);

    state->daemon_observer = mb_daemon_observer_new();

    GError *observer_error = NULL;
    if (!mb_daemon_observer_start(state->daemon_observer,
                                  daemon_observer_state_changed_cb,
                                  state,
                                  &observer_error)) {
        g_printerr("[midi-ble-rt-gui] daemon observer start failed: %s\n",
                   observer_error && observer_error->message ? observer_error->message : "unknown error");
        g_clear_error(&observer_error);
        set_label(state->daemon_switch_label, "Serviço MIDI-BLE indisponível");
    }

    daemon_dbus_observer_start(state);

    /*
     * BlueZ observer intentionally disabled.
     *
     * BlueZ state belongs to midi-ble-rtd.  The GUI will regain scan/import
     * through daemon D-Bus methods, not through direct BlueZ observation.
     */
    state->bluez_observer = NULL;
    bluez_observer_state_changed_cb(false, false, false, false, NULL, state);

    mb_gnome_window_refresh(state);

    return GTK_WINDOW(state->window);
}
