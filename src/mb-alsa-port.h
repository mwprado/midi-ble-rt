#ifndef MB_ALSA_PORT_H
#define MB_ALSA_PORT_H

#include <alsa/asoundlib.h>
#include <stdbool.h>

/*
 * ALSA Sequencer lifecycle helpers.
 *
 * The daemon opens one shared ALSA client and creates one duplex port per
 * configured BLE-MIDI device. Event polling, MIDI encode/decode policy and
 * runtime routing remain outside this module.
 */

bool mb_alsa_client_open(snd_seq_t **seq,
                         const char *client_name);

int mb_alsa_port_create_duplex(snd_seq_t *seq,
                               const char *port_name);

bool mb_alsa_midi_event_pair_new(snd_midi_event_t **encoder,
                                 snd_midi_event_t **decoder);

void mb_alsa_midi_event_pair_free(snd_midi_event_t **encoder,
                                  snd_midi_event_t **decoder);

#endif /* MB_ALSA_PORT_H */
