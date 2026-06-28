#include "mb-config-dir-alsa.h"

#include <glib.h>

static const char *nonempty_or(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

void mb_config_dir_alsa_close(MbConfigDirAlsa *alsa) {
    if (!alsa)
        return;

    if (alsa->ports) {
        g_array_unref(alsa->ports);
        alsa->ports = NULL;
    }

    if (alsa->seq) {
        snd_seq_close(alsa->seq);
        alsa->seq = NULL;
    }
}

bool mb_config_dir_alsa_open_ports(const MbConfig *cfg, MbConfigDirAlsa *alsa) {
    if (!cfg || !alsa)
        return false;

    int err = snd_seq_open(&alsa->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0)
        return false;

    snd_seq_set_client_name(alsa->seq, nonempty_or(cfg->alsa_client_name, "midi-ble-rt"));
    alsa->ports = g_array_new(FALSE, FALSE, sizeof(int));

    for (unsigned i = 0; i < mb_config_device_count(cfg); i++) {
        const MbDeviceConfig *device = mb_config_get_device(cfg, i);
        if (!device)
            continue;

        int port = snd_seq_create_simple_port(
            alsa->seq,
            nonempty_or(device->alsa_port_name, nonempty_or(device->id, "BLE-MIDI")),
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

        if (port >= 0)
            g_array_append_val(alsa->ports, port);
    }

    if (alsa->ports->len == 0) {
        mb_config_dir_alsa_close(alsa);
        return false;
    }

    return true;
}
