#include "mb-frame-model.h"

#include <glib.h>
#include <string.h>

static void fill_payload(uint8_t *buf, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i);
}

static void test_make_and_get(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    uint8_t data[5] = {0x90, 0x40, 0x7f, 0x80, 0x40};
    MbFrameModelSlice slice;

    g_assert_true(mb_frame_model_make(&pool, data, sizeof(data), 123, &slice));
    g_assert_true(mb_frame_model_slice_ok(slice));
    g_assert_cmpuint(slice.off, ==, 0);
    g_assert_cmpuint(slice.len, ==, sizeof(data));

    size_t len = 0;
    const uint8_t *got = mb_frame_model_get(&pool, slice, &len);
    g_assert_nonnull(got);
    g_assert_cmpuint(len, ==, sizeof(data));
    g_assert_cmpmem(got, len, data, sizeof(data));
}

static void test_hold_done_lifetime(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    uint8_t data[3] = {0x90, 0x45, 0x60};
    MbFrameModelSlice slice;

    g_assert_true(mb_frame_model_make(&pool, data, sizeof(data), 0, &slice));
    g_assert_true(mb_frame_model_hold(&pool, slice));

    g_assert_true(mb_frame_model_done(&pool, slice));
    g_assert_nonnull(mb_frame_model_get(&pool, slice, NULL));

    g_assert_true(mb_frame_model_done(&pool, slice));
    g_assert_null(mb_frame_model_get(&pool, slice, NULL));
}

static void test_pool_exhaustion(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    uint8_t byte = 0x55;
    MbFrameModelSlice slices[MB_FRAME_MODEL_COUNT];

    for (unsigned i = 0; i < MB_FRAME_MODEL_COUNT; i++)
        g_assert_true(mb_frame_model_make(&pool, &byte, 1, i, &slices[i]));

    MbFrameModelSlice extra;
    g_assert_false(mb_frame_model_make(&pool, &byte, 1, 999, &extra));
    g_assert_cmpuint(pool.empty_count, ==, 1);

    g_assert_true(mb_frame_model_done(&pool, slices[0]));
    g_assert_true(mb_frame_model_make(&pool, &byte, 1, 1000, &extra));
    g_assert_true(mb_frame_model_slice_ok(extra));
}

static void test_reject_oversize(void) {
    MbFrameModelPool pool;
    mb_frame_model_pool_init(&pool);

    uint8_t data[MB_FRAME_MODEL_CAPACITY + 1];
    fill_payload(data, sizeof(data), 0x10);

    MbFrameModelSlice slice;
    g_assert_false(mb_frame_model_make(&pool, data, sizeof(data), 0, &slice));
    g_assert_false(mb_frame_model_slice_ok(slice));
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-frame-model/make-and-get", test_make_and_get);
    g_test_add_func("/mb-frame-model/hold-done-lifetime", test_hold_done_lifetime);
    g_test_add_func("/mb-frame-model/pool-exhaustion", test_pool_exhaustion);
    g_test_add_func("/mb-frame-model/reject-oversize", test_reject_oversize);

    return g_test_run();
}
