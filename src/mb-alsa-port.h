#ifndef MB_ALSA_PORT_H
#define MB_ALSA_PORT_H

#include <alsa/asoundlib.h>
#include <stdbool.h>

/*
 * ALSA Sequencer port lifecycle.
 *
 * This module owns only opening the duplex Sequencer client/port and creating
 * MIDI encoder/decoder handles. Event polling, MIDI encode/decode policy and
 * runtime routing remain outside this module.
 */

bool mb_alsa_port_open_duplex(snd_seq_t **seq,
                              int *port,
                              snd_midi_event_t **encoder,
                              snd_midi_event_t **decoder,
                              const char *client_name,
                              const char *port_name);

void mb_alsa_port_close(snd_seq_t **seq,
                        snd_midi_event_t **encoder,
                        snd_midi_event_t **decoder);

#endif /* MB_ALSA_PORT_H */
