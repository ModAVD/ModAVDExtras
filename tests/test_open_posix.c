/*
 * POSIX open tests with real shared memory and a controlled console response.
 *
 * Copyright 2026 HarcoChen
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>

#include "fake_frame.h"

static int failures;
static int console_calls;
static int console_result;
static fake_frame* replacement;

#define CHECK(condition)                                                                                               \
    do {                                                                                                              \
        if (!(condition)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                            \
            ++failures;                                                                                               \
        }                                                                                                             \
    } while (0)

/* This definition replaces console.c from the static archive. No network or
 * auth token is needed; core.c and shm.c run through the public entry points. */
int mx_console_command(int port, const char* command, int timeout_ms)
{
    ++console_calls;
    CHECK(port == 65013);
    CHECK(strcmp(command, "screenrecord webrtc start") == 0);
    CHECK(timeout_ms > 0);
    if (replacement) {
        CHECK(fake_frame_create(replacement, port, 8, 4, 8, 4) == 0);
    }
    return console_result;
}

int main(void)
{
    fake_frame stale;
    if (fake_frame_create(&stale, 65013, 8, 4, 8, 4) != 0) {
        return 1;
    }
    fake_frame_set_size(&stale, 4, 2);
    modavd_open_params p = { 0 };
    p.size = sizeof(p);
    p.console_port = stale.port;
    p.timeout_ms = 100;
    modavd_handle h = NULL;

    /* A valid old frame must not hide an absent or inaccessible producer. */
    console_result = MODAVD_E_NOT_FOUND;
    CHECK(modavd_open(&p, &h) == MODAVD_E_NOT_FOUND);
    CHECK(h == NULL);
    modavd_close(h);
    console_result = MODAVD_E_IO;
    CHECK(modavd_open(&p, &h) == MODAVD_E_IO);
    CHECK(h == NULL);
    modavd_close(h);
    CHECK(console_calls == 2);

    /* NO_START explicitly trusts the supplied mapping, without a console. */
    p.flags = MODAVD_OPEN_NO_START;
    CHECK(modavd_open(&p, &h) == MODAVD_OK);
    CHECK(console_calls == 2);
    modavd_close(h);

    /* The new producer replaces the old name when starting. Open after that
     * command so that the handle sees the new 8x4 frame, not the old 4x2 one. */
    fake_frame current = { 0 };
    replacement = &current;
    console_result = MODAVD_OK;
    p.flags = 0;
    CHECK(modavd_open(&p, &h) == MODAVD_OK);
    CHECK(console_calls == 3);
    CHECK(h != NULL);
    replacement = NULL;
    modavd_handle second = NULL;
    CHECK(modavd_open(&p, &second) == MODAVD_OK);
    CHECK(console_calls == 4);
    CHECK(second != NULL);
    modavd_frame_info info = { 0 };
    info.size = sizeof(info);
    uint8_t buf[8 * 4 * 4];
    if (h && second) {
        CHECK(modavd_capture(h, 0, MODAVD_FMT_BGRA, 0, buf, sizeof(buf), &info) == MODAVD_OK);
        CHECK(info.width == 8 && info.height == 4);
        CHECK(modavd_capture(second, 0, MODAVD_FMT_BGRA, 0, buf, sizeof(buf), &info) == MODAVD_OK);
        CHECK(info.width == 8 && info.height == 4);
        modavd_close(second);
        second = NULL;
        /* Closing another reader must not stop a static, already published frame. */
        CHECK(modavd_capture(h, 0, MODAVD_FMT_BGRA, 0, buf, sizeof(buf), &info) == MODAVD_OK);
    }
    modavd_close(second);
    modavd_close(h);
    if (current.view) {
        fake_frame_destroy(&current);
    }
    fake_frame_destroy(&stale);
    return failures ? 1 : 0;
}
