#include "mb-control-protocol.h"

#include <glib.h>

static void assert_parse_ok(const char *line,
                            MbControlCommandType expected_type,
                            const char *expected_device) {
    MbControlCommand command;

    g_assert_true(mb_control_command_parse(line, &command));
    g_assert_cmpint(command.type, ==, expected_type);
    g_assert_cmpstr(command.device, ==, expected_device);

    mb_control_command_clear(&command);
}

static void assert_parse_rejected(const char *line) {
    MbControlCommand command;

    g_assert_false(mb_control_command_parse(line, &command));
    mb_control_command_clear(&command);
}

static void test_parse_read_only_commands(void) {
    assert_parse_ok("PING", MB_CONTROL_CMD_PING, NULL);
    assert_parse_ok("STATUS", MB_CONTROL_CMD_STATUS, NULL);
    assert_parse_ok("LIST", MB_CONTROL_CMD_LIST, NULL);
}

static void test_parse_device_commands(void) {
    assert_parse_ok("CONNECT device-a", MB_CONTROL_CMD_CONNECT, "device-a");
    assert_parse_ok("DISCONNECT AA:BB:CC:DD:EE:FF", MB_CONTROL_CMD_DISCONNECT, "AA:BB:CC:DD:EE:FF");
    assert_parse_ok("RECHECK controller_1", MB_CONTROL_CMD_RECHECK, "controller_1");
}

static void test_parse_is_case_insensitive_and_trims(void) {
    assert_parse_ok("  ping  ", MB_CONTROL_CMD_PING, NULL);
    assert_parse_ok("\tconnect\tdevice-a\t", MB_CONTROL_CMD_CONNECT, "device-a");
    assert_parse_ok("status", MB_CONTROL_CMD_STATUS, NULL);
}

static void test_parse_rejects_invalid_lines(void) {
    assert_parse_rejected(NULL);
    assert_parse_rejected("");
    assert_parse_rejected("   ");
    assert_parse_rejected("UNKNOWN");
    assert_parse_rejected("PING device-a");
    assert_parse_rejected("STATUS device-a");
    assert_parse_rejected("LIST device-a");
    assert_parse_rejected("CONNECT");
    assert_parse_rejected("DISCONNECT");
    assert_parse_rejected("RECHECK");
    assert_parse_rejected("CONNECT device-a extra");
}

static void test_command_names_are_stable(void) {
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_PING), ==, "PING");
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_STATUS), ==, "STATUS");
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_LIST), ==, "LIST");
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_CONNECT), ==, "CONNECT");
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_DISCONNECT), ==, "DISCONNECT");
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_RECHECK), ==, "RECHECK");
    g_assert_cmpstr(mb_control_command_name(MB_CONTROL_CMD_UNKNOWN), ==, "UNKNOWN");
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-control-protocol/parse-read-only-commands", test_parse_read_only_commands);
    g_test_add_func("/mb-control-protocol/parse-device-commands", test_parse_device_commands);
    g_test_add_func("/mb-control-protocol/parse-is-case-insensitive-and-trims", test_parse_is_case_insensitive_and_trims);
    g_test_add_func("/mb-control-protocol/rejects-invalid-lines", test_parse_rejects_invalid_lines);
    g_test_add_func("/mb-control-protocol/command-names-are-stable", test_command_names_are_stable);

    return g_test_run();
}
