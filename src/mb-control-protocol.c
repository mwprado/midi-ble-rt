#include "mb-control-protocol.h"

#include <string.h>

const char *mb_control_command_name(MbControlCommandType type) {
    switch (type) {
        case MB_CONTROL_CMD_PING: return "PING";
        case MB_CONTROL_CMD_STATUS: return "STATUS";
        case MB_CONTROL_CMD_LIST: return "LIST";
        case MB_CONTROL_CMD_CONNECT: return "CONNECT";
        case MB_CONTROL_CMD_DISCONNECT: return "DISCONNECT";
        case MB_CONTROL_CMD_RECHECK: return "RECHECK";
        case MB_CONTROL_CMD_UNKNOWN:
        default: return "UNKNOWN";
    }
}

void mb_control_command_clear(MbControlCommand *command) {
    if (!command)
        return;

    g_free(command->device);
    memset(command, 0, sizeof(*command));
}

static MbControlCommandType command_type_from_token(const char *token) {
    if (g_ascii_strcasecmp(token, "PING") == 0)
        return MB_CONTROL_CMD_PING;
    if (g_ascii_strcasecmp(token, "STATUS") == 0)
        return MB_CONTROL_CMD_STATUS;
    if (g_ascii_strcasecmp(token, "LIST") == 0)
        return MB_CONTROL_CMD_LIST;
    if (g_ascii_strcasecmp(token, "CONNECT") == 0)
        return MB_CONTROL_CMD_CONNECT;
    if (g_ascii_strcasecmp(token, "DISCONNECT") == 0)
        return MB_CONTROL_CMD_DISCONNECT;
    if (g_ascii_strcasecmp(token, "RECHECK") == 0)
        return MB_CONTROL_CMD_RECHECK;

    return MB_CONTROL_CMD_UNKNOWN;
}

static bool command_requires_device(MbControlCommandType type) {
    return type == MB_CONTROL_CMD_CONNECT ||
           type == MB_CONTROL_CMD_DISCONNECT ||
           type == MB_CONTROL_CMD_RECHECK;
}

bool mb_control_command_parse(const char *line, MbControlCommand *out_command) {
    if (!out_command)
        return false;

    memset(out_command, 0, sizeof(*out_command));

    if (!line)
        return false;

    char *copy = g_strdup(line);
    g_strstrip(copy);

    if (*copy == '\0') {
        g_free(copy);
        return false;
    }

    char **tokens = g_strsplit_set(copy, " \t", 0);
    g_free(copy);

    if (!tokens || !tokens[0] || !*tokens[0]) {
        g_strfreev(tokens);
        return false;
    }

    MbControlCommandType type = command_type_from_token(tokens[0]);
    if (type == MB_CONTROL_CMD_UNKNOWN) {
        g_strfreev(tokens);
        return false;
    }

    const char *device = NULL;
    for (gsize i = 1; tokens[i]; i++) {
        if (!*tokens[i])
            continue;
        if (!device) {
            device = tokens[i];
            continue;
        }

        g_strfreev(tokens);
        return false;
    }

    if (command_requires_device(type) && (!device || !*device)) {
        g_strfreev(tokens);
        return false;
    }

    if (!command_requires_device(type) && device) {
        g_strfreev(tokens);
        return false;
    }

    out_command->type = type;
    out_command->device = device ? g_strdup(device) : NULL;

    g_strfreev(tokens);
    return true;
}
