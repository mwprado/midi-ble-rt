#include "mb-rtkit.h"

#include <gio/gio.h>
#include <stdint.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define MB_RTKIT_DBUS_NAME "org.freedesktop.RealtimeKit1"
#define MB_RTKIT_DBUS_PATH "/org/freedesktop/RealtimeKit1"
#define MB_RTKIT_DBUS_IFACE "org.freedesktop.RealtimeKit1"
#define MB_RTKIT_DBUS_TIMEOUT_MS 1000

static gint rtkit_enabled_flag;
static unsigned rtkit_configured_priority = MB_RTKIT_DEFAULT_PRIORITY;
static gint rtkit_warning_emitted;

static void rtkit_warn_once(const char *message, const GError *error) {
    if (g_atomic_int_compare_and_exchange(&rtkit_warning_emitted, 0, 1)) {
        g_printerr("RtKit unavailable: %s", message ? message : "unknown error");
        if (error && error->message)
            g_printerr(": %s", error->message);
        g_printerr(". Continuing without realtime scheduling.\n");
    }
}

static unsigned rtkit_clamp_priority(unsigned priority) {
    if (priority == 0)
        return MB_RTKIT_DEFAULT_PRIORITY;
    if (priority > MB_RTKIT_MAX_PRIORITY)
        return MB_RTKIT_MAX_PRIORITY;
    return priority;
}

void mb_rtkit_configure(bool enabled, unsigned priority) {
    if (!enabled)
        g_atomic_int_set(&rtkit_enabled_flag, 0);

    rtkit_configured_priority = rtkit_clamp_priority(priority);
    g_atomic_int_set(&rtkit_warning_emitted, 0);

    if (enabled)
        g_atomic_int_set(&rtkit_enabled_flag, 1);
}

bool mb_rtkit_enabled(void) {
    return g_atomic_int_get(&rtkit_enabled_flag) != 0;
}

unsigned mb_rtkit_priority(void) {
    return rtkit_configured_priority;
}

#ifdef __linux__
static uint64_t rtkit_current_thread_id(void) {
    return (uint64_t)syscall(SYS_gettid);
}
#else
static uint64_t rtkit_current_thread_id(void) {
    return 0;
}
#endif

void mb_rtkit_make_current_thread_realtime(const char *thread_name) {
    if (!mb_rtkit_enabled())
        return;

#ifdef __linux__
    uint64_t thread_id = rtkit_current_thread_id();
    if (thread_id == 0) {
        rtkit_warn_once("could not resolve current Linux thread id", NULL);
        return;
    }

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        rtkit_warn_once("could not connect to system bus", error);
        g_clear_error(&error);
        return;
    }

    GVariant *reply = g_dbus_connection_call_sync(
        bus,
        MB_RTKIT_DBUS_NAME,
        MB_RTKIT_DBUS_PATH,
        MB_RTKIT_DBUS_IFACE,
        "MakeThreadRealtime",
        g_variant_new("(tu)", (guint64)thread_id, (guint32)rtkit_configured_priority),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        MB_RTKIT_DBUS_TIMEOUT_MS,
        NULL,
        &error);

    if (!reply) {
        rtkit_warn_once("MakeThreadRealtime failed", error);
        g_clear_error(&error);
        g_object_unref(bus);
        return;
    }

    g_variant_unref(reply);
    g_object_unref(bus);

    g_printerr("RtKit realtime scheduling enabled for %s thread=%" G_GUINT64_FORMAT " priority=%u.\n",
               thread_name && *thread_name ? thread_name : "runtime",
               (guint64)thread_id,
               rtkit_configured_priority);
#else
    rtkit_warn_once("RtKit scheduling is only implemented on Linux", NULL);
#endif
}
