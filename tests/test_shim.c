/*
 * Tests the MuMu-shaped shell the way MaaCore uses it: LoadLibrary the DLL,
 * look up the four functions by name, capture from a fake shared frame.
 * Usage: test_shim <path to external_renderer_ipc.dll> <scratch directory>
 *
 * Windows only. Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_frame.h"

typedef int (*connect_fn)(const wchar_t* path, int index);
typedef void (*disconnect_fn)(int handle);
typedef int (*display_id_fn)(int handle, const char* pkg, int app_index);
typedef int (*capture_fn)(int handle, unsigned int display_id, int buffer_size, int* width, int* height,
                          unsigned char* pixels);

static int g_failures;
#define CHECK(cond)                                                                                                   \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test_shim <external_renderer_ipc.dll> <scratch directory>\n");
        return 2;
    }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        fprintf(stderr, "LoadLibrary %s failed (%lu)\n", argv[1], (unsigned long)GetLastError());
        return 1;
    }
    connect_fn nemu_connect = (connect_fn)(void*)GetProcAddress(dll, "nemu_connect");
    disconnect_fn nemu_disconnect = (disconnect_fn)(void*)GetProcAddress(dll, "nemu_disconnect");
    display_id_fn nemu_get_display_id = (display_id_fn)(void*)GetProcAddress(dll, "nemu_get_display_id");
    capture_fn nemu_capture_display = (capture_fn)(void*)GetProcAddress(dll, "nemu_capture_display");
    CHECK(nemu_connect && nemu_disconnect && nemu_get_display_id && nemu_capture_display);
    /* Same exports as the old avdshim: no input functions (MaaCore must keep maatouch), no library API. */
    CHECK(GetProcAddress(dll, "nemu_input_event_finger_touch_down") == NULL);
    CHECK(GetProcAddress(dll, "modavd_open") == NULL);
    if (g_failures) {
        return 1;
    }

    const int port = 65003;
    const uint32_t w = 48, h = 20;
    fake_frame f;
    if (fake_frame_create(&f, port, w, h, w, h) != 0) {
        fprintf(stderr, "cannot create the fake frame\n");
        return 1;
    }

    wchar_t scratch[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, scratch, MAX_PATH);
    char log_path[MAX_PATH];
    snprintf(log_path, sizeof(log_path), "%s\\avdshim.log", argv[2]);
    remove(log_path);

    int handle = nemu_connect(scratch, port); /* index >= 20000 is the console port, as AUTO-MAS passes it */
    CHECK(handle == 1);
    CHECK(nemu_get_display_id(handle, "com.hypergryph.arknights", 0) == 0);
    CHECK(nemu_get_display_id(handle + 5, "x", 0) == -1);

    int cw = 0, ch = 0;
    CHECK(nemu_capture_display(handle, 0, 0, &cw, &ch, NULL) == 0);
    CHECK(cw == (int)w && ch == (int)h);

    size_t size = (size_t)w * h * 4;
    unsigned char* buf = (unsigned char*)malloc(size);
    CHECK(nemu_capture_display(handle, 0, (int)size, &cw, &ch, buf) == 0);
    int bad = 0;
    for (uint32_t row = 0; row < h && !bad; ++row) {
        uint32_t y = h - 1 - row; /* MuMu hands rows bottom-up */
        for (uint32_t x = 0; x < w; ++x) {
            const unsigned char* p = buf + ((size_t)row * w + x) * 4;
            if (p[0] != (uint8_t)(x ^ y) || p[1] != (uint8_t)y || p[2] != (uint8_t)x || p[3] != 0xFF) {
                fprintf(stderr, "pixel (%u,%u) differs\n", x, y);
                bad = 1;
                break;
            }
        }
    }
    CHECK(!bad);
    CHECK(nemu_capture_display(handle, 0, 16, &cw, &ch, buf) == -3);
    CHECK(nemu_capture_display(99, 0, 0, &cw, &ch, NULL) == -1);

    fake_frame_set_size(&f, 0, 0);
    CHECK(nemu_capture_display(handle, 0, (int)size, &cw, &ch, buf) == -2);
    fake_frame_set_size(&f, w, h);

    /* Started but not published yet: the size and the frame as they are, like
     * the DLL this replaces; MaaCore's connect asks for the size right away. */
    mx_video_info* vi = (mx_video_info*)f.view;
    uint64_t ts = vi->ts_us;
    vi->ts_us = 0;
    cw = ch = 0;
    CHECK(nemu_capture_display(handle, 0, 0, &cw, &ch, NULL) == 0);
    CHECK(cw == (int)w && ch == (int)h);
    CHECK(nemu_capture_display(handle, 0, (int)size, &cw, &ch, buf) == 0);
    vi->ts_us = ts;

    nemu_disconnect(handle);
    CHECK(nemu_capture_display(handle, 0, 0, &cw, &ch, NULL) == -1);
    free(buf);

    /* No frame on that port and nothing listens: connect fails with 0. */
    CHECK(nemu_connect(scratch, 65021) == 0);

    /* An instance number (as a user types it into MAA): the console port is on
     * the port plan of the root that the MuMu path sits in, <root>\mumu-shim.
     * Plan 1100 and index 3000: port 31100, while the default plan would give
     * 50000; neither is near a real instance's ports (20000.., 21000..), so a
     * wrong plan only fails to connect and never reaches a running emulator. */
    {
        const int base = 1100, index = 3000;
        fake_frame g;
        if (fake_frame_create(&g, base + 10 * index, w, h, w, h) != 0) {
            fprintf(stderr, "cannot create the second fake frame\n");
            return 1;
        }
        char dir[MAX_PATH], json[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\root\\mumu-shim", argv[2]);
        snprintf(json, sizeof(json), "%s\\root\\mas-avd.json", argv[2]);
        char parent[MAX_PATH];
        snprintf(parent, sizeof(parent), "%s\\root", argv[2]);
        CreateDirectoryA(parent, NULL);
        CreateDirectoryA(dir, NULL);
        wchar_t wdir[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH);
        static const char* plans[] = {
            "{\"portBase\": 1100}",
            "\xEF\xBB\xBF{\r\n  \"components\": {},\r\n  \"portBase\" :\r\n 1100,\r\n  \"instances\": {}\r\n}",
        };
        for (size_t k = 0; k < sizeof(plans) / sizeof(plans[0]); ++k) {
            FILE* jf = fopen(json, "wb");
            CHECK(jf != NULL);
            if (jf) {
                fputs(plans[k], jf);
                fclose(jf);
            }
            int hi = nemu_connect(wdir, index);
            CHECK(hi > 0);
            CHECK(nemu_capture_display(hi, 0, 0, &cw, &ch, NULL) == 0 && cw == (int)w && ch == (int)h);
            nemu_disconnect(hi);
            /* a trailing separator on the MuMu path names the same root */
            wchar_t slash[MAX_PATH];
            swprintf(slash, MAX_PATH, L"%ls\\", wdir);
            hi = nemu_connect(slash, index);
            CHECK(hi > 0);
            nemu_disconnect(hi);
        }
        /* A portBase ModAVD does not accept is not taken: the plan falls back to 20000 (port 50000, no frame).
         * Frames wait where taking 1150 or 1000 would land, so taking them shows. */
        fake_frame g1150, g1000;
        if (fake_frame_create(&g1150, 1150 + 10 * index, w, h, w, h) != 0 ||
            fake_frame_create(&g1000, 1000 + 10 * index, w, h, w, h) != 0) {
            fprintf(stderr, "cannot create the fake frames for refused plans\n");
            return 1;
        }
        static const char* bad_plans[] = { "{\"portBase\": 1150}", "{\"portBase\": 1100000}", "{\"portBase\": 1100.5}",
                                           "{\"portBase\": \"1100\"}", "{\"portBase\": 1000}", "{\"portBase\": 65500}",
                                           "{\"portbase\": 1100}" };
        for (size_t k = 0; k < sizeof(bad_plans) / sizeof(bad_plans[0]); ++k) {
            FILE* jf = fopen(json, "wb");
            CHECK(jf != NULL);
            if (jf) {
                fputs(bad_plans[k], jf);
                fclose(jf);
            }
            int hi = nemu_connect(wdir, index);
            if (hi > 0) {
                fprintf(stderr, "portBase %s was taken\n", bad_plans[k]);
                ++g_failures;
                nemu_disconnect(hi);
            }
        }
        fake_frame_destroy(&g1150);
        fake_frame_destroy(&g1000);
        fake_frame_destroy(&g);
        remove(json);
        RemoveDirectoryA(dir);
        RemoveDirectoryA(parent);
    }

    FILE* lf = fopen(log_path, "rb");
    CHECK(lf != NULL);
    if (lf) {
        char text[4096] = { 0 };
        fread(text, 1, sizeof(text) - 1, lf);
        fclose(lf);
        CHECK(strstr(text, "connect index=65003 port=65003 handle=1 48x20 fps=60 frame=7") != NULL);
        CHECK(strstr(text, "disconnect handle=1 port=65003") != NULL);
        CHECK(strstr(text, "connect index=65021 port=65021: shared memory unavailable") != NULL);
    }

    fake_frame_destroy(&f);
    FreeLibrary(dll);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_shim: all checks passed\n");
    return 0;
}
