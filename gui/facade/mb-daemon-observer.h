#ifndef MB_DAEMON_OBSERVER_H
#define MB_DAEMON_OBSERVER_H

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef struct _MbDaemonObserver MbDaemonObserver;

typedef void (*MbDaemonObserverCallback)(bool active,
                                         const char *active_state,
                                         const char *sub_state,
                                         void *user_data);

MbDaemonObserver *mb_daemon_observer_new(void);
void mb_daemon_observer_free(MbDaemonObserver *observer);

bool mb_daemon_observer_start(MbDaemonObserver *observer,
                              MbDaemonObserverCallback callback,
                              void *user_data,
                              GError **error);

bool mb_daemon_observer_refresh(MbDaemonObserver *observer,
                                GError **error);

bool mb_daemon_observer_is_active(const MbDaemonObserver *observer);
const char *mb_daemon_observer_active_state(const MbDaemonObserver *observer);
const char *mb_daemon_observer_sub_state(const MbDaemonObserver *observer);

G_END_DECLS

#endif /* MB_DAEMON_OBSERVER_H */
