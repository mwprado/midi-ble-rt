#include "mb-gui-window.h"
#include <gio/gio.h>
#include <stdbool.h>
#include "mb-gui-ctl-client.h"
#include "mb-gui-model.h"

#include <string.h>

#define REFRESH_INTERVAL_SECONDS 2

typedef struct {
    AdwApplicationWindow *window;
    MbGuiCtlClient *client;
    MbGuiSnapshot *snapshot;
    bool refresh_in_flight;

    GtkWidget *daemon_pill;
    GtkWidget *sidebar_list;
    GtkWidget *device_count_label;

    GtkWidget *title_name;
    GtkWidget *title_address;
    GtkWidget *state_pill;
    GtkWidget *selected_id_label;

    GtkWidget *device_card;
    GtkWidget *connection_card;
    GtkWidget *alsa_card;
    GtkWidget *runtime_card;
    GtkWidget *diagnostics_card;

    GtkWidget *summary_daemon;
    GtkWidget *summary_devices;
    GtkWidget *summary_streaming;
    GtkWidget *summary_queue;

    char *selected_id;
    guint refresh_source_id;
} MbGuiWindowState;

static void mb_gui_window_refresh(MbGuiWindowState *state);

static const char *safe(const char *s, const char *fallback) {
    return s && *s ? s : fallback;
}

static bool state_is_streaming(const char *state) {
    return g_ascii_strcasecmp(safe(state, ""), "STREAMING") == 0;
}

static GtkWidget *label_new(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    if (css_class)
        gtk_widget_add_css_class(label, css_class);
    return label;
}

static GtkWidget *value_label_new(const char *text) {
    GtkWidget *label = label_new(text, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    return label;
}

static void box_clear(GtkWidget *box) {
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static GtkWidget *card_new(const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "card");
    gtk_widget_set_hexpand(box, TRUE);

    GtkWidget *heading = label_new(title, "card-title");
    gtk_box_append(GTK_BOX(box), heading);
    return box;
}

static void card_reset(GtkWidget *card, const char *title) {
    box_clear(card);
    GtkWidget *heading = label_new(title, "card-title");
    gtk_box_append(GTK_BOX(card), heading);
}

static void card_add_row(GtkWidget *card, const char *key, const char *value) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "kv-row");

    GtkWidget *k = label_new(key, "dim-label");
    GtkWidget *v = value_label_new(value);
    gtk_box_append(GTK_BOX(row), k);
    gtk_box_append(GTK_BOX(row), v);
    gtk_box_append(GTK_BOX(card), row);
}

static GtkWidget *pill_new(const char *text, const char *css_class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "pill");
    if (css_class)
        gtk_widget_add_css_class(label, css_class);
    return label;
}

static void set_label(GtkWidget *label, const char *text) {
    if (GTK_IS_LABEL(label))
        gtk_label_set_text(GTK_LABEL(label), text ? text : "");
}

static void set_widget_text_class(GtkWidget *widget,
                                  const char *text,
                                  const char *remove_a,
                                  const char *remove_b,
                                  const char *add) {
    set_label(widget, text);
    if (remove_a)
        gtk_widget_remove_css_class(widget, remove_a);
    if (remove_b)
        gtk_widget_remove_css_class(widget, remove_b);
    if (add)
        gtk_widget_add_css_class(widget, add);
}

static void show_error_dialog(MbGuiWindowState *state, const char *title, const char *message) {
    AdwDialog *dialog = adw_alert_dialog_new(title, message ? message : "Unknown error");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_dialog_present(dialog, GTK_WIDGET(state->window));
}

static const MbGuiDevice *selected_device(MbGuiWindowState *state) {
    if (!state || !state->snapshot)
        return NULL;

    const MbGuiDevice *device = mb_gui_snapshot_find_device(state->snapshot, state->selected_id);
    if (device)
        return device;

    return mb_gui_snapshot_get_device(state->snapshot, 0);
}

static void update_detail_cards(MbGuiWindowState *state) {
    const MbGuiDevice *device = selected_device(state);
    char buf[128];

    if (!device) {
        set_label(state->title_name, "No device selected");
        set_label(state->title_address, "Start midi-ble-rtd or configure a BLE-MIDI device.");
        set_widget_text_class(state->state_pill, "OFFLINE", "ok", "warn", "err");
        set_label(state->selected_id_label, "-");

        card_reset(state->device_card, "Device");
        card_add_row(state->device_card, "Name", "-");
        card_add_row(state->device_card, "Bluetooth Address", "-");

        card_reset(state->connection_card, "Connection");
        card_add_row(state->connection_card, "State", state->snapshot && state->snapshot->last_error ? state->snapshot->last_error : "Unavailable");

        card_reset(state->alsa_card, "ALSA");
        card_add_row(state->alsa_card, "ALSA Port", "-");

        card_reset(state->runtime_card, "Runtime");
        card_add_row(state->runtime_card, "RX Workers", "-");
        card_add_row(state->runtime_card, "TX Workers", "-");

        card_reset(state->diagnostics_card, "Diagnostics");
        card_add_row(state->diagnostics_card, "Stats", "Available after daemon telemetry is enabled");
        return;
    }

    g_free(state->selected_id);
    state->selected_id = g_strdup(device->id);

    set_label(state->title_name, safe(device->name, "Unnamed device"));
    set_label(state->title_address, safe(device->address, "-"));
    set_label(state->selected_id_label, safe(device->id, "-"));
    set_widget_text_class(state->state_pill,
                          safe(device->state, "UNKNOWN"),
                          "ok",
                          "err",
                          state_is_streaming(device->state) ? "ok" : "err");

    card_reset(state->device_card, "Device");
    card_add_row(state->device_card, "Name", safe(device->name, "-"));
    card_add_row(state->device_card, "ID", safe(device->id, "-"));
    card_add_row(state->device_card, "Bluetooth Address", safe(device->address, "-"));

    card_reset(state->connection_card, "Connection");
    card_add_row(state->connection_card, "State", safe(device->state, "UNKNOWN"));
    card_add_row(state->connection_card, "Daemon", state->snapshot->status.online ? "Running" : "Offline");
    card_add_row(state->connection_card, "Lifecycle Busy", safe(state->snapshot->status.lifecycle_busy, "no"));

    card_reset(state->alsa_card, "ALSA");
    if (device->alsa_port >= 0)
        g_snprintf(buf, sizeof(buf), "midi-ble-rt:%d", device->alsa_port);
    else
        g_strlcpy(buf, "-", sizeof(buf));
    card_add_row(state->alsa_card, "ALSA Port", buf);
    card_add_row(state->alsa_card, "Direction", "Duplex");
    card_add_row(state->alsa_card, "TX Thread", safe(state->snapshot->status.alsa_tx_thread, "-"));

    card_reset(state->runtime_card, "Runtime");
    g_snprintf(buf, sizeof(buf), "%u", state->snapshot->status.rx_workers);
    card_add_row(state->runtime_card, "RX Workers", buf);
    g_snprintf(buf, sizeof(buf), "%u", state->snapshot->status.tx_workers);
    card_add_row(state->runtime_card, "TX Workers", buf);
    card_add_row(state->runtime_card, "RTKit", safe(state->snapshot->status.rtkit, "off"));
    card_add_row(state->runtime_card, "RTKit RX", safe(state->snapshot->status.rtkit_rx, "off"));
    card_add_row(state->runtime_card, "RTKit TX", safe(state->snapshot->status.rtkit_tx, "off"));
    g_snprintf(buf, sizeof(buf), "%u", state->snapshot->status.rtkit_priority);
    card_add_row(state->runtime_card, "Priority", buf);

    card_reset(state->diagnostics_card, "Diagnostics");
    card_add_row(state->diagnostics_card, "RX Activity", "stats.tsv integration pending");
    card_add_row(state->diagnostics_card, "TX Activity", "stats.tsv integration pending");
    g_snprintf(buf, sizeof(buf), "%u pending", state->snapshot->status.lifecycle_queue);
    card_add_row(state->diagnostics_card, "Lifecycle Queue", buf);
}

static GtkWidget *device_row_new(const MbGuiDevice *device) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "device-row");

    GtkWidget *icon = gtk_image_new_from_icon_name("audio-x-generic-symbolic");
    gtk_widget_add_css_class(icon, "device-icon");
    gtk_box_append(GTK_BOX(row), icon);

    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(text_box, TRUE);
    GtkWidget *name = label_new(safe(device->name, "Unnamed device"), "device-name");
    GtkWidget *address = label_new(safe(device->address, "-"), "dim-label");
    GtkWidget *state = label_new(safe(device->state, "UNKNOWN"), state_is_streaming(device->state) ? "state-ok" : "state-muted");
    gtk_box_append(GTK_BOX(text_box), name);
    gtk_box_append(GTK_BOX(text_box), address);
    gtk_box_append(GTK_BOX(text_box), state);
    gtk_box_append(GTK_BOX(row), text_box);

    GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_box_append(GTK_BOX(row), chevron);
    return row;
}

static void rebuild_sidebar(MbGuiWindowState *state) {
    GtkWidget *child = gtk_widget_get_first_child(state->sidebar_list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(state->sidebar_list), child);
        child = next;
    }

    if (!state->snapshot || !state->snapshot->devices)
        return;

    for (guint i = 0; i < state->snapshot->devices->len; i++) {
        const MbGuiDevice *device = g_ptr_array_index(state->snapshot->devices, i);
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
}

static void update_summary(MbGuiWindowState *state) {
    char buf[128];
    bool online = state->snapshot && state->snapshot->status.online;

    set_widget_text_class(state->daemon_pill,
                          online ? "Daemon  Running" : "Daemon  Offline",
                          "ok",
                          "err",
                          online ? "ok" : "err");

    g_snprintf(buf, sizeof(buf), "%u devices", state->snapshot ? state->snapshot->devices->len : 0);
    set_label(state->device_count_label, buf);

    set_label(state->summary_daemon, online ? "Daemon\nRunning" : "Daemon\nOffline");
    g_snprintf(buf, sizeof(buf), "Devices\n%u configured", state->snapshot ? state->snapshot->status.devices : 0);
    set_label(state->summary_devices, buf);
    g_snprintf(buf, sizeof(buf), "Streaming\n%u active", state->snapshot ? state->snapshot->status.streaming : 0);
    set_label(state->summary_streaming, buf);
    g_snprintf(buf, sizeof(buf), "Lifecycle Queue\n%u pending", state->snapshot ? state->snapshot->status.lifecycle_queue : 0);
    set_label(state->summary_queue, buf);
}

static void refresh_task_thread(GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;

    MbGuiWindowState *state = task_data;
    MbGuiSnapshot *snapshot = mb_gui_ctl_client_snapshot(state->client);

    g_task_return_pointer(task,
                          snapshot,
                          (GDestroyNotify)mb_gui_snapshot_free);
}

static void refresh_task_done(GObject *source_object,
                              GAsyncResult *result,
                              gpointer user_data) {
    (void)source_object;

    MbGuiWindowState *state = user_data;
    GError *error = NULL;

    MbGuiSnapshot *snapshot =
        g_task_propagate_pointer(G_TASK(result), &error);

    state->refresh_in_flight = false;

    if (error) {
        if (state->snapshot)
            mb_gui_snapshot_free(state->snapshot);

        state->snapshot = mb_gui_snapshot_new();
        state->snapshot->status.online = false;
        state->snapshot->last_error = g_strdup(error->message);

        g_clear_error(&error);
    } else if (snapshot) {
        if (state->snapshot)
            mb_gui_snapshot_free(state->snapshot);

        state->snapshot = snapshot;
    }

    rebuild_sidebar(state);
    update_summary(state);
    update_detail_cards(state);
}

static void mb_gui_window_refresh(MbGuiWindowState *state) {
    if (!state || state->refresh_in_flight)
        return;

    state->refresh_in_flight = true;

    GTask *task = g_task_new(G_OBJECT(state->window),
                             NULL,
                             refresh_task_done,
                             state);

    g_task_set_task_data(task, state, NULL);
    g_task_run_in_thread(task, refresh_task_thread);
    g_object_unref(task);
}

static gboolean refresh_timer_cb(gpointer user_data) {
    mb_gui_window_refresh(user_data);
    return G_SOURCE_CONTINUE;
}

static void refresh_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    mb_gui_window_refresh(user_data);
}

static void device_selected_cb(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    MbGuiWindowState *state = user_data;
    if (!row)
        return;

    const char *device_id = g_object_get_data(G_OBJECT(row), "device-id");
    if (!device_id)
        return;

    g_free(state->selected_id);
    state->selected_id = g_strdup(device_id);
    update_detail_cards(state);
}

static void device_command(MbGuiWindowState *state, const char *verb) {
    const MbGuiDevice *device = selected_device(state);
    if (!device) {
        show_error_dialog(state, "No device selected", "Select a configured BLE-MIDI device first.");
        return;
    }

    GError *error = NULL;
    bool ok = false;
    if (g_strcmp0(verb, "connect") == 0)
        ok = mb_gui_ctl_client_connect_device(state->client, device->id, &error);
    else if (g_strcmp0(verb, "disconnect") == 0)
        ok = mb_gui_ctl_client_disconnect_device(state->client, device->id, &error);
    else if (g_strcmp0(verb, "recheck") == 0)
        ok = mb_gui_ctl_client_recheck_device(state->client, device->id, &error);

    if (!ok) {
        show_error_dialog(state, "Daemon command failed", error ? error->message : "Unknown error");
        g_clear_error(&error);
        return;
    }

    mb_gui_window_refresh(state);
}

static void connect_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    device_command(user_data, "connect");
}

static void disconnect_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    device_command(user_data, "disconnect");
}

static void recheck_clicked_cb(GtkButton *button, gpointer user_data) {
    (void)button;
    device_command(user_data, "recheck");
}

static GtkWidget *summary_tile_new(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.5f);
    gtk_widget_add_css_class(label, "summary-tile");
    gtk_widget_set_hexpand(label, TRUE);
    return label;
}

static void install_css(void) {
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".app-bg { background: #f6f7f8; }"
        ".sidebar { background: #ffffff; border-right: 1px solid alpha(#000, 0.08); padding: 18px; }"
        ".main { padding: 22px; }"
        ".card { background: #ffffff; border-radius: 18px; padding: 18px; box-shadow: 0 1px 4px alpha(#000, 0.08); }"
        ".card-title { font-weight: 700; font-size: 15px; }"
        ".device-row { padding: 12px; border-radius: 14px; }"
        ".device-name { font-weight: 700; }"
        ".dim-label { color: alpha(#000, 0.58); }"
        ".state-ok { color: #1a7f37; font-weight: 700; }"
        ".state-muted { color: alpha(#000, 0.55); font-weight: 700; }"
        ".pill { border-radius: 999px; padding: 6px 12px; font-weight: 700; }"
        ".ok { background: alpha(#2da44e, 0.14); color: #1a7f37; }"
        ".err { background: alpha(#cf222e, 0.12); color: #a40e26; }"
        ".warn { background: alpha(#bf8700, 0.14); color: #7d4e00; }"
        ".title-name { font-size: 26px; font-weight: 800; }"
        ".summary-tile { background: #ffffff; border-radius: 16px; padding: 16px; font-weight: 700; }"
        ".kv-row { padding: 2px 0; }"
    );

    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void state_free(MbGuiWindowState *state) {
    if (!state)
        return;
    if (state->refresh_source_id)
        g_source_remove(state->refresh_source_id);
    mb_gui_snapshot_free(state->snapshot);
    mb_gui_ctl_client_free(state->client);
    g_free(state->selected_id);
    g_free(state);
}

GtkWindow *mb_gui_window_new(AdwApplication *application) {
    install_css();

    MbGuiWindowState *state = g_new0(MbGuiWindowState, 1);
    state->client = mb_gui_ctl_client_new();

    state->window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
    gtk_window_set_title(GTK_WINDOW(state->window), "MIDI-BLE-RT");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1180, 760);
    g_object_set_data_full(G_OBJECT(state->window), "mb-gui-state", state, (GDestroyNotify)state_free);

    GtkWidget *toolbar = adw_toolbar_view_new();
    gtk_widget_add_css_class(toolbar, "app-bg");

    GtkWidget *header = adw_header_bar_new();
    GtkWidget *title = adw_window_title_new("MIDI-BLE-RT", "BLE-MIDI daemon control");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);

    state->daemon_pill = pill_new("Daemon  Offline", "err");
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");

    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), refresh);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), state->daemon_pill);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(content, TRUE);

    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 320, -1);

    GtkWidget *sidebar_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *devices_title = label_new("Devices", "card-title");
    gtk_widget_set_hexpand(devices_title, TRUE);
    GtkWidget *add_button = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_box_append(GTK_BOX(sidebar_header), devices_title);
    gtk_box_append(GTK_BOX(sidebar_header), add_button);
    gtk_box_append(GTK_BOX(sidebar), sidebar_header);

    state->sidebar_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->sidebar_list), GTK_SELECTION_SINGLE);
    gtk_widget_set_vexpand(state->sidebar_list, TRUE);
    gtk_box_append(GTK_BOX(sidebar), state->sidebar_list);

    state->device_count_label = label_new("0 devices", "dim-label");
    gtk_box_append(GTK_BOX(sidebar), state->device_count_label);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(main, "main");
    gtk_widget_set_hexpand(main, TRUE);
    gtk_widget_set_vexpand(main, TRUE);

    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(hero, "card");
    GtkWidget *hero_icon = gtk_image_new_from_icon_name("audio-x-generic-symbolic");
    gtk_widget_set_size_request(hero_icon, 64, 64);
    gtk_box_append(GTK_BOX(hero), hero_icon);

    GtkWidget *hero_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(hero_text, TRUE);
    state->title_name = label_new("No device selected", "title-name");
    state->title_address = label_new("-", "dim-label");
    state->state_pill = pill_new("OFFLINE", "err");
    state->selected_id_label = label_new("-", "dim-label");
    gtk_box_append(GTK_BOX(hero_text), state->title_name);
    gtk_box_append(GTK_BOX(hero_text), state->title_address);
    gtk_box_append(GTK_BOX(hero_text), state->state_pill);
    gtk_box_append(GTK_BOX(hero), hero_text);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *disconnect = gtk_button_new_with_label("Disconnect");
    GtkWidget *recheck = gtk_button_new_with_label("Recheck");
    GtkWidget *connect = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(disconnect, "suggested-action");
    gtk_box_append(GTK_BOX(actions), disconnect);
    gtk_box_append(GTK_BOX(actions), recheck);
    gtk_box_append(GTK_BOX(actions), connect);
    gtk_box_append(GTK_BOX(hero), actions);
    gtk_box_append(GTK_BOX(main), hero);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_widget_set_vexpand(grid, TRUE);

    state->device_card = card_new("Device");
    state->connection_card = card_new("Connection");
    state->alsa_card = card_new("ALSA");
    state->runtime_card = card_new("Runtime");
    state->diagnostics_card = card_new("Diagnostics");

    gtk_grid_attach(GTK_GRID(grid), state->device_card, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->connection_card, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->alsa_card, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->runtime_card, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->diagnostics_card, 1, 1, 2, 1);
    gtk_box_append(GTK_BOX(main), grid);

    GtkWidget *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    state->summary_daemon = summary_tile_new("Daemon\nOffline");
    state->summary_devices = summary_tile_new("Devices\n0 configured");
    state->summary_streaming = summary_tile_new("Streaming\n0 active");
    state->summary_queue = summary_tile_new("Lifecycle Queue\n0 pending");
    gtk_box_append(GTK_BOX(summary), state->summary_daemon);
    gtk_box_append(GTK_BOX(summary), state->summary_devices);
    gtk_box_append(GTK_BOX(summary), state->summary_streaming);
    gtk_box_append(GTK_BOX(summary), state->summary_queue);
    gtk_box_append(GTK_BOX(main), summary);

    gtk_box_append(GTK_BOX(content), sidebar);
    gtk_box_append(GTK_BOX(content), main);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
    adw_application_window_set_content(state->window, toolbar);

    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked_cb), state);
    g_signal_connect(state->sidebar_list, "row-selected", G_CALLBACK(device_selected_cb), state);
    g_signal_connect(connect, "clicked", G_CALLBACK(connect_clicked_cb), state);
    g_signal_connect(disconnect, "clicked", G_CALLBACK(disconnect_clicked_cb), state);
    g_signal_connect(recheck, "clicked", G_CALLBACK(recheck_clicked_cb), state);

    mb_gui_window_refresh(state);
    state->refresh_source_id = g_timeout_add_seconds(REFRESH_INTERVAL_SECONDS, refresh_timer_cb, state);

    return GTK_WINDOW(state->window);
}
