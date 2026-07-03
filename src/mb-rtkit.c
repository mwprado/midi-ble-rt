#include "mb-rtkit.h"

#include <gio/gio.h>
#include <stdint.h>

#ifdef __linux__
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define MB_RTKIT_DBUS_NAME "org.freedesktop.RealtimeKit1"
#define MB_RTKIT_DBUS_PATH "/org/freedesktop/RealtimeKit1"
#define MB_RTKIT_DBUS_IFACE "org.freedesktop.RealtimeKit1"
#define MB_RTKIT_DBUS_TIMEOUT_MS 1000
#define MB_RTKIT_RTTIME_USEC 200000u

static gint rtkit_enabled_flag;
static gint rtkit_realtime_rx_flag = 1;
static gint rtkit_realtime_tx_flag;
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

#ifdef __linux__
static void rtkit_warn_errno_once(const char *message) {
    if (g_atomic_int_compare_and_exchange(&rtkit_warning_emitted, 0, 1)) {
        g_printerr("RtKit unavailable: %s: %s. Continuing without realtime scheduling.\n",
                   message ? message : "unknown errno error",
                   g_strerror(errno));
    }
}
#endif

static unsigned rtkit_clamp_priority(unsigned priority) {
    if (priority == 0)
        return MB_RTKIT_DEFAULT_PRIORITY;
    if (priority > MB_RTKIT_MAX_PRIORITY)
        return MB_RTKIT_MAX_PRIORITY;
    return priority;
}

void mb_rtkit_configure(bool enabled,
                        unsigned priority,
                        bool realtime_rx,
                        bool realtime_tx) {
    if (!enabled)
        g_atomic_int_set(&rtkit_enabled_flag, 0);

    rtkit_configured_priority = rtkit_clamp_priority(priority);
    g_atomic_int_set(&rtkit_realtime_rx_flag, realtime_rx ? 1 : 0);
    g_atomic_int_set(&rtkit_realtime_tx_flag, realtime_tx ? 1 : 0);
    g_atomic_int_set(&rtkit_warning_emitted, 0);

    if (enabled)
        g_atomic_int_set(&rtkit_enabled_flag, 1);
}

bool mb_rtkit_enabled(void) {
    return g_atomic_int_get(&rtkit_enabled_flag) != 0;
}

bool mb_rtkit_realtime_rx_enabled(void) {
    return mb_rtkit_enabled() && g_atomic_int_get(&rtkit_realtime_rx_flag) != 0;
}

bool mb_rtkit_realtime_tx_enabled(void) {
    return mb_rtkit_enabled() && g_atomic_int_get(&rtkit_realtime_tx_flag) != 0;
}

unsigned mb_rtkit_priority(void) {
    return rtkit_configured_priority;
}

#ifdef __linux__
static uint64_t rtkit_current_thread_id(void) {
    return (uint64_t)syscall(SYS_gettid);
}

static bool rtkit_ensure_rttime_limit(void) {
#ifdef RLIMIT_RTTIME
    struct rlimit current;
    if (getrlimit(RLIMIT_RTTIME, &current) != 0) {
        rtkit_warn_errno_once("could not read RLIMIT_RTTIME");
        return false;
    }

    if (current.rlim_cur != RLIM_INFINITY && current.rlim_cur <= MB_RTKIT_RTTIME_USEC)
        return true;

    rlim_t desired = MB_RTKIT_RTTIME_USEC;
    if (current.rlim_max != RLIM_INFINITY && current.rlim_max < desired)
        desired = current.rlim_max;

    if (desired == 0) {
        rtkit_warn_once("RLIMIT_RTTIME hard limit is zero", NULL);
        return false;
    }

    struct rlimit updated = current;
    updated.rlim_cur = desired;

    if (setrlimit(RLIMIT_RTTIME, &updated) != 0) {
        rtkit_warn_errno_once("could not set RLIMIT_RTTIME");
        return false;
    }

    return true;
#else
    rtkit_warn_once("RLIMIT_RTTIME is not available on this platform", NULL);
    return false;
#endif
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
    if (!rtkit_ensure_rttime_limit())
        return;

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

    g_printerr("RtKit realtime scheduling enabled for %s thread=%" G_GUINT64_FORMAT " priority=%u rttime_usec=%u.\n",
               thread_name && *thread_name ? thread_name : "runtime",
               (guint64)thread_id,
               rtkit_configured_priority,
               MB_RTKIT_RTTIME_USEC);
#else
    rtkit_warn_once("RtKit scheduling is only implemented on Linux", NULL);
#endif
}
