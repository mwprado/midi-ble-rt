#include "mb-gnome-window.h"
#include "mb-ui-facade.h"

#include <gio/gio.h>
#include <stdbool.h>
#include <string.h>

#define REFRESH_INTERVAL_SECONDS 2

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
    GSubprocess *daemon_process;
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

    GtkWidget *auto_reconnect_switch;
    GtkWidget *scan_on_open_switch;
    GtkWidget *clock_filter_switch;
    GtkWidget *profile_combo;
    GtkWidget *timeout_combo;

    char *selected_id;
    guint refresh_source_id;
} MbGnomeWindowState;
static void show_scan_pair_dialog(MbGnomeWindowState *state);


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


static void daemon_switch_notify_active_cb(GObject *object,
                                           GParamSpec *pspec,
                                           gpointer user_data);
static void show_error_dialog(MbGnomeWindowState *state, const char *title, const char *message);
static void mb_gnome_window_refresh(MbGnomeWindowState *state);


typedef struct {
    MbGnomeWindowState *state;
    char *verb;
    char *device_id;
} DeviceCommandTask;

typedef struct {
    MbGnomeWindowState *state;
    unsigned timeout_seconds;
} ScanTask;

static void mb_gnome_window_refresh(MbGnomeWindowState *state);

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

    if (!device->paired)
        return true;

    return g_ascii_strcasecmp(safe(device->state, ""), "UNPAIRED") == 0;
}

static bool device_is_functional_for_gui(const MbUiDevice *device) {
    return device && device->imported && !device_is_unpaired_for_gui(device);
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
    if (!state || !state->snapshot)
        return NULL;

    const MbUiDevice *device = mb_ui_snapshot_find_device(state->snapshot, state->selected_id);
    if (device)
        return device;

    return mb_ui_snapshot_get_device(state->snapshot, 0);
}


static char *default_config_dir(void) {
    const char *home = g_get_home_dir();

    if (!home || !*home)
        return g_strdup(".config/midi-ble-rt");

    return g_build_filename(home, ".config", "midi-ble-rt", NULL);
}

static bool start_daemon_from_gui(MbGnomeWindowState *state, GError **error) {
    if (!state)
        return false;

    if (state->daemon_process) {
        g_printerr("[midi-ble-rt-gui] daemon already started by GUI\n");
        return true;
    }

    char *config_dir = default_config_dir();

    g_printerr("[midi-ble-rt-gui] daemon start: midi-ble-rtd --config %s\n", config_dir);

    state->daemon_process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                             G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                             error,
                                             "midi-ble-rtd",
                                             "--config",
                                             config_dir,
                                             NULL);

    g_free(config_dir);

    if (!state->daemon_process) {
        if (error && *error)
            g_printerr("[midi-ble-rt-gui] ERROR: daemon start failed: %s\n", (*error)->message);
        return false;
    }

    return true;
}

static void stop_daemon_from_gui(MbGnomeWindowState *state) {
    if (!state)
        return;

    if (state->daemon_process) {
        g_printerr("[midi-ble-rt-gui] daemon stop: terminating GUI-owned midi-ble-rtd\n");
        g_subprocess_force_exit(state->daemon_process);
        g_clear_object(&state->daemon_process);
        return;
    }

    g_printerr("[midi-ble-rt-gui] daemon stop ignored: daemon was not started by this GUI instance\n");
}

static void update_daemon_switch_state(MbGnomeWindowState *state) {
    if (!state || !state->daemon_switch)
        return;

    bool online = state->snapshot && state->snapshot->status.online;

    g_signal_handlers_block_by_func(state->daemon_switch,
                                    G_CALLBACK(daemon_switch_notify_active_cb),
                                    state);

    gtk_switch_set_active(GTK_SWITCH(state->daemon_switch), online);

    g_signal_handlers_unblock_by_func(state->daemon_switch,
                                      G_CALLBACK(daemon_switch_notify_active_cb),
                                      state);

    if (state->daemon_switch_label)
        set_label(state->daemon_switch_label, online ? "Serviço MIDI-BLE ativado" : "Serviço MIDI-BLE desativado");
}

static void daemon_switch_notify_active_cb(GObject *object,
                                           GParamSpec *pspec,
                                           gpointer user_data) {
    (void)pspec;

    MbGnomeWindowState *state = user_data;

    if (!state)
        return;

    bool requested_active = gtk_switch_get_active(GTK_SWITCH(object));
    bool online = state->snapshot && state->snapshot->status.online;

    if (requested_active == online)
        return;

    if (requested_active) {
        GError *error = NULL;

        if (!start_daemon_from_gui(state, &error)) {
            show_error_dialog(state,
                              "Falha ao iniciar Serviço MIDI-BLE",
                              error ? error->message : "Erro desconhecido");
            g_clear_error(&error);
        }

        mb_gnome_window_refresh(state);
        return;
    }

    stop_daemon_from_gui(state);
    mb_gnome_window_refresh(state);
}

static void update_action_sensitivity(MbGnomeWindowState *state, const MbUiDevice *device) {
    bool has_device = device != NULL;
    bool busy = state->command_in_flight;
    bool streaming = has_device && state_is_streaming(device->state);
    bool service_online = state->snapshot && state->snapshot->status.online;
    bool functional = has_device && device_is_functional_for_gui(device);

    /*
     * Workflow table:
     * - imported + paired: visible, functional, connect/disconnect toggle enabled.
     * - imported + not paired: visible, must pair again, connect toggle disabled.
     * - not imported: absent from main list.
     *
     * Excluir is local catalog removal and must not require daemon online.
     */
    if (state->connect_button) {
        button_set_icon_text(state->connect_button,
                             streaming ? "edit-clear-symbolic" : "insert-link-symbolic",
                             streaming ? "Desconectar" : "Conectar");
        gtk_widget_set_sensitive(state->connect_button,
                                 service_online && functional && !busy);
    }

    if (state->disconnect_button)
        gtk_widget_set_visible(state->disconnect_button, FALSE);

    gtk_widget_set_sensitive(state->refresh_button,
                             service_online && has_device && !busy);

    gtk_widget_set_sensitive(state->forget_button,
                             has_device && !busy);

    if (state->scan_button)
        gtk_widget_set_sensitive(state->scan_button, !state->scan_in_flight && !busy);
}




static bool device_is_visible_in_main_list(const MbUiDevice *device);
static void update_main_panel(MbGnomeWindowState *state) {
    bool online = state->snapshot && state->snapshot->status.online;
    const MbUiDevice *device = selected_device(state);

    if (!device_is_visible_in_main_list(device))
        device = NULL;

    set_label(state->service_label, online ? "Serviço MIDI-BLE ativo" : "Serviço MIDI-BLE inativo");
    gtk_widget_remove_css_class(state->service_dot, "ok-text");
    gtk_widget_remove_css_class(state->service_dot, "err-text");
    gtk_widget_add_css_class(state->service_dot, online ? "ok-text" : "err-text");

    guint count = state->snapshot && state->snapshot->devices ? state->snapshot->devices->len : 0;
    char devices_text[96];
    g_snprintf(devices_text,
               sizeof(devices_text),
               "%u instrumento%s importado%s",
               count,
               count == 1 ? "" : "s",
               count == 1 ? "" : "s");
    set_label(state->footer_devices_label, devices_text);

    if (!device) {
        gtk_widget_set_visible(state->empty_box, TRUE);
        gtk_widget_set_visible(state->device_box, FALSE);
        set_label(state->footer_connection_label, "Nenhum conectado");
        update_action_sensitivity(state, NULL);
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
    update_action_sensitivity(state, device);
    update_daemon_switch_state(state);
}



static bool device_is_visible_in_main_list(const MbUiDevice *device) {
    return device != NULL;
}


static GtkWidget *device_row_new(const MbUiDevice *device) {
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

    gtk_box_append(GTK_BOX(row), text);
    return row;
}

static void rebuild_sidebar(MbGnomeWindowState *state) {
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

    for (guint i = 0; i < state->snapshot->devices->len; i++) {
        const MbUiDevice *device = g_ptr_array_index(state->snapshot->devices, i);

        if (!device_is_visible_in_main_list(device))
            continue;
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), device_row_new(device));
        g_object_set_data_full(G_OBJECT(row), "device-id", g_strdup(device->id), g_free);
        gtk_list_box_append(GTK_LIST_BOX(state->sidebar_list), row);

        if (state->selected_id && g_strcmp0(state->selected_id, device->id) == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(state->sidebar_list), GTK_LIST_BOX_ROW(row));
    }

    if (!state->selected_id && state->snapshot->devices->len > 0) {
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(state->sidebar_list), 0);
        if (first)
            gtk_list_box_select_row(GTK_LIST_BOX(state->sidebar_list), first);
    }

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

static GtkWidget *scan_pair_device_row_new(ScanPairDialog *dialog, const MbUiDevice *device) {
    GtkWidget *row = gtk_list_box_row_new();

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(outer, 8);
    gtk_widget_set_margin_bottom(outer, 8);
    gtk_widget_set_margin_start(outer, 10);
    gtk_widget_set_margin_end(outer, 10);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget *name = label_new(device && device->name ? device->name : "Dispositivo BLE-MIDI", "device-name");

    const char *import_state = device && device->imported ? "Já importado" : "Não importado";

    char *meta_text = g_strdup_printf("%s · %s · %s · %s",
                                      device && device->address ? device->address : "-",
                                      import_state,
                                      device && device->paired
                                          ? "Pareado no BlueZ"
                                          : (device && device->imported ? "Despareado no BlueZ" : "Não pareado no BlueZ"),
                                      device && device->profile && *device->profile ? device->profile : "BLE-MIDI");

    GtkWidget *meta = label_new(meta_text, "muted-text");
    g_free(meta_text);

    gtk_box_append(GTK_BOX(text), name);
    gtk_box_append(GTK_BOX(text), meta);
    gtk_box_append(GTK_BOX(outer), text);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);

    GtkWidget *pair_import_button = gtk_button_new_with_label("Parear e Importar");
    gtk_widget_add_css_class(pair_import_button, "suggested-action");

    /*
     * User-facing rule:
     *
     * - paired + imported: nothing to do
     * - paired + not imported: import/create .ini
     * - not paired + imported: pair again, keep existing .ini
     * - not paired + not imported: pair and create .ini
     */
    bool already_ready = device && device->paired && device->imported;
    gtk_widget_set_sensitive(pair_import_button, device && !already_ready);

    if (already_ready)
        gtk_button_set_label(GTK_BUTTON(pair_import_button), "Já pronto");

    if (device && device->id) {
        g_object_set_data_full(G_OBJECT(pair_import_button), "device-id", g_strdup(device->id), g_free);
        g_object_set_data_full(G_OBJECT(row), "device-id", g_strdup(device->id), g_free);
    }

    g_signal_connect(pair_import_button,
                     "clicked",
                     G_CALLBACK(scan_pair_dialog_row_pair_import_clicked),
                     dialog);

    gtk_box_append(GTK_BOX(actions), pair_import_button);
    gtk_box_append(GTK_BOX(outer), actions);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
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
    GError *error = NULL;

    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        g_printerr("[midi-ble-rt-gui] ERROR: scan dialog failed: %s\n",
                   error && error->message ? error->message : "Erro desconhecido");

        if (dialog && dialog->status_label)
            set_label(dialog->status_label, error ? error->message : "Falha ao escanear.");

        g_clear_error(&error);
        scan_pair_dialog_set_busy(dialog, false);
        return;
    }

    if (dialog->snapshot)
        mb_ui_snapshot_free(dialog->snapshot);

    dialog->snapshot = mb_ui_facade_get_scan_snapshot(dialog->state->facade);

    if (dialog->status_label)
        set_label(dialog->status_label, "Escolha um instrumento para preparar o uso.");

    scan_pair_dialog_set_busy(dialog, false);
    scan_pair_dialog_rebuild_list(dialog);
}


static void scan_pair_dialog_refresh_snapshot(ScanPairDialog *dialog) {
    if (!dialog || !dialog->state || !dialog->state->facade)
        return;

    GError *scan_error = NULL;

    if (!mb_ui_facade_scan_devices(dialog->state->facade, 2, &scan_error)) {
        g_printerr("[midi-ble-rt-gui] discovery refresh skipped/failed: %s\n",
                   scan_error && scan_error->message ? scan_error->message : "unknown error");
        g_clear_error(&scan_error);
    }

    if (dialog->snapshot)
        mb_ui_snapshot_free(dialog->snapshot);

    dialog->snapshot = mb_ui_facade_get_scan_snapshot(dialog->state->facade);
    scan_pair_dialog_rebuild_list(dialog);
}

static void scan_pair_dialog_start_scan(ScanPairDialog *dialog) {
    if (!dialog || dialog->busy)
        return;

    scan_pair_dialog_set_busy(dialog, true);

    if (dialog->status_label)
        set_label(dialog->status_label, "Procurando instrumentos BLE-MIDI...");

    ScanPairTask *scan = g_new0(ScanPairTask, 1);
    scan->dialog = dialog;
    scan->timeout_seconds = 10;

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

        if (dialog && dialog->status_label)
            set_label(dialog->status_label, error ? error->message : "Falha ao importar instrumento.");

        show_error_dialog(dialog->state,
                          "Falha ao importar instrumento",
                          error ? error->message : "Erro desconhecido");

        g_clear_error(&error);
        scan_pair_dialog_set_busy(dialog, false);
        return;
    }

    g_printerr("[midi-ble-rt-gui] import: device enrolled successfully\n");

    if (dialog && dialog->status_label)
        set_label(dialog->status_label, "Instrumento pronto para uso.");

    scan_pair_dialog_refresh_snapshot(dialog);
    scan_pair_dialog_set_busy(dialog, false);
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

    if (dialog->status_label)
        set_label(dialog->status_label, "Preparando instrumento...");

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

    dialog->status_label = label_new("Procurando instrumentos BLE-MIDI...", "muted-text");
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

    show_scan_pair_dialog(state);
}





static void diagnostics_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    show_diagnostics_dialog(user_data);
}

static void row_selected_cb(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;

    MbGnomeWindowState *state = user_data;
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
        show_error_dialog(state, "Nenhum instrumento importado", "Selecione um instrumento BLE-MIDI primeiro.");
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




static void refresh_device_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    device_command(user_data, "refresh");
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

    if (state->refresh_source_id)
        g_source_remove(state->refresh_source_id);

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

    GtkWidget *sidebar_title = label_new("Instrumentos importados", "section-title");
    gtk_box_append(GTK_BOX(sidebar), sidebar_title);

    state->scan_button = icon_button_new("edit-find-symbolic", "Adicionar instrumento");
    gtk_widget_add_css_class(state->scan_button, "suggested-action");
    gtk_widget_add_css_class(state->scan_button, "scan-button");
    gtk_widget_set_sensitive(state->scan_button, TRUE);
    gtk_widget_set_can_target(state->scan_button, TRUE);
    gtk_box_append(GTK_BOX(sidebar), state->scan_button);

    g_signal_connect(state->scan_button, "clicked", G_CALLBACK(scan_clicked_cb), state);

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

    GtkWidget *last_scan = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *last_scan_icon = gtk_image_new_from_icon_name("view-refresh-symbolic");
    state->last_scan_label = label_new("Catálogo atualizado agora", "muted-text");
    gtk_box_append(GTK_BOX(last_scan), last_scan_icon);
    gtk_box_append(GTK_BOX(last_scan), state->last_scan_label);
    gtk_box_append(GTK_BOX(sidebar), last_scan);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(main, "main");
    gtk_widget_set_hexpand(main, TRUE);
    gtk_widget_set_vexpand(main, TRUE);

    state->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_valign(state->empty_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(state->empty_box, GTK_ALIGN_CENTER);

    GtkWidget *empty_icon = musical_keyboard_icon_new(NULL, 88);
    GtkWidget *empty_title = label_new("Nenhum instrumento importado", "device-name");
    gtk_label_set_xalign(GTK_LABEL(empty_title), 0.5f);
    GtkWidget *empty_hint = label_new("Adicione ou importe um instrumento BLE-MIDI para começar.", "muted-text");
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

    state->refresh_button = icon_button_new("view-refresh-symbolic", "Atualizar");
    gtk_widget_set_hexpand(state->refresh_button, TRUE);

    state->forget_button = icon_button_new("edit-delete-symbolic", "Excluir");
    gtk_widget_add_css_class(state->forget_button, "destructive-action");
    gtk_widget_set_hexpand(state->forget_button, TRUE);

    gtk_box_append(GTK_BOX(actions), state->connect_button);
    gtk_box_append(GTK_BOX(actions), state->refresh_button);
    gtk_box_append(GTK_BOX(actions), state->forget_button);
    gtk_box_append(GTK_BOX(hero), actions);

    GtkWidget *config = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(config, "config-card");

    /*
     * Configuração básica ainda é visual/provisória.
     * Mantemos o bloco visível para validar layout, mas sem interação
     * até a semântica ser ligada corretamente à façade/backend.
     */
    gtk_widget_set_sensitive(config, FALSE);

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
    g_signal_connect(state->refresh_button, "clicked", G_CALLBACK(refresh_device_clicked_cb), state);
    g_signal_connect(state->forget_button, "clicked", G_CALLBACK(forget_clicked_cb), state);

    gtk_widget_set_visible(state->device_box, FALSE);
    update_action_sensitivity(state, NULL);

    mb_gnome_window_refresh(state);
    /* Passive polling disabled for now: avoid full-window flicker during manual operations. */

    return GTK_WINDOW(state->window);
}
