#define G_LOG_DOMAIN "midi-ble-rtd"

#include "mb-log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *priority_for_level(GLogLevelFlags level) {
    if (level & G_LOG_LEVEL_ERROR)
        return "3";
    if (level & G_LOG_LEVEL_CRITICAL)
        return "3";
    if (level & G_LOG_LEVEL_WARNING)
        return "4";
    if (level & G_LOG_LEVEL_MESSAGE)
        return "5";
    if (level & G_LOG_LEVEL_INFO)
        return "6";
    if (level & G_LOG_LEVEL_DEBUG)
        return "7";
    return "5";
}

static void add_field(GLogField *fields,
                      gsize *n_fields,
                      const char *key,
                      const char *value) {
    if (!value || !*value)
        return;

    fields[*n_fields].key = key;
    fields[*n_fields].value = value;
    fields[*n_fields].length = -1;
    (*n_fields)++;
}

static void mb_log_emit(GLogLevelFlags level,
                        const MbLogContext *ctx,
                        const char *message) {
    GLogField fields[12];
    gsize n_fields = 0;

    add_field(fields, &n_fields, "MESSAGE", message ? message : "");
    add_field(fields, &n_fields, "PRIORITY", priority_for_level(level));
    add_field(fields, &n_fields, "GLIB_DOMAIN", G_LOG_DOMAIN);
    add_field(fields, &n_fields, "SYSLOG_IDENTIFIER", MB_LOG_IDENTIFIER);

    if (ctx) {
        add_field(fields, &n_fields, "MIDI_BLE_RT_COMPONENT", ctx->component);
        add_field(fields, &n_fields, "MIDI_BLE_RT_EVENT", ctx->event);
        add_field(fields, &n_fields, "MIDI_BLE_RT_DEVICE_ADDRESS", ctx->device_address);
        add_field(fields, &n_fields, "MIDI_BLE_RT_DEVICE_PATH", ctx->device_path);
        add_field(fields, &n_fields, "MIDI_BLE_RT_SESSION_STATE", ctx->session_state);
        add_field(fields, &n_fields, "MIDI_BLE_RT_ERROR", ctx->error_name);
    }

    g_log_structured_array(level, fields, n_fields);
}

void mb_log_event(GLogLevelFlags level,
                  const MbLogContext *ctx,
                  const char *format,
                  ...) {
    va_list ap;
    va_start(ap, format);
    char *message = g_strdup_vprintf(format, ap);
    va_end(ap);

    mb_log_emit(level, ctx, message);
    g_free(message);
}

void mb_log_simple(GLogLevelFlags level,
                   const char *component,
                   const char *event,
                   const char *format,
                   ...) {
    MbLogContext ctx = {
        .component = component,
        .event = event,
    };

    va_list ap;
    va_start(ap, format);
    char *message = g_strdup_vprintf(format, ap);
    va_end(ap);

    mb_log_emit(level, &ctx, message);
    g_free(message);
}

void mb_log_debug_hex(const MbLogContext *ctx,
                      const char *label,
                      const guint8 *bytes,
                      gsize len) {
    if (!bytes && len > 0)
        return;

    GString *s = g_string_new(label && *label ? label : "bytes");
    g_string_append_c(s, ':');

    for (gsize i = 0; i < len; i++)
        g_string_append_printf(s, " %02x", bytes[i]);

    mb_log_emit(G_LOG_LEVEL_DEBUG, ctx, s->str);
    g_string_free(s, TRUE);
}
