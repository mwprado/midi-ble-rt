#ifndef MB_GUI_DAEMON_CLIENT_H
#define MB_GUI_DAEMON_CLIENT_H

#include "mb-gui-model.h"

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MbGuiDaemonClient MbGuiDaemonClient;

MbGuiDaemonClient *mb_gui_daemon_client_new(void);
void mb_gui_daemon_client_free(MbGuiDaemonClient *client);

MbGuiSnapshot *mb_gui_daemon_client_snapshot(MbGuiDaemonClient *client);
bool mb_gui_daemon_client_connect_device(MbGuiDaemonClient *client,
                                         const char *device_id,
                                         GError **error);
bool mb_gui_daemon_client_disconnect_device(MbGuiDaemonClient *client,
                                            const char *device_id,
                                            GError **error);
bool mb_gui_daemon_client_recheck_device(MbGuiDaemonClient *client,
                                         const char *device_id,
                                         GError **error);

G_END_DECLS

#endif /* MB_GUI_DAEMON_CLIENT_H */
