#include "mb-alsa-port.h"

#include <glib.h>

bool mb_alsa_port_open_duplex(snd_seq_t **seq,
                              int *port,
                              snd_midi_event_t **encoder,
                              snd_midi_event_t **decoder,
                              const char *client_name,
                              const char *port_name) {
    int err = snd_seq_open(seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (err < 0) {
        g_printerr("snd_seq_open(default) failed: %s\n", snd_strerror(err));
        return false;
    }

    snd_seq_set_client_name(*seq, client_name);

    *port = snd_seq_create_simple_port(
        *seq,
        port_name,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (*port < 0) {
        g_printerr("snd_seq_create_simple_port failed: %s\n", snd_strerror(*port));
        return false;
    }

    err = snd_midi_event_new(2048, encoder);
    if (err < 0) {
        g_printerr("snd_midi_event_new encoder failed: %s\n", snd_strerror(err));
        return false;
    }

    err = snd_midi_event_new(2048, decoder);
    if (err < 0) {
        g_printerr("snd_midi_event_new decoder failed: %s\n", snd_strerror(err));
        return false;
    }

    snd_midi_event_init(*encoder);
    snd_midi_event_init(*decoder);

    g_print("ALSA Sequencer duplex port created: %s:%s\n",
            client_name, port_name);
    g_print("  READ  side: BLE-MIDI -> ALSA clients, e.g. aseqdump -p <client>:<port>\n");
    g_print("  WRITE side: ALSA clients -> BLE-MIDI, e.g. aplaymidi -p <client>:<port> file.mid\n");

    return true;
}
