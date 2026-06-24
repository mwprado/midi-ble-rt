#include "mb-alsa.h"

#include <glib.h>
#include <string.h>

static snd_seq_event_t event_of_type(int type) {
    snd_seq_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    return ev;
}

static void test_midi_payload_events_are_accepted(void) {
    snd_seq_event_t note_on = event_of_type(SND_SEQ_EVENT_NOTEON);
    snd_seq_event_t note_off = event_of_type(SND_SEQ_EVENT_NOTEOFF);
    snd_seq_event_t controller = event_of_type(SND_SEQ_EVENT_CONTROLLER);
    snd_seq_event_t pitchbend = event_of_type(SND_SEQ_EVENT_PITCHBEND);

    g_assert_true(mb_alsa_event_is_midi_payload(&note_on));
    g_assert_true(mb_alsa_event_is_midi_payload(&note_off));
    g_assert_true(mb_alsa_event_is_midi_payload(&controller));
    g_assert_true(mb_alsa_event_is_midi_payload(&pitchbend));
}

static void test_control_events_are_rejected(void) {
    snd_seq_event_t subscribed = event_of_type(SND_SEQ_EVENT_PORT_SUBSCRIBED);
    snd_seq_event_t unsubscribed = event_of_type(SND_SEQ_EVENT_PORT_UNSUBSCRIBED);
    snd_seq_event_t client_start = event_of_type(SND_SEQ_EVENT_CLIENT_START);
    snd_seq_event_t port_start = event_of_type(SND_SEQ_EVENT_PORT_START);

    g_assert_false(mb_alsa_event_is_midi_payload(&subscribed));
    g_assert_false(mb_alsa_event_is_midi_payload(&unsubscribed));
    g_assert_false(mb_alsa_event_is_midi_payload(&client_start));
    g_assert_false(mb_alsa_event_is_midi_payload(&port_start));
}

static void test_null_event_is_rejected(void) {
    g_assert_false(mb_alsa_event_is_midi_payload(NULL));
}

static void test_event_type_names(void) {
    g_assert_cmpstr(mb_alsa_event_type_name(SND_SEQ_EVENT_PORT_SUBSCRIBED), ==, "PORT_SUBSCRIBED");
    g_assert_cmpstr(mb_alsa_event_type_name(SND_SEQ_EVENT_PORT_UNSUBSCRIBED), ==, "PORT_UNSUBSCRIBED");
    g_assert_cmpstr(mb_alsa_event_type_name(9999), ==, "non-MIDI/control");
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-alsa/midi-payload-events", test_midi_payload_events_are_accepted);
    g_test_add_func("/mb-alsa/control-events", test_control_events_are_rejected);
    g_test_add_func("/mb-alsa/null-event", test_null_event_is_rejected);
    g_test_add_func("/mb-alsa/event-type-names", test_event_type_names);

    return g_test_run();
}
