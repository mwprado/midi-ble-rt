#ifndef MB_LOG_H
#define MB_LOG_H

#include <glib.h>

G_BEGIN_DECLS

#define MB_LOG_IDENTIFIER "midi-ble-rtd"

typedef struct {
    const char *component;
    const char *event;
    const char *device_address;
    const char *device_path;
    const char *session_state;
    const char *error_name;
} MbLogContext;

void mb_log_event(GLogLevelFlags level,
                  const MbLogContext *ctx,
                  const char *format,
                  ...) G_GNUC_PRINTF(3, 4);

void mb_log_simple(GLogLevelFlags level,
                   const char *component,
                   const char *event,
                   const char *format,
                   ...) G_GNUC_PRINTF(4, 5);

void mb_log_debug_hex(const MbLogContext *ctx,
                      const char *label,
                      const guint8 *bytes,
                      gsize len);

#define MB_LOG_INFO(component, event, ...) \
    mb_log_simple(G_LOG_LEVEL_MESSAGE, component, event, __VA_ARGS__)

#define MB_LOG_WARNING(component, event, ...) \
    mb_log_simple(G_LOG_LEVEL_WARNING, component, event, __VA_ARGS__)

#define MB_LOG_ERROR(component, event, ...) \
    mb_log_simple(G_LOG_LEVEL_CRITICAL, component, event, __VA_ARGS__)

G_END_DECLS

#endif /* MB_LOG_H */
