#include "mb-session.h"

#include <glib.h>

static void drive_to_streaming(MbSession *session) {
    g_assert_true(mb_session_handle_event(session, MB_EV_CMD_CONNECT));
    g_assert_cmpint(session->state, ==, MB_SESSION_CONNECTING);

    g_assert_true(mb_session_handle_event(session, MB_EV_BLUEZ_CONNECTED));
    g_assert_cmpint(session->state, ==, MB_SESSION_WAIT_SERVICES);
    g_assert_true(session->bluez_connected);

    g_assert_true(mb_session_handle_event(session, MB_EV_BLUEZ_SERVICES_RESOLVED));
    g_assert_cmpint(session->state, ==, MB_SESSION_BINDING_MIDI);
    g_assert_true(session->services_resolved);

    mb_session_set_midi_binding(session,
                                "/org/bluez/hci0/dev_A/service0001",
                                "/org/bluez/hci0/dev_A/service0001/char0002",
                                "00006bf3-0000-1000-8000-00805f9b34fb");
    g_assert_true(mb_session_handle_event(session, MB_EV_MIDI_CHAR_FOUND));
    g_assert_cmpint(session->state, ==, MB_SESSION_CREATING_ALSA);

    mb_session_set_alsa_port(session, 3);
    g_assert_true(mb_session_handle_event(session, MB_EV_ALSA_READY));
    g_assert_cmpint(session->state, ==, MB_SESSION_ENABLING_NOTIFY);

    g_assert_true(mb_session_handle_event(session, MB_EV_NOTIFY_OK));
    g_assert_cmpint(session->state, ==, MB_SESSION_STREAMING);
    g_assert_true(mb_session_is_streaming_ready(session));
}

static void test_single_session_happy_path(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "GO:KEYS");

    drive_to_streaming(&session);

    g_assert_cmpstr(mb_session_state_name(session.state), ==, "STREAMING");
    g_assert_cmpint(session.error, ==, MB_ERR_NONE);
    g_assert_cmpint(session.alsa_port_id, ==, 3);

    mb_session_clear(&session);
}

static void test_disconnect_enters_reconnecting_and_preserves_identity(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "GO:KEYS");

    drive_to_streaming(&session);

    g_assert_true(mb_session_handle_event(&session, MB_EV_BLUEZ_DISCONNECTED));
    g_assert_cmpint(session.state, ==, MB_SESSION_RECONNECTING);
    g_assert_cmpint(session.error, ==, MB_ERR_DISCONNECTED);
    g_assert_false(session.bluez_connected);
    g_assert_false(session.services_resolved);
    g_assert_false(session.notify_enabled);
    g_assert_cmpstr(session.address, ==, "AA:BB:CC:DD:EE:FF");
    g_assert_cmpstr(session.device_path, ==, "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF");

    g_assert_true(mb_session_handle_event(&session, MB_EV_RECONNECT_TIMER));
    g_assert_cmpint(session.state, ==, MB_SESSION_CONNECTING);
    g_assert_cmpuint(session.reconnect_attempts, ==, 1u);

    mb_session_clear(&session);
}

static void test_two_sessions_are_independent(void) {
    MbDaemon daemon;
    mb_daemon_init(&daemon);

    MbSession *a = mb_daemon_ensure_session(&daemon,
                                            "/org/bluez/hci0/dev_AA_AA_AA_AA_AA_AA",
                                            "AA:AA:AA:AA:AA:AA",
                                            "GO:KEYS A");
    MbSession *b = mb_daemon_ensure_session(&daemon,
                                            "/org/bluez/hci0/dev_BB_BB_BB_BB_BB_BB",
                                            "BB:BB:BB:BB:BB:BB",
                                            "GO:KEYS B");

    g_assert_nonnull(a);
    g_assert_nonnull(b);
    g_assert_true(a != b);
    g_assert_cmpuint(mb_daemon_session_count(&daemon), ==, 2u);
    g_assert_true(mb_daemon_find_session_by_address(&daemon, "AA:AA:AA:AA:AA:AA") == a);
    g_assert_true(mb_daemon_find_session_by_address(&daemon, "BB:BB:BB:BB:BB:BB") == b);

    drive_to_streaming(a);

    g_assert_true(mb_session_handle_event(b, MB_EV_CMD_CONNECT));
    g_assert_true(mb_session_handle_event(b, MB_EV_BLUEZ_CONNECTED));
    g_assert_cmpint(b->state, ==, MB_SESSION_WAIT_SERVICES);

    g_assert_true(mb_session_handle_event(a, MB_EV_BLUEZ_DISCONNECTED));
    g_assert_cmpint(a->state, ==, MB_SESSION_RECONNECTING);
    g_assert_cmpint(b->state, ==, MB_SESSION_WAIT_SERVICES);
    g_assert_true(b->bluez_connected);

    mb_daemon_clear(&daemon);
}

static void test_identical_keyboards_with_different_addresses_are_distinct(void) {
    MbDaemon daemon;
    mb_daemon_init(&daemon);

    MbSession *left = mb_daemon_ensure_session(&daemon,
                                               "/org/bluez/hci0/dev_11_22_33_44_55_66",
                                               "11:22:33:44:55:66",
                                               "GO:KEYS");
    MbSession *right = mb_daemon_ensure_session(&daemon,
                                                "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                                                "AA:BB:CC:DD:EE:FF",
                                                "GO:KEYS");

    g_assert_nonnull(left);
    g_assert_nonnull(right);
    g_assert_true(left != right);
    g_assert_cmpstr(left->name, ==, "GO:KEYS");
    g_assert_cmpstr(right->name, ==, "GO:KEYS");
    g_assert_cmpuint(mb_daemon_session_count(&daemon), ==, 2u);
    g_assert_true(mb_daemon_find_session_by_address(&daemon, "11:22:33:44:55:66") == left);
    g_assert_true(mb_daemon_find_session_by_address(&daemon, "AA:BB:CC:DD:EE:FF") == right);

    mb_session_set_alsa_port(left, 0);
    mb_session_set_alsa_port(right, 1);
    g_assert_cmpint(left->alsa_port_id, ==, 0);
    g_assert_cmpint(right->alsa_port_id, ==, 1);

    mb_daemon_clear(&daemon);
}

static void test_duplicate_address_reuses_session_and_reindexes_path(void) {
    MbDaemon daemon;
    mb_daemon_init(&daemon);

    MbSession *first = mb_daemon_ensure_session(&daemon,
                                                "/org/bluez/hci0/dev_11_22_33_44_55_66",
                                                "11:22:33:44:55:66",
                                                "GO:KEYS");
    MbSession *same = mb_daemon_ensure_session(&daemon,
                                               "/org/bluez/hci1/dev_11_22_33_44_55_66",
                                               "11:22:33:44:55:66",
                                               "GO:KEYS MIDI");

    g_assert_true(first == same);
    g_assert_cmpuint(mb_daemon_session_count(&daemon), ==, 1u);
    g_assert_null(mb_daemon_find_session_by_device_path(&daemon, "/org/bluez/hci0/dev_11_22_33_44_55_66"));
    g_assert_true(mb_daemon_find_session_by_device_path(&daemon, "/org/bluez/hci1/dev_11_22_33_44_55_66") == same);
    g_assert_true(mb_daemon_find_session_by_address(&daemon, "11:22:33:44:55:66") == same);
    g_assert_cmpstr(same->name, ==, "GO:KEYS MIDI");

    mb_daemon_clear(&daemon);
}

static void test_error_path_for_missing_midi_characteristic(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "GO:KEYS");

    g_assert_true(mb_session_handle_event(&session, MB_EV_CMD_CONNECT));
    g_assert_true(mb_session_handle_event(&session, MB_EV_BLUEZ_CONNECTED));
    g_assert_true(mb_session_handle_event(&session, MB_EV_BLUEZ_SERVICES_RESOLVED));
    g_assert_cmpint(session.state, ==, MB_SESSION_BINDING_MIDI);

    g_assert_true(mb_session_handle_event(&session, MB_EV_MIDI_CHAR_NOT_FOUND));
    g_assert_cmpint(session.state, ==, MB_SESSION_ERROR);
    g_assert_cmpint(session.error, ==, MB_ERR_MIDI_CHAR_NOT_FOUND);

    mb_session_clear(&session);
}

static void test_remove_session_updates_indexes(void) {
    MbDaemon daemon;
    mb_daemon_init(&daemon);

    const char *path = "/org/bluez/hci0/dev_AA_AA_AA_AA_AA_AA";
    const char *addr = "AA:AA:AA:AA:AA:AA";

    MbSession *session = mb_daemon_ensure_session(&daemon, path, addr, "GO:KEYS");
    g_assert_nonnull(session);
    g_assert_cmpuint(mb_daemon_session_count(&daemon), ==, 1u);
    g_assert_true(mb_daemon_find_session_by_device_path(&daemon, path) == session);
    g_assert_true(mb_daemon_find_session_by_address(&daemon, addr) == session);

    g_assert_true(mb_daemon_remove_session_by_device_path(&daemon, path));
    g_assert_cmpuint(mb_daemon_session_count(&daemon), ==, 0u);
    g_assert_null(mb_daemon_find_session_by_device_path(&daemon, path));
    g_assert_null(mb_daemon_find_session_by_address(&daemon, addr));

    mb_daemon_clear(&daemon);
}

static void test_invalid_transition_does_not_change_state(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "GO:KEYS");

    g_assert_false(mb_session_handle_event(&session, MB_EV_NOTIFY_OK));
    g_assert_cmpint(session.state, ==, MB_SESSION_IDLE);
    g_assert_cmpint(session.error, ==, MB_ERR_INVALID_TRANSITION);

    mb_session_clear(&session);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-session/single-session-happy-path", test_single_session_happy_path);
    g_test_add_func("/mb-session/disconnect-reconnect", test_disconnect_enters_reconnecting_and_preserves_identity);
    g_test_add_func("/mb-session/two-sessions-independent", test_two_sessions_are_independent);
    g_test_add_func("/mb-session/identical-keyboards-different-addresses", test_identical_keyboards_with_different_addresses_are_distinct);
    g_test_add_func("/mb-session/duplicate-address-reuses-session", test_duplicate_address_reuses_session_and_reindexes_path);
    g_test_add_func("/mb-session/error-missing-midi-characteristic", test_error_path_for_missing_midi_characteristic);
    g_test_add_func("/mb-session/remove-session-updates-indexes", test_remove_session_updates_indexes);
    g_test_add_func("/mb-session/invalid-transition", test_invalid_transition_does_not_change_state);

    return g_test_run();
}
