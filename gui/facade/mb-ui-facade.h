#ifndef MB_UI_FACADE_H
#define MB_UI_FACADE_H

#include "mb-ui-model.h"

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef struct _MbUiFacade MbUiFacade;

typedef struct {
    bool connect_on_start;
    bool reconnect_on_link_loss;
    bool enable_tx;
    bool pair;
    bool trust;
} MbUiDeviceConfig;


void mb_ui_device_config_defaults(MbUiDeviceConfig *config);

MbUiFacade *mb_ui_facade_new(void);
void mb_ui_facade_free(MbUiFacade *facade);

MbUiSnapshot *mb_ui_facade_get_snapshot(MbUiFacade *facade);
MbUiSnapshot *mb_ui_facade_get_scan_snapshot(MbUiFacade *facade);

bool mb_ui_facade_scan_devices(MbUiFacade *facade,
                               unsigned timeout_seconds,
                               GError **error);

bool mb_ui_facade_scan(MbUiFacade *facade, GError **error);

bool mb_ui_facade_connect(MbUiFacade *facade,
                          const char *device_id,
                          GError **error);


bool mb_ui_facade_pair_scanned_device(MbUiFacade *facade,
                                      const char *device_id,
                                      GError **error);

bool mb_ui_facade_import_scanned_device(MbUiFacade *facade,
                                         const char *device_id,
                                         GError **error);

bool mb_ui_facade_connect_with_config(MbUiFacade *facade,
                                      const char *device_id,
                                      const MbUiDeviceConfig *config,
                                      GError **error);

bool mb_ui_facade_disconnect(MbUiFacade *facade,
                             const char *device_id,
                             GError **error);

bool mb_ui_facade_refresh_device(MbUiFacade *facade,
                                 const char *device_id,
                                 GError **error);

bool mb_ui_facade_forget_device(MbUiFacade *facade,
                                const char *device_id,
                                GError **error);

bool mb_ui_facade_save_device_config(MbUiFacade *facade,
                                     const MbUiDevice *device,
                                     GError **error);

G_END_DECLS

#endif /* MB_UI_FACADE_H */
