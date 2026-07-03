#include "mb-alsa-port.h"

#include <errno.h>
#include <glib.h>

static const char *nonempty_or(const char *value, const char *fallback) {
    return value && *value ? value : fallback;
}

bool mb_alsa_client_open(snd_seq_t **seq,
                         const char *client_name) {
    if (!seq)
        return false;

    if (*seq)
        return true;

    int err = snd_seq_open(seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0) {
        g_printerr("snd_seq_open(default) failed: %s\n", snd_strerror(err));
        return false;
    }

    snd_seq_set_client_name(*seq, nonempty_or(client_name, "midi-ble-rt"));
    return true;
}

int mb_alsa_port_create_duplex(snd_seq_t *seq,
                               const char *port_name) {
    if (!seq)
        return -EINVAL;

    int port = snd_seq_create_simple_port(
        seq,
        nonempty_or(port_name, "BLE-MIDI"),
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (port < 0)
        g_printerr("snd_seq_create_simple_port failed: %s\n", snd_strerror(port));

    return port;
}

bool mb_alsa_midi_event_pair_new(snd_midi_event_t **encoder,
                                 snd_midi_event_t **decoder) {
    int err = 0;

    if (!encoder || !decoder)
        return false;

    *encoder = NULL;
    *decoder = NULL;

    err = snd_midi_event_new(2048, encoder);
    if (err < 0) {
        g_printerr("snd_midi_event_new encoder failed: %s\n", snd_strerror(err));
        return false;
    }

    err = snd_midi_event_new(2048, decoder);
    if (err < 0) {
        g_printerr("snd_midi_event_new decoder failed: %s\n", snd_strerror(err));
        mb_alsa_midi_event_pair_free(encoder, decoder);
        return false;
    }

    snd_midi_event_init(*encoder);
    snd_midi_event_init(*decoder);
    return true;
}

void mb_alsa_midi_event_pair_free(snd_midi_event_t **encoder,
                                  snd_midi_event_t **decoder) {
    if (decoder && *decoder) {
        snd_midi_event_free(*decoder);
        *decoder = NULL;
    }

    if (encoder && *encoder) {
        snd_midi_event_free(*encoder);
        *encoder = NULL;
    }
}
