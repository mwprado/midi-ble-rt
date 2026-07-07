#ifndef MB_BLUEZ_OBSERVER_H
#define MB_BLUEZ_OBSERVER_H

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef struct _MbBluezObserver MbBluezObserver;

typedef void (*MbBluezObserverCallback)(bool available,
                                        bool powered_known,
                                        bool powered,
                                        bool discovering,
                                        const char *adapter_path,
                                        void *user_data);

MbBluezObserver *mb_bluez_observer_new(void);
void mb_bluez_observer_free(MbBluezObserver *observer);

bool mb_bluez_observer_start(MbBluezObserver *observer,
                             MbBluezObserverCallback callback,
                             void *user_data,
                             GError **error);

bool mb_bluez_observer_refresh(MbBluezObserver *observer,
                               GError **error);

bool mb_bluez_observer_is_available(const MbBluezObserver *observer);
bool mb_bluez_observer_is_powered_known(const MbBluezObserver *observer);
bool mb_bluez_observer_is_powered(const MbBluezObserver *observer);
bool mb_bluez_observer_is_discovering(const MbBluezObserver *observer);
bool mb_bluez_observer_can_scan(const MbBluezObserver *observer);
const char *mb_bluez_observer_adapter_path(const MbBluezObserver *observer);

G_END_DECLS

#endif /* MB_BLUEZ_OBSERVER_H */
