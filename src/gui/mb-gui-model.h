#ifndef MB_GUI_MODEL_H
#define MB_GUI_MODEL_H

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
} MbGuiDaemonStatus;

typedef struct {
    char *id;
    char *address;
    char *name;
    char *state;
    int alsa_port;
} MbGuiDevice;

typedef struct {
    MbGuiDaemonStatus status;
    GPtrArray *devices; /* element type: MbGuiDevice* */
    char *last_error;
} MbGuiSnapshot;

void mb_gui_daemon_status_clear(MbGuiDaemonStatus *status);
MbGuiDevice *mb_gui_device_new(const char *id,
                               const char *address,
                               const char *name,
                               const char *state,
                               int alsa_port);
void mb_gui_device_free(MbGuiDevice *device);

MbGuiSnapshot *mb_gui_snapshot_new(void);
void mb_gui_snapshot_free(MbGuiSnapshot *snapshot);

const MbGuiDevice *mb_gui_snapshot_get_device(const MbGuiSnapshot *snapshot,
                                              guint index);
const MbGuiDevice *mb_gui_snapshot_find_device(const MbGuiSnapshot *snapshot,
                                               const char *id_or_address);

G_END_DECLS

#endif /* MB_GUI_MODEL_H */
