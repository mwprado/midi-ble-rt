#include "mb-gui-model.h"

#include <stdlib.h>
#include <string.h>

void mb_gui_daemon_status_clear(MbGuiDaemonStatus *status) {
    if (!status)
        return;

    g_free(status->alsa_tx_thread);
    g_free(status->rtkit);
    g_free(status->rtkit_rx);
    g_free(status->rtkit_tx);
    g_free(status->lifecycle_busy);
    memset(status, 0, sizeof(*status));
}

MbGuiDevice *mb_gui_device_new(const char *id,
                               const char *address,
                               const char *name,
                               const char *state,
                               int alsa_port) {
    MbGuiDevice *device = g_new0(MbGuiDevice, 1);
    device->id = g_strdup(id && *id ? id : "-");
    device->address = g_strdup(address && *address ? address : "-");
    device->name = g_strdup(name && *name ? name : device->id);
    device->state = g_strdup(state && *state ? state : "UNKNOWN");
    device->alsa_port = alsa_port;
    return device;
}

void mb_gui_device_free(MbGuiDevice *device) {
    if (!device)
        return;

    g_free(device->id);
    g_free(device->address);
    g_free(device->name);
    g_free(device->state);
    g_free(device);
}

MbGuiSnapshot *mb_gui_snapshot_new(void) {
    MbGuiSnapshot *snapshot = g_new0(MbGuiSnapshot, 1);
    snapshot->devices = g_ptr_array_new_with_free_func((GDestroyNotify)mb_gui_device_free);
    return snapshot;
}

void mb_gui_snapshot_free(MbGuiSnapshot *snapshot) {
    if (!snapshot)
        return;

    mb_gui_daemon_status_clear(&snapshot->status);
    if (snapshot->devices)
        g_ptr_array_free(snapshot->devices, TRUE);
    g_free(snapshot->last_error);
    g_free(snapshot);
}

const MbGuiDevice *mb_gui_snapshot_get_device(const MbGuiSnapshot *snapshot,
                                              guint index) {
    if (!snapshot || !snapshot->devices || index >= snapshot->devices->len)
        return NULL;
    return g_ptr_array_index(snapshot->devices, index);
}

const MbGuiDevice *mb_gui_snapshot_find_device(const MbGuiSnapshot *snapshot,
                                               const char *id_or_address) {
    if (!snapshot || !snapshot->devices || !id_or_address || !*id_or_address)
        return NULL;

    for (guint i = 0; i < snapshot->devices->len; i++) {
        MbGuiDevice *device = g_ptr_array_index(snapshot->devices, i);
        if (!device)
            continue;
        if (g_strcmp0(device->id, id_or_address) == 0 ||
            g_strcmp0(device->address, id_or_address) == 0)
            return device;
    }

    return NULL;
}
