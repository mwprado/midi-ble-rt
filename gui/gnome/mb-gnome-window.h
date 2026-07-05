#ifndef MB_GNOME_WINDOW_H
#define MB_GNOME_WINDOW_H

#include "mb-ui-facade.h"

#include <adwaita.h>

G_BEGIN_DECLS

GtkWindow *mb_gnome_window_new(AdwApplication *application);

G_END_DECLS

#endif /* MB_GNOME_WINDOW_H */
