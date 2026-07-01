#include "mb-session.h"

#include <glib.h>

static void assert_event(MbSession *session,
                         MbEventType event,
                         gboolean accepted,
                         MbSessionState state,
                         MbErrorCode error) {
    g_assert_cmpint(mb_session_handle_event(session, event), ==, accepted);
    g_assert_cmpint(session->state, ==, state);
    g_assert_cmpint(session->error, ==, error);
}

static void test_nominal_streaming_path(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "BLE-MIDI");

    assert_event(&session, MB_EV_CMD_CONNECT, true, MB_SESSION_CONNECTING, MB_ERR_NONE);
    assert_event(&session, MB_EV_BLUEZ_CONNECTED, true, MB_SESSION_WAIT_SERVICES, MB_ERR_NONE);
    assert_event(&session, MB_EV_BLUEZ_SERVICES_RESOLVED, true, MB_SESSION_BINDING_MIDI, MB_ERR_NONE);
    assert_event(&session, MB_EV_MIDI_CHAR_FOUND, true, MB_SESSION_CREATING_ALSA, MB_ERR_NONE);
    assert_event(&session, MB_EV_ALSA_READY, true, MB_SESSION_ENABLING_NOTIFY, MB_ERR_NONE);
    assert_event(&session, MB_EV_NOTIFY_OK, true, MB_SESSION_STREAMING, MB_ERR_NONE);

    g_assert_true(session.bluez_connected);
    g_assert_true(session.services_resolved);
    g_assert_true(session.alsa_ready);
    g_assert_true(session.notify_enabled);

    mb_session_clear(&session);
}

static void test_invalid_direct_streaming_transition_is_rejected(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "BLE-MIDI");

    assert_event(&session, MB_EV_NOTIFY_OK, false, MB_SESSION_IDLE, MB_ERR_INVALID_TRANSITION);

    mb_session_clear(&session);
}

static void test_disconnect_resets_runtime_state(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "BLE-MIDI");

    assert_event(&session, MB_EV_CMD_CONNECT, true, MB_SESSION_CONNECTING, MB_ERR_NONE);
    assert_event(&session, MB_EV_BLUEZ_CONNECTED, true, MB_SESSION_WAIT_SERVICES, MB_ERR_NONE);
    assert_event(&session, MB_EV_BLUEZ_SERVICES_RESOLVED, true, MB_SESSION_BINDING_MIDI, MB_ERR_NONE);
    mb_session_set_midi_binding(&session,
                                "/service",
                                "/char",
                                "7772e5db-3868-4112-a1a9-f2669d106bf3");
    assert_event(&session, MB_EV_MIDI_CHAR_FOUND, true, MB_SESSION_CREATING_ALSA, MB_ERR_NONE);
    mb_session_set_alsa_port(&session, 7);
    assert_event(&session, MB_EV_ALSA_READY, true, MB_SESSION_ENABLING_NOTIFY, MB_ERR_NONE);
    assert_event(&session, MB_EV_NOTIFY_OK, true, MB_SESSION_STREAMING, MB_ERR_NONE);

    assert_event(&session, MB_EV_CMD_DISCONNECT, true, MB_SESSION_IDLE, MB_ERR_NONE);

    g_assert_false(session.bluez_connected);
    g_assert_false(session.services_resolved);
    g_assert_false(session.notify_enabled);
    g_assert_false(session.alsa_ready);
    g_assert_cmpint(session.alsa_port_id, ==, -1);
    g_assert_null(session.midi_service_path);
    g_assert_null(session.midi_char_path);
    g_assert_null(session.midi_char_uuid);

    mb_session_clear(&session);
}

static void test_streaming_disconnect_enters_reconnecting_when_enabled(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "BLE-MIDI");

    assert_event(&session, MB_EV_CMD_CONNECT, true, MB_SESSION_CONNECTING, MB_ERR_NONE);
    assert_event(&session, MB_EV_BLUEZ_CONNECTED, true, MB_SESSION_WAIT_SERVICES, MB_ERR_NONE);
    assert_event(&session, MB_EV_BLUEZ_SERVICES_RESOLVED, true, MB_SESSION_BINDING_MIDI, MB_ERR_NONE);
    assert_event(&session, MB_EV_MIDI_CHAR_FOUND, true, MB_SESSION_CREATING_ALSA, MB_ERR_NONE);
    mb_session_set_alsa_port(&session, 9);
    assert_event(&session, MB_EV_ALSA_READY, true, MB_SESSION_ENABLING_NOTIFY, MB_ERR_NONE);
    assert_event(&session, MB_EV_NOTIFY_OK, true, MB_SESSION_STREAMING, MB_ERR_NONE);

    assert_event(&session, MB_EV_BLUEZ_DISCONNECTED, true, MB_SESSION_RECONNECTING, MB_ERR_DISCONNECTED);
    g_assert_cmpint(session.alsa_port_id, ==, 9);
    g_assert_false(session.bluez_connected);
    g_assert_false(session.services_resolved);
    g_assert_false(session.notify_enabled);

    assert_event(&session, MB_EV_RECONNECT_TIMER, true, MB_SESSION_CONNECTING, MB_ERR_NONE);
    g_assert_cmpuint(session.reconnect_attempts, ==, 1);

    mb_session_clear(&session);
}

static void test_error_can_restart_connect(void) {
    MbSession session;
    mb_session_init(&session,
                    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                    "AA:BB:CC:DD:EE:FF",
                    "BLE-MIDI");

    assert_event(&session, MB_EV_RUNTIME_FAILED, true, MB_SESSION_ERROR, MB_ERR_RUNTIME_FAILED);
    assert_event(&session, MB_EV_CMD_CONNECT, true, MB_SESSION_CONNECTING, MB_ERR_NONE);

    mb_session_clear(&session);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-session-state/nominal-streaming-path", test_nominal_streaming_path);
    g_test_add_func("/mb-session-state/rejects-direct-streaming", test_invalid_direct_streaming_transition_is_rejected);
    g_test_add_func("/mb-session-state/disconnect-resets-runtime-state", test_disconnect_resets_runtime_state);
    g_test_add_func("/mb-session-state/streaming-disconnect-reconnects", test_streaming_disconnect_enters_reconnecting_when_enabled);
    g_test_add_func("/mb-session-state/error-can-restart-connect", test_error_can_restart_connect);

    return g_test_run();
}
