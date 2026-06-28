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

bool mb_alsa_port_open_duplex(snd_seq_t **seq,
                              int *port,
                              snd_midi_event_t **encoder,
                              snd_midi_event_t **decoder,
                              const char *client_name,
                              const char *port_name) {
    if (!seq || !port || !encoder || !decoder)
        return false;

    if (!mb_alsa_client_open(seq, client_name))
        return false;

    *port = mb_alsa_port_create_duplex(*seq, port_name);
    if (*port < 0)
        return false;

    if (!mb_alsa_midi_event_pair_new(encoder, decoder))
        return false;

    g_print("ALSA Sequencer duplex port created: %s:%s\n",
            nonempty_or(client_name, "midi-ble-rt"),
            nonempty_or(port_name, "BLE-MIDI"));
    g_print("  READ  side: BLE-MIDI -> ALSA clients, e.g. aseqdump -p <client>:<port>\n");
    g_print("  WRITE side: ALSA clients -> BLE-MIDI, e.g. aplaymidi -p <client>:<port> file.mid\n");

    return true;
}

void mb_alsa_port_close(snd_seq_t **seq,
                        snd_midi_event_t **encoder,
                        snd_midi_event_t **decoder) {
    mb_alsa_midi_event_pair_free(encoder, decoder);

    if (seq && *seq) {
        snd_seq_close(*seq);
        *seq = NULL;
    }
}
