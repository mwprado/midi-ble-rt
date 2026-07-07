#include "mb-ui-model.h"

#include <stdlib.h>
#include <string.h>

void mb_ui_service_status_clear(MbUiServiceStatus *status) {
    if (!status)
        return;

    g_free(status->alsa_tx_thread);
    g_free(status->rtkit);
    g_free(status->rtkit_rx);
    g_free(status->rtkit_tx);
    g_free(status->lifecycle_busy);
    memset(status, 0, sizeof(*status));
}

MbUiDevice *mb_ui_device_new(const char *id,
                               const char *address,
                               const char *name,
                               const char *state,
                               int alsa_port) {
    MbUiDevice *device = g_new0(MbUiDevice, 1);
    device->id = g_strdup(id && *id ? id : "-");
    device->address = g_strdup(address && *address ? address : "-");
    device->name = g_strdup(name && *name ? name : device->id);
    device->state = g_strdup(state && *state ? state : "UNKNOWN");
    device->alsa_port = alsa_port;
    return device;
}

void mb_ui_device_free(MbUiDevice *device) {
    if (!device)
        return;

    g_free(device->id);
    g_free(device->address);
    g_free(device->name);
    g_free(device->state);
    g_free(device->profile);
    g_free(device->config_file);
    g_free(device->enabled);
    g_free(device->connect_on_start);
    g_free(device->alsa_port_name);
    g_free(device->policy_pair);
    g_free(device->policy_trust);
    g_free(device->policy_reconnect_on_link_loss);
    g_free(device->midi_enable_tx);
    g_free(device->gatt_service_uuid);
    g_free(device->gatt_io_uuid);
    g_free(device->gatt_io_uuid_alias);
    g_free(device);
}

MbUiSnapshot *mb_ui_snapshot_new(void) {
    MbUiSnapshot *snapshot = g_new0(MbUiSnapshot, 1);
    snapshot->devices = g_ptr_array_new_with_free_func((GDestroyNotify)mb_ui_device_free);
    return snapshot;
}

void mb_ui_snapshot_free(MbUiSnapshot *snapshot) {
    if (!snapshot)
        return;

    mb_ui_service_status_clear(&snapshot->status);
    if (snapshot->devices)
        g_ptr_array_free(snapshot->devices, TRUE);
    g_free(snapshot->last_error);
    g_free(snapshot);
}

const MbUiDevice *mb_ui_snapshot_get_device(const MbUiSnapshot *snapshot,
                                              guint index) {
    if (!snapshot || !snapshot->devices || index >= snapshot->devices->len)
        return NULL;
    return g_ptr_array_index(snapshot->devices, index);
}

const MbUiDevice *mb_ui_snapshot_find_device(const MbUiSnapshot *snapshot,
                                               const char *id_or_address) {
    if (!snapshot || !snapshot->devices || !id_or_address || !*id_or_address)
        return NULL;

    for (guint i = 0; i < snapshot->devices->len; i++) {
        MbUiDevice *device = g_ptr_array_index(snapshot->devices, i);
        if (!device)
            continue;
        if (g_strcmp0(device->id, id_or_address) == 0 ||
            g_strcmp0(device->address, id_or_address) == 0)
            return device;
    }

    return NULL;
}
