#ifndef MB_GUI_WINDOW_H
#define MB_GUI_WINDOW_H

#include "mb-gui-ctl-client.h"

#include <adwaita.h>

G_BEGIN_DECLS

GtkWindow *mb_gui_window_new(AdwApplication *application);

G_END_DECLS

#endif /* MB_GUI_WINDOW_H */
