#ifndef MB_BLUEZ_AGENT_H
#define MB_BLUEZ_AGENT_H

#include <gio/gio.h>
#include <stdbool.h>

typedef struct {
    GDBusConnection *bus;
    GDBusNodeInfo *node_info;
    guint registration_id;
    bool registered_with_bluez;
} MbBluezAgent;

bool mb_bluez_agent_start(MbBluezAgent *agent,
                          GDBusConnection *system_bus,
                          GError **error);
void mb_bluez_agent_stop(MbBluezAgent *agent);

#endif /* MB_BLUEZ_AGENT_H */
