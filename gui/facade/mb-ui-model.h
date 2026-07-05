#ifndef MB_UI_MODEL_H
#define MB_UI_MODEL_H

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef struct {
    bool online;
    unsigned devices;
    unsigned streaming;
    unsigned rx_workers;
    unsigned tx_workers;
    unsigned rtkit_priority;
    unsigned lifecycle_queue;
    char *alsa_tx_thread;
    char *rtkit;
    char *rtkit_rx;
    char *rtkit_tx;
    char *lifecycle_busy;
} MbUiServiceStatus;

typedef struct {
    char *id;
    char *address;
    char *name;
    char *state;
    int alsa_port;

    /*
     * BlueZ/discovery metadata.
     *
     * For imported devices these fields may be false/empty unless overlaid
     * from discovery. For scan snapshots they drive the Add Instrument dialog.
     */
    bool paired;
    bool trusted;
    bool connected;
    bool gatt_resolved;
    bool imported;
    char *profile;
} MbUiDevice;

typedef struct {
    MbUiServiceStatus status;
    GPtrArray *devices; /* element type: MbUiDevice* */
    char *last_error;
} MbUiSnapshot;

void mb_ui_service_status_clear(MbUiServiceStatus *status);
MbUiDevice *mb_ui_device_new(const char *id,
                               const char *address,
                               const char *name,
                               const char *state,
                               int alsa_port);
void mb_ui_device_free(MbUiDevice *device);

MbUiSnapshot *mb_ui_snapshot_new(void);
void mb_ui_snapshot_free(MbUiSnapshot *snapshot);

const MbUiDevice *mb_ui_snapshot_get_device(const MbUiSnapshot *snapshot,
                                              guint index);
const MbUiDevice *mb_ui_snapshot_find_device(const MbUiSnapshot *snapshot,
                                               const char *id_or_address);

G_END_DECLS

#endif /* MB_UI_MODEL_H */
