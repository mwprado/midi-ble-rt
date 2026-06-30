#ifndef MB_CONTROL_PROTOCOL_H
#define MB_CONTROL_PROTOCOL_H

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef enum {
    MB_CONTROL_CMD_UNKNOWN = 0,
    MB_CONTROL_CMD_PING,
    MB_CONTROL_CMD_STATUS,
    MB_CONTROL_CMD_LIST,
    MB_CONTROL_CMD_CONNECT,
    MB_CONTROL_CMD_DISCONNECT,
    MB_CONTROL_CMD_RECHECK
} MbControlCommandType;

typedef struct {
    MbControlCommandType type;
    char *device;
} MbControlCommand;

const char *mb_control_command_name(MbControlCommandType type);
void mb_control_command_clear(MbControlCommand *command);
bool mb_control_command_parse(const char *line, MbControlCommand *out_command);

G_END_DECLS

#endif /* MB_CONTROL_PROTOCOL_H */
