#ifndef MB_ALSA_PORT_H
#define MB_ALSA_PORT_H

#include <alsa/asoundlib.h>
#include <stdbool.h>

/*
 * ALSA Sequencer lifecycle helpers.
 *
 * Single-device mode can still use mb_alsa_port_open_duplex(), which opens one
 * client and one duplex port.  Multi-device mode should open one shared client
 * with mb_alsa_client_open() and then create one duplex port per session with
 * mb_alsa_port_create_duplex().
 *
 * Event polling, MIDI encode/decode policy and runtime routing remain outside
 * this module.
 */

bool mb_alsa_client_open(snd_seq_t **seq,
                         const char *client_name);

int mb_alsa_port_create_duplex(snd_seq_t *seq,
                               const char *port_name);

bool mb_alsa_midi_event_pair_new(snd_midi_event_t **encoder,
                                 snd_midi_event_t **decoder);

void mb_alsa_midi_event_pair_free(snd_midi_event_t **encoder,
                                  snd_midi_event_t **decoder);

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
