#ifndef MB_GUI_CTL_CLIENT_H
#define MB_GUI_CTL_CLIENT_H

#include "mb-gui-model.h"

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef struct _MbGuiCtlClient MbGuiCtlClient;

MbGuiCtlClient *mb_gui_ctl_client_new(void);
void mb_gui_ctl_client_free(MbGuiCtlClient *client);

MbGuiSnapshot *mb_gui_ctl_client_snapshot(MbGuiCtlClient *client);

bool mb_gui_ctl_client_connect_device(MbGuiCtlClient *client,
                                      const char *device_id,
                                      GError **error);

bool mb_gui_ctl_client_disconnect_device(MbGuiCtlClient *client,
                                         const char *device_id,
                                         GError **error);

bool mb_gui_ctl_client_recheck_device(MbGuiCtlClient *client,
                                      const char *device_id,
                                      GError **error);

G_END_DECLS

#endif /* MB_GUI_CTL_CLIENT_H */
