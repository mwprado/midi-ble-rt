#ifndef MB_LEGACY_CORE_H
#define MB_LEGACY_CORE_H

/*
 * Compatibility shim for names that were previously provided by including the
 * old monolithic midi-ble-rtd.c implementation.
 *
 * This header deliberately no longer includes any .c file.  It exists only to
 * keep the orchestrator source stable while the final symbol names are migrated
 * from legacy short names to the mb_app_* API.
 */

#include "mb-app.h"

static inline bool get_device_bool_property(App *app, const char *property, bool *out) {
    return mb_app_get_device_bool_property(app, property, out);
}

static inline char *find_device(App *app) {
    return mb_app_find_device(app);
}

static inline bool set_device_trusted(App *app) {
    return mb_app_set_device_trusted(app);
}

static inline bool pair_device(App *app) {
    return mb_app_pair_device(app);
}

static inline bool connect_device(App *app) {
    return mb_app_connect_device(app);
}

static inline bool wait_services_resolved(App *app, int timeout_ms) {
    return mb_app_wait_services_resolved(app, timeout_ms);
}

static inline char *find_ble_midi_service(App *app) {
    return mb_app_find_ble_midi_service(app);
}

static inline char *find_ble_midi_characteristic(App *app) {
    return mb_app_find_ble_midi_characteristic(app);
}

static inline bool alsa_init(App *app) {
    return mb_app_alsa_init(app);
}

static inline void ble_midi_decode_packet(App *app, const uint8_t *p, size_t len) {
    mb_app_ble_midi_decode_packet(app, p, len);
}

static inline void print_midi_bytes(const char *prefix, const uint8_t *bytes, size_t len) {
    mb_app_print_midi_bytes(prefix, bytes, len);
}

static inline void app_cleanup(App *app) {
    mb_app_cleanup(app);
}

#endif /* MB_LEGACY_CORE_H */
