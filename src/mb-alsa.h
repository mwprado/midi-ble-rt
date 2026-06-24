#ifndef MB_ALSA_H
#define MB_ALSA_H

#include <stdbool.h>
#include <alsa/asoundlib.h>

bool mb_alsa_event_is_midi_payload(const snd_seq_event_t *ev);
const char *mb_alsa_event_type_name(int type);

#endif /* MB_ALSA_H */
