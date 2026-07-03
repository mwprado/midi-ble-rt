#ifndef MB_RTKIT_H
#define MB_RTKIT_H

#include <stdbool.h>

#include <glib.h>

G_BEGIN_DECLS

#define MB_RTKIT_DEFAULT_PRIORITY 10u
#define MB_RTKIT_MAX_PRIORITY 20u

void mb_rtkit_configure(bool enabled, unsigned priority);
bool mb_rtkit_enabled(void);
unsigned mb_rtkit_priority(void);
void mb_rtkit_make_current_thread_realtime(const char *thread_name);

G_END_DECLS

#endif /* MB_RTKIT_H */
