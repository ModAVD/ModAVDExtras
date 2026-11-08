/*
 * Library tests that need no emulator: address parsing, error codes, and
 * capture from a fake shared frame in every format.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_frame.h"
#include "modavd_extras.h"

static int g_failures;

#define CHECK(cond)                                                                                                   \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                \
    do {                                                                                                              \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                                         \
        if (va_ != vb_) {                                                                                             \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = %lld, %s = %lld\n", __FILE__, __LINE__, #a, va_, #b, vb_);  \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

static void test_parse_console_port(void)
{
    CHECK_EQ(modavd_parse_console_port("emulator-20010"), 20010);
    CHECK_EQ(modavd_parse_console_port("emulator-5554"), 5554);
    CHECK_EQ(modavd_parse_console_port("127.0.0.1:20011"), 20010);
    CHECK_EQ(modavd_parse_console_port("localhost:5555"), 5554);
    CHECK_EQ(modavd_parse_console_port("[::1]:5555"), 5554);
    CHECK_EQ(modavd_parse_console_port(NULL), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port(""), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("emulator-"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("emulator-70000"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("emulator-12ab"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("127.0.0.1:"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("127.0.0.1:1"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port(":5555"), MODAVD_E_INVALID);
    CHECK_EQ(modavd_parse_console_port("R5CT20ABCDE"), MODAVD_E_INVALID);
}

static void test_version_and_errors(void)
{
    CHECK_EQ(modavd_api_version(), MODAVD_API_VERSION);
    CHECK(strlen(modavd_version_string()) > 0);
    CHECK(strcmp(modavd_strerror(MODAVD_OK), "ok") == 0);
    CHECK(modavd_strerror(-1234) != NULL);
    CHECK_EQ(modavd_capabilities(NULL), 0);
    CHECK_EQ(modavd_console_port(NULL), MODAVD_E_INVALID);
}

static void test_open_errors(void)
{
    modavd_handle h = (modavd_handle)(uintptr_t)1; /* must be reset to NULL on failure */
    CHECK_EQ(modavd_open(NULL, &h), MODAVD_E_INVALID);

    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_INVALID); /* neither address nor port */
    CHECK(h == NULL);
    p.adb_address = "R5CT20ABCDE"; /* a phone's serial: no console port in it */
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_INVALID);
    p.adb_address = NULL;

    p.size = 4; /* a caller from a future version would never send less */
    p.console_port = 65010;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_INVALID);

    /* No frame on this port and starting it is not allowed. */
    p.size = sizeof(p);
    p.flags = MODAVD_OPEN_NO_START;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_NOT_READY);

    /* No frame, and no console to start it: normally nothing listens here (NOT_FOUND);
     * if something unrelated does, it will not answer like a console (NOT_READY). */
    p.flags = 0;
    p.console_port = 65020;
    p.timeout_ms = 200;
    int rc = modavd_open(&p, &h);
    CHECK(rc == MODAVD_E_NOT_FOUND || rc == MODAVD_E_NOT_READY);
    CHECK(h == NULL);
}

static uint8_t expected_channel(uint32_t format, int channel, uint32_t x, uint32_t y)
{
    uint8_t b = (uint8_t)x, g = (uint8_t)y, r = (uint8_t)(x ^ y);
    switch (format) {
    case MODAVD_FMT_BGRA: return (uint8_t[]){ b, g, r, 0xFF }[channel];
    case MODAVD_FMT_RGBA: return (uint8_t[]){ r, g, b, 0xFF }[channel];
    default: return (uint8_t[]){ b, g, r, 0 }[channel]; /* 4 entries: GCC -O3 cannot tell channel < 3 here */
    }
}

static void check_capture(modavd_handle h, uint32_t format, int bpp, uint32_t flags, uint32_t w, uint32_t hgt)
{
    modavd_frame_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    size_t size = (size_t)w * hgt * (size_t)bpp;
    uint8_t* buf = (uint8_t*)malloc(size);
    CHECK_EQ(modavd_capture(h, 0, format, flags, buf, size, &info), MODAVD_OK);
    CHECK_EQ(info.width, w);
    CHECK_EQ(info.height, hgt);
    CHECK_EQ(info.stride, w * (uint32_t)bpp);
    CHECK_EQ(info.format, format);
    CHECK_EQ(info.frame_number, 7);
    CHECK_EQ(info.timestamp_us, 123456789ull);
    int bad = 0;
    for (uint32_t row = 0; row < hgt && !bad; ++row) {
        uint32_t y = (flags & MODAVD_CAPTURE_BOTTOM_UP) ? hgt - 1 - row : row;
        for (uint32_t x = 0; x < w && !bad; ++x) {
            for (int c = 0; c < bpp; ++c) {
                if (buf[(size_t)row * info.stride + (size_t)x * bpp + c] != expected_channel(format, c, x, y)) {
                    fprintf(stderr, "format %u flags %u: pixel (%u,%u) channel %d differs\n", format, flags, x, y, c);
                    bad = 1;
                    break;
                }
            }
        }
    }
    CHECK(!bad);
    free(buf);
}

static void test_capture_from_fake_frame(void)
{
    const int port = 65001;
    const uint32_t w = 64, hgt = 32;
    fake_frame f;
    if (fake_frame_create(&f, port, w, hgt, w, hgt) != 0) {
        fprintf(stderr, "cannot create the fake frame\n");
        ++g_failures;
        return;
    }

    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.adb_address = "127.0.0.1:65002"; /* adb port = console + 1 */
    p.flags = MODAVD_OPEN_NO_START;
    p.client_name = "test_core";
    modavd_handle h = NULL;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_OK);
    if (!h) {
        fake_frame_destroy(&f);
        return;
    }
    CHECK_EQ(modavd_console_port(h), port);
    CHECK_EQ(modavd_capabilities(h), MODAVD_CAP_SCREENCAP);

    check_capture(h, MODAVD_FMT_BGRA, 4, 0, w, hgt);
    check_capture(h, MODAVD_FMT_BGR, 3, 0, w, hgt);
    check_capture(h, MODAVD_FMT_RGBA, 4, 0, w, hgt);
    check_capture(h, MODAVD_FMT_RGBA, 4, MODAVD_CAPTURE_BOTTOM_UP, w, hgt);
    check_capture(h, MODAVD_FMT_BGR, 3, MODAVD_CAPTURE_BOTTOM_UP, w, hgt);

    modavd_frame_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    /* size probe */
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_OK);
    CHECK_EQ(info.stride * info.height, w * 3 * hgt);
    /* small buffer */
    uint8_t small[16];
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, small, sizeof(small), &info), MODAVD_E_BUFFER);
    CHECK_EQ(info.width, w);
    /* bad arguments */
    CHECK_EQ(modavd_capture(h, 1, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_capture(h, 0, 99, 0, NULL, 0, &info), MODAVD_E_INVALID);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, NULL), MODAVD_E_INVALID);
    info.size = 4;
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_INVALID);
    info.size = sizeof(info);
    CHECK_EQ(modavd_capture(NULL, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_INVALID);

    /* nothing composed yet */
    fake_frame_set_size(&f, 0, 0);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_NOT_READY);
    /* the display grew past the region */
    fake_frame_set_size(&f, w * 4, hgt * 4);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_E_RESIZED);
    fake_frame_set_size(&f, w, hgt);
    CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info), MODAVD_OK);

    /* input is not in this version */
    CHECK_EQ(modavd_touch_down(h, 0, 0, 1, 1, 1), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_touch_release_all(h), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_key(h, 0, 4, 1), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_input_text(h, "a"), MODAVD_E_UNSUPPORTED);

    modavd_close(h);
    modavd_close(NULL);
    fake_frame_destroy(&f);
}

/* The emulator writes without a lock. The hook publishes a frame after a copy,
 * as if the emulator wrote one meanwhile. */
typedef struct publish_state {
    fake_frame* frame;
    int remaining; /* how many more copies get a new frame after them */
    int calls;
} publish_state;

static void publish_after_copy(void* user)
{
    publish_state* s = (publish_state*)user;
    ++s->calls;
    if (s->remaining > 0) {
        --s->remaining;
        ++((mx_video_info*)s->frame->view)->frame_number;
    }
}

/* One new frame mid-copy: the next copy is clean. A new frame during every
 * copy: MODAVD_E_NOT_READY rather than a frame that may mix two. */
static void test_capture_new_frame_mid_copy(void)
{
    const int port = 65020;
    const uint32_t w = 8, hgt = 4;
    fake_frame f;
    if (fake_frame_create(&f, port, w, hgt, w, hgt) != 0) {
        fprintf(stderr, "cannot create the fake frame\n");
        ++g_failures;
        return;
    }
    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.console_port = port;
    p.flags = MODAVD_OPEN_NO_START;
    modavd_handle h = NULL;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_OK);
    if (h) {
        uint8_t buf[8 * 4 * 4];
        modavd_frame_info info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        uint32_t first = ((mx_video_info*)f.view)->frame_number;
        publish_state s = { &f, 1, 0 };
        mx_test_after_copy = publish_after_copy;
        mx_test_after_copy_user = &s;

        CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGRA, 0, buf, sizeof(buf), &info), MODAVD_OK);
        CHECK_EQ(s.calls, 2);
        CHECK_EQ(info.frame_number, first + 1);

        s.remaining = MX_MAX_COPY_ATTEMPTS;
        s.calls = 0;
        CHECK_EQ(modavd_capture(h, 0, MODAVD_FMT_BGRA, 0, buf, sizeof(buf), &info), MODAVD_E_NOT_READY);
        CHECK_EQ(s.calls, MX_MAX_COPY_ATTEMPTS);

        /* The shim's call: it hands the last copy over, so it needs to tell this apart. */
        s.remaining = MX_MAX_COPY_ATTEMPTS;
        s.calls = 0;
        CHECK_EQ(mx_copy_frame(&h->shm, MODAVD_FMT_RGBA, MODAVD_CAPTURE_BOTTOM_UP, 0, buf, sizeof(buf), &info),
                 MX_TORN);

        mx_test_after_copy = NULL;
        mx_test_after_copy_user = NULL;
        modavd_close(h);
    }
    fake_frame_destroy(&f);
}

/* Same frame through the public call at sizes that leave every possible tail after the
 * 16- and 4-pixel blocks, top-down and bottom-up. */
static void test_capture_odd_sizes(void)
{
    static const uint32_t sizes[][2] = { { 1, 1 }, { 1, 5 }, { 3, 4 }, { 15, 3 }, { 17, 5 }, { 33, 2 }, { 1281, 3 } };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const int port = 65003 + (int)i;
        const uint32_t w = sizes[i][0], hgt = sizes[i][1];
        fake_frame f;
        if (fake_frame_create(&f, port, w, hgt, w, hgt) != 0) {
            fprintf(stderr, "cannot create the fake frame %ux%u\n", w, hgt);
            ++g_failures;
            continue;
        }
        modavd_open_params p;
        memset(&p, 0, sizeof(p));
        p.size = sizeof(p);
        p.console_port = port;
        p.flags = MODAVD_OPEN_NO_START;
        modavd_handle h = NULL;
        CHECK_EQ(modavd_open(&p, &h), MODAVD_OK);
        if (h) {
            static const uint32_t all_flags[] = { 0, MODAVD_CAPTURE_BOTTOM_UP };
            for (size_t fi = 0; fi < 2; ++fi) {
                check_capture(h, MODAVD_FMT_BGR, 3, all_flags[fi], w, hgt);
                check_capture(h, MODAVD_FMT_RGBA, 4, all_flags[fi], w, hgt);
                check_capture(h, MODAVD_FMT_BGRA, 4, all_flags[fi], w, hgt);
            }
            modavd_close(h);
        }
        fake_frame_destroy(&f);
    }
}

/* One pixel of the reference: what mx_convert_row must write for BGRA pixel s. */
static void reference_pixel(const uint8_t* s, uint32_t format, uint8_t* out)
{
    switch (format) {
    case MODAVD_FMT_RGBA:
        out[0] = s[2], out[1] = s[1], out[2] = s[0], out[3] = s[3];
        break;
    case MODAVD_FMT_BGR:
        out[0] = s[0], out[1] = s[1], out[2] = s[2];
        break;
    default:
        memcpy(out, s, 4);
        break;
    }
}

/* Row conversion against the per-pixel reference: every width up to a few blocks plus
 * real screen widths, misaligned source and destination, the alpha byte not 0xFF, and
 * guard bytes on both sides of the row to catch a block writing past its end. Runs the
 * portable code and, where the CPU has it, the vector code. */
static void test_convert_row(void)
{
    const size_t GUARD = 64, MAX_W = 1921;
    static const uint32_t extra_widths[] = { 255, 256, 257, 719, 720, 1279, 1280, 1281, 1919, 1920, 1921 };
    static const uint32_t formats[][2] = { { MODAVD_FMT_BGR, 3 }, { MODAVD_FMT_RGBA, 4 }, { MODAVD_FMT_BGRA, 4 } };
    uint8_t* src = (uint8_t*)malloc(MAX_W * 4 + 4);
    uint8_t* dst = (uint8_t*)malloc(GUARD + MAX_W * 4 + 4 + GUARD);
    uint8_t* want = (uint8_t*)malloc(MAX_W * 4);
    if (!src || !dst || !want) {
        ++g_failures;
        free(src), free(dst), free(want);
        return;
    }
    uint32_t seed = 12345;
    for (size_t i = 0; i < MAX_W * 4 + 4; ++i) {
        seed = seed * 1103515245u + 12345u;
        src[i] = (uint8_t)(seed >> 16);
    }

    int simd_levels[2] = { 0, mx_cpu_simd() };
    int n_levels = simd_levels[1] ? 2 : 1;
    printf("test_core: row conversion with portable code%s\n", n_levels == 2 ? " and the vector path" : " only");
    int bad = 0;
    for (int lv = 0; lv < n_levels && !bad; ++lv) {
        for (size_t fi = 0; fi < sizeof(formats) / sizeof(formats[0]) && !bad; ++fi) {
            const uint32_t format = formats[fi][0], bpp = formats[fi][1];
            for (uint32_t wi = 0; wi < 68 + sizeof(extra_widths) / sizeof(extra_widths[0]) && !bad; ++wi) {
                const uint32_t w = wi < 68 ? wi : extra_widths[wi - 68];
                for (uint32_t so = 0; so < 4 && !bad; ++so) {
                    for (uint32_t d_off = 0; d_off < 4 && !bad; ++d_off) {
                        const uint8_t* s = src + so;
                        for (uint32_t x = 0; x < w; ++x) {
                            reference_pixel(s + (size_t)x * 4, format, want + (size_t)x * bpp);
                        }
                        size_t row = (size_t)w * bpp, total = GUARD + MAX_W * 4 + 4 + GUARD;
                        memset(dst, 0xA5, total);
                        uint8_t* d = dst + GUARD + d_off;
                        mx_convert_row(s, d, w, format, simd_levels[lv]);
                        if (memcmp(d, want, row) != 0) {
                            fprintf(stderr, "simd %d format %u width %u offsets %u/%u: row differs\n",
                                    simd_levels[lv], format, w, so, d_off);
                            bad = 1;
                        }
                        for (size_t i = 0; i < total && !bad; ++i) {
                            int inside = i >= (size_t)(d - dst) && i < (size_t)(d - dst) + row;
                            if (!inside && dst[i] != 0xA5) {
                                fprintf(stderr, "simd %d format %u width %u offsets %u/%u: wrote outside the row\n",
                                        simd_levels[lv], format, w, so, d_off);
                                bad = 1;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(!bad);
    free(src), free(dst), free(want);
}

static int g_log_lines;
static void count_log(int level, const char* msg, void* user)
{
    (void)level;
    (void)msg;
    (void)user;
    ++g_log_lines;
}

static void test_log_callback(void)
{
    modavd_set_log(count_log, NULL);
    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.console_port = 65011;
    p.flags = MODAVD_OPEN_NO_START;
    modavd_handle h = NULL;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_E_NOT_READY);
    CHECK(g_log_lines > 0);
    modavd_set_log(NULL, NULL);
}

int main(void)
{
    test_parse_console_port();
    test_version_and_errors();
    test_open_errors();
    test_capture_from_fake_frame();
    test_capture_new_frame_mid_copy();
    test_capture_odd_sizes();
    test_convert_row();
    test_log_callback();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_core: all checks passed\n");
    return 0;
}
