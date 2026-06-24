#include "mb-alsa.h"

bool mb_alsa_event_is_midi_payload(const snd_seq_event_t *ev) {
    if (!ev)
        return false;

    switch (ev->type) {
        case SND_SEQ_EVENT_NOTE:
        case SND_SEQ_EVENT_NOTEON:
        case SND_SEQ_EVENT_NOTEOFF:
        case SND_SEQ_EVENT_KEYPRESS:
        case SND_SEQ_EVENT_CONTROLLER:
        case SND_SEQ_EVENT_PGMCHANGE:
        case SND_SEQ_EVENT_CHANPRESS:
        case SND_SEQ_EVENT_PITCHBEND:
        case SND_SEQ_EVENT_CONTROL14:
        case SND_SEQ_EVENT_NONREGPARAM:
        case SND_SEQ_EVENT_REGPARAM:
        case SND_SEQ_EVENT_SONGPOS:
        case SND_SEQ_EVENT_SONGSEL:
        case SND_SEQ_EVENT_QFRAME:
        case SND_SEQ_EVENT_TIMESIGN:
        case SND_SEQ_EVENT_KEYSIGN:
        case SND_SEQ_EVENT_START:
        case SND_SEQ_EVENT_CONTINUE:
        case SND_SEQ_EVENT_STOP:
        case SND_SEQ_EVENT_SETPOS_TICK:
        case SND_SEQ_EVENT_SETPOS_TIME:
        case SND_SEQ_EVENT_TEMPO:
        case SND_SEQ_EVENT_CLOCK:
        case SND_SEQ_EVENT_TICK:
        case SND_SEQ_EVENT_TUNE_REQUEST:
        case SND_SEQ_EVENT_RESET:
        case SND_SEQ_EVENT_SENSING:
            return true;
        default:
            return false;
    }
}

const char *mb_alsa_event_type_name(int type) {
    switch (type) {
        case SND_SEQ_EVENT_PORT_SUBSCRIBED:
            return "PORT_SUBSCRIBED";
        case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
            return "PORT_UNSUBSCRIBED";
        case SND_SEQ_EVENT_CLIENT_START:
            return "CLIENT_START";
        case SND_SEQ_EVENT_CLIENT_EXIT:
            return "CLIENT_EXIT";
        case SND_SEQ_EVENT_CLIENT_CHANGE:
            return "CLIENT_CHANGE";
        case SND_SEQ_EVENT_PORT_START:
            return "PORT_START";
        case SND_SEQ_EVENT_PORT_EXIT:
            return "PORT_EXIT";
        case SND_SEQ_EVENT_PORT_CHANGE:
            return "PORT_CHANGE";
        default:
            return "non-MIDI/control";
    }
}
