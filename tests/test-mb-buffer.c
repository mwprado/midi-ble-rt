#include "mb-buffer.h"
#include "mb-session.h"

#include <glib.h>
#include <string.h>

static void fill_bytes(uint8_t *buf, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i);
}

static void test_ring_initial_state(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_RX_RAW, 0);

    g_assert_cmpstr(mb_buffer_kind_name(ring.kind), ==, "rx_raw");
    g_assert_cmpuint(mb_buffer_ring_active_limit(&ring), ==, MB_BUFFER_LIMIT_MIN);
    g_assert_cmpuint(mb_buffer_ring_count(&ring), ==, 0);
    g_assert_true(mb_buffer_ring_is_empty(&ring));
    g_assert_false(mb_buffer_ring_is_full(&ring));
}

static void test_ring_push_pop_order(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_RX_RAW, 0);

    uint8_t a[3] = {0x90, 0x40, 0x7f};
    uint8_t b[3] = {0x80, 0x40, 0x00};

    g_assert_true(mb_buffer_ring_push(&ring, a, sizeof(a), 100, 100));
    g_assert_true(mb_buffer_ring_push(&ring, b, sizeof(b), 200, 200));
    g_assert_cmpuint(mb_buffer_ring_count(&ring), ==, 2);

    MbBufferSlot slot;
    g_assert_true(mb_buffer_ring_pop(&ring, &slot));
    g_assert_cmpuint(slot.len, ==, sizeof(a));
    g_assert_cmpuint(slot.seq, ==, 0);
    g_assert_cmpuint(slot.timestamp_ns, ==, 100);
    g_assert_cmpmem(slot.data, slot.len, a, sizeof(a));

    g_assert_true(mb_buffer_ring_pop(&ring, &slot));
    g_assert_cmpuint(slot.len, ==, sizeof(b));
    g_assert_cmpuint(slot.seq, ==, 1);
    g_assert_cmpuint(slot.timestamp_ns, ==, 200);
    g_assert_cmpmem(slot.data, slot.len, b, sizeof(b));

    g_assert_true(mb_buffer_ring_is_empty(&ring));
}

static void test_ring_wraparound(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_TX_TRANSMIT, 0);

    uint8_t byte = 0x7f;
    MbBufferSlot slot;

    for (unsigned i = 0; i < MB_BUFFER_SLOT_COUNT; i++)
        g_assert_true(mb_buffer_ring_push(&ring, &byte, 1, i, i));
    g_assert_true(mb_buffer_ring_is_full(&ring));

    for (unsigned i = 0; i < 8; i++) {
        g_assert_true(mb_buffer_ring_pop(&ring, &slot));
        g_assert_cmpuint(slot.seq, ==, i);
    }

    for (unsigned i = 0; i < 8; i++)
        g_assert_true(mb_buffer_ring_push(&ring, &byte, 1, 100 + i, 100 + i));

    g_assert_true(mb_buffer_ring_is_full(&ring));

    for (unsigned i = 8; i < MB_BUFFER_SLOT_COUNT + 8; i++) {
        g_assert_true(mb_buffer_ring_pop(&ring, &slot));
        g_assert_cmpuint(slot.seq, ==, i);
    }

    g_assert_true(mb_buffer_ring_is_empty(&ring));
}

static void test_ring_grows_by_len(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_RX_RAW, 0);

    uint8_t buf[MB_BUFFER_SLOT_CAPACITY];
    fill_bytes(buf, sizeof(buf), 0x10);

    g_assert_true(mb_buffer_ring_push(&ring, buf, 65, 0, 0));
    g_assert_cmpuint(ring.active_limit, ==, 128);
    g_assert_cmpuint(ring.grow_count, ==, 1);

    g_assert_true(mb_buffer_ring_push(&ring, buf, 129, 1, 1));
    g_assert_cmpuint(ring.active_limit, ==, 256);
    g_assert_cmpuint(ring.grow_count, ==, 2);
}

static void test_ring_rejects_oversize(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_RX_RAW, 0);

    uint8_t buf[MB_BUFFER_SLOT_CAPACITY + 1];
    fill_bytes(buf, sizeof(buf), 0x20);

    g_assert_false(mb_buffer_ring_push(&ring, buf, sizeof(buf), 0, 0));
    g_assert_cmpuint(ring.oversize_drops, ==, 1);
    g_assert_cmpuint(ring.overflows, ==, 1);
    g_assert_cmpuint(mb_buffer_ring_count(&ring), ==, 0);
}

static void test_ring_overflow_at_16_slots(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_RX_RAW, 0);

    uint8_t byte = 0x55;
    for (unsigned i = 0; i < MB_BUFFER_SLOT_COUNT; i++)
        g_assert_true(mb_buffer_ring_push(&ring, &byte, 1, i, i));

    g_assert_false(mb_buffer_ring_push(&ring, &byte, 1, 999, 999));
    g_assert_cmpuint(ring.overflows, ==, 1);
    g_assert_cmpuint(mb_buffer_ring_count(&ring), ==, MB_BUFFER_SLOT_COUNT);
}

static void test_ring_shrinks_after_30s(void) {
    MbBufferRing ring;
    mb_buffer_ring_init(&ring, MB_BUFFER_RX_RAW, 0);

    uint8_t buf[129];
    fill_bytes(buf, sizeof(buf), 0x30);

    g_assert_true(mb_buffer_ring_push(&ring, buf, sizeof(buf), 0, 0));
    g_assert_cmpuint(ring.active_limit, ==, 256);

    mb_buffer_ring_tick(&ring, MB_BUFFER_SHRINK_AFTER_NS - 1);
    g_assert_cmpuint(ring.active_limit, ==, 256);

    mb_buffer_ring_tick(&ring, MB_BUFFER_SHRINK_AFTER_NS);
    g_assert_cmpuint(ring.active_limit, ==, 128);
    g_assert_cmpuint(ring.shrink_count, ==, 1);

    mb_buffer_ring_tick(&ring, MB_BUFFER_SHRINK_AFTER_NS * 2);
    g_assert_cmpuint(ring.active_limit, ==, 64);
    g_assert_cmpuint(ring.shrink_count, ==, 2);

    mb_buffer_ring_tick(&ring, MB_BUFFER_SHRINK_AFTER_NS * 3);
    g_assert_cmpuint(ring.active_limit, ==, 64);
    g_assert_cmpuint(ring.shrink_count, ==, 2);
}

static void test_session_has_six_independent_rings(void) {
    MbSession session;
    mb_session_init(&session, "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", "AA:BB:CC:DD:EE:FF", "BLE-MIDI");

    g_assert_cmpstr(mb_buffer_kind_name(session.buffers.rx_raw.kind), ==, "rx_raw");
    g_assert_cmpstr(mb_buffer_kind_name(session.buffers.rx_decoded.kind), ==, "rx_decoded");
    g_assert_cmpstr(mb_buffer_kind_name(session.buffers.rx_play.kind), ==, "rx_play");
    g_assert_cmpstr(mb_buffer_kind_name(session.buffers.tx_raw.kind), ==, "tx_raw");
    g_assert_cmpstr(mb_buffer_kind_name(session.buffers.tx_decoded.kind), ==, "tx_decoded");
    g_assert_cmpstr(mb_buffer_kind_name(session.buffers.tx_transmit.kind), ==, "tx_transmit");

    uint8_t byte = 0x90;
    g_assert_true(mb_buffer_ring_push(&session.buffers.rx_raw, &byte, 1, 0, 0));
    g_assert_cmpuint(session.buffers.rx_raw.count, ==, 1);
    g_assert_cmpuint(session.buffers.tx_raw.count, ==, 0);

    mb_session_clear(&session);
}

static void test_session_reset_clears_buffers(void) {
    MbSession session;
    mb_session_init(&session, "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", "AA:BB:CC:DD:EE:FF", "BLE-MIDI");

    uint8_t byte = 0x90;
    g_assert_true(mb_buffer_ring_push(&session.buffers.rx_raw, &byte, 1, 0, 0));
    g_assert_cmpuint(session.buffers.rx_raw.count, ==, 1);

    g_assert_true(mb_session_handle_event(&session, MB_EV_CMD_CONNECT));
    g_assert_true(mb_session_handle_event(&session, MB_EV_BLUEZ_CONNECTED));
    g_assert_true(mb_session_handle_event(&session, MB_EV_CMD_DISCONNECT));

    g_assert_cmpuint(session.buffers.rx_raw.count, ==, 0);
    g_assert_cmpuint(session.buffers.rx_raw.active_limit, ==, MB_BUFFER_LIMIT_MIN);

    mb_session_clear(&session);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-buffer/ring-initial-state", test_ring_initial_state);
    g_test_add_func("/mb-buffer/ring-push-pop-order", test_ring_push_pop_order);
    g_test_add_func("/mb-buffer/ring-wraparound", test_ring_wraparound);
    g_test_add_func("/mb-buffer/ring-grows-by-len", test_ring_grows_by_len);
    g_test_add_func("/mb-buffer/ring-rejects-oversize", test_ring_rejects_oversize);
    g_test_add_func("/mb-buffer/ring-overflow-at-16-slots", test_ring_overflow_at_16_slots);
    g_test_add_func("/mb-buffer/ring-shrinks-after-30s", test_ring_shrinks_after_30s);
    g_test_add_func("/mb-buffer/session-has-six-independent-rings", test_session_has_six_independent_rings);
    g_test_add_func("/mb-buffer/session-reset-clears-buffers", test_session_reset_clears_buffers);

    return g_test_run();
}
