#include "mb-config.h"

#include "mb-ble-midi.h"

#include <glib.h>
#include <string.h>

static char *keyfile_get_string_default(GKeyFile *kf, const char *group, const char *key, const char *fallback) {
    GError *error = NULL;
    char *value = g_key_file_get_string(kf, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return g_strdup(fallback);
    }
    return value;
}

static bool keyfile_get_bool_default(GKeyFile *kf, const char *group, const char *key, bool fallback) {
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(kf, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return fallback;
    }
    return value;
}

static void mb_config_load_defaults_from_key_file(MbConfig *cfg, GKeyFile *kf) {
    cfg->address = keyfile_get_string_default(kf, "device", "address", "");
    cfg->name = keyfile_get_string_default(kf, "device", "name", "");
    cfg->pair = keyfile_get_bool_default(kf, "device", "pair", false);
    cfg->trust = keyfile_get_bool_default(kf, "device", "trust", true);
    cfg->auto_reconnect = keyfile_get_bool_default(kf, "device", "auto_reconnect", true);

    cfg->service_uuid = keyfile_get_string_default(kf, "gatt", "service_uuid",
        MB_BLE_MIDI_SERVICE_UUID);
    cfg->io_uuid = keyfile_get_string_default(kf, "gatt", "io_uuid",
        MB_BLE_MIDI_IO_UUID);
    cfg->io_uuid_alias = keyfile_get_string_default(kf, "gatt", "io_uuid_alias",
        MB_BLE_MIDI_ROLAND_IO_UUID_ALIAS);
    cfg->require_notify = keyfile_get_bool_default(kf, "gatt", "require_notify", true);
    cfg->require_write_without_response = keyfile_get_bool_default(kf, "gatt", "require_write_without_response", true);

    cfg->alsa_client_name = keyfile_get_string_default(kf, "alsa", "client_name", "midi-ble-rt");
    cfg->alsa_port_name = keyfile_get_string_default(kf, "alsa", "port_name", "BLE-MIDI In");

    cfg->print_ble_packets = keyfile_get_bool_default(kf, "debug", "print_ble_packets", false);
    cfg->print_midi_events = keyfile_get_bool_default(kf, "debug", "print_midi_events", false);
    cfg->enable_tx = keyfile_get_bool_default(kf, "midi", "enable_tx", true);
}

bool mb_config_load(MbConfig *cfg, const char *path) {
    if (!cfg || !path)
        return false;

    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
        g_printerr("Failed to load config %s: %s\n", path, error->message);
        g_clear_error(&error);
        g_key_file_unref(kf);
        return false;
    }

    MbConfig tmp;
    memset(&tmp, 0, sizeof(tmp));
    mb_config_load_defaults_from_key_file(&tmp, kf);
    g_key_file_unref(kf);

    mb_config_clear(cfg);
    *cfg = tmp;
    return true;
}

void mb_config_clear(MbConfig *cfg) {
    if (!cfg)
        return;

    g_free(cfg->address);
    g_free(cfg->name);
    g_free(cfg->service_uuid);
    g_free(cfg->io_uuid);
    g_free(cfg->io_uuid_alias);
    g_free(cfg->alsa_client_name);
    g_free(cfg->alsa_port_name);

    memset(cfg, 0, sizeof(*cfg));
}
