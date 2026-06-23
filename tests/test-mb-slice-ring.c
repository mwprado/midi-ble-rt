#include "mb-slice-ring.h"

#include <glib.h>
#include <string.h>

static void test_push_take_owned(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    MbSliceRing ring;
    mb_slice_ring_init(&ring, MB_SLICE_RING_RX_RAW, &pool);

    uint8_t data[3] = {0x90, 0x40, 0x7f};
    MbFrameModelSlice slice;
    g_assert_true(mb_frame_model_make(&pool, data, sizeof(data), 100, &slice));
    g_assert_true(mb_slice_ring_push_owned(&ring, slice, 100));
    g_assert_cmpuint(mb_slice_ring_count(&ring), ==, 1);

    MbSliceRingItem item;
    g_assert_true(mb_slice_ring_take(&ring, &item));
    g_assert_cmpuint(item.slice.id, ==, slice.id);
    g_assert_cmpuint(item.slice.len, ==, sizeof(data));

    size_t len = 0;
    const uint8_t *got = mb_frame_model_get(&pool, item.slice, &len);
    g_assert_nonnull(got);
    g_assert_cmpmem(got, len, data, sizeof(data));

    mb_slice_ring_item_done(&ring, &item);
    g_assert_null(mb_frame_model_get(&pool, slice, NULL));
}

static void test_shared_slice_lifetime(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    MbSliceRing raw;
    MbSliceRing decoded;
    mb_slice_ring_init(&raw, MB_SLICE_RING_RX_RAW, &pool);
    mb_slice_ring_init(&decoded, MB_SLICE_RING_RX_DECODED, &pool);

    uint8_t data[5] = {0x90, 0x40, 0x7f, 0x80, 0x40};
    MbFrameModelSlice slice;
    g_assert_true(mb_frame_model_make(&pool, data, sizeof(data), 0, &slice));

    g_assert_true(mb_slice_ring_push_owned(&raw, slice, 0));
    g_assert_true(mb_slice_ring_push_shared(&decoded, slice, 1));

    MbSliceRingItem raw_item;
    g_assert_true(mb_slice_ring_take(&raw, &raw_item));
    mb_slice_ring_item_done(&raw, &raw_item);
    g_assert_nonnull(mb_frame_model_get(&pool, slice, NULL));

    MbSliceRingItem decoded_item;
    g_assert_true(mb_slice_ring_take(&decoded, &decoded_item));
    mb_slice_ring_item_done(&decoded, &decoded_item);
    g_assert_null(mb_frame_model_get(&pool, slice, NULL));
}

static void test_ring_overflow_keeps_frame_alive(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    MbSliceRing ring;
    mb_slice_ring_init(&ring, MB_SLICE_RING_TX_RAW, &pool);

    uint8_t byte = 0x55;
    MbFrameModelSlice slice[MB_SLICE_RING_COUNT + 1];

    for (unsigned i = 0; i < MB_SLICE_RING_COUNT + 1; i++)
        g_assert_true(mb_frame_model_make(&pool, &byte, 1, i, &slice[i]));

    for (unsigned i = 0; i < MB_SLICE_RING_COUNT; i++)
        g_assert_true(mb_slice_ring_push_owned(&ring, slice[i], i));

    g_assert_false(mb_slice_ring_push_owned(&ring, slice[MB_SLICE_RING_COUNT], 999));
    g_assert_cmpuint(mb_slice_ring_overflow_count(&ring), ==, 1);

    g_assert_nonnull(mb_frame_model_get(&pool, slice[MB_SLICE_RING_COUNT], NULL));
    g_assert_true(mb_frame_model_done(&pool, slice[MB_SLICE_RING_COUNT]));
    g_assert_null(mb_frame_model_get(&pool, slice[MB_SLICE_RING_COUNT], NULL));
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-slice-ring/push-take-owned", test_push_take_owned);
    g_test_add_func("/mb-slice-ring/shared-slice-lifetime", test_shared_slice_lifetime);
    g_test_add_func("/mb-slice-ring/overflow-keeps-frame-alive", test_ring_overflow_keeps_frame_alive);

    return g_test_run();
}
