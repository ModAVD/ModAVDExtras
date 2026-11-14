/*
 * Internal interfaces shared by the library, the MuMu-shaped shim and the tests.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef MX_INTERNAL_H
#define MX_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "modavd_extras.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Layout of the emulator's shared frame (AOSP VideoFrameSharer.h, VideoInfo):
 * this header, then width * height BGRA pixels, rows top-down. The emulator
 * writes the pixels first and then frameNumber / tsUs, without a lock. */
typedef struct mx_video_info {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t frame_number;
    uint64_t ts_us;
} mx_video_info;

#define MX_HEADER_SIZE 24
#define MX_MAX_COPY_ATTEMPTS 3
#define MX_DEFAULT_TIMEOUT_MS 2000

/* mx_copy_frame() when the emulator published a new frame during every one
 * of the MX_MAX_COPY_ATTEMPTS copies: the buffer may mix two frames. Internal
 * only (positive, never a MODAVD_* code): modavd_capture() reports
 * MODAVD_E_NOT_READY; the MuMu-shaped shim hands the last copy over, as the
 * DLL it replaces did. */
#define MX_TORN 1

/* mx_copy_frame() flag, internal only (modavd_capture() clears it): copy the
 * frame even if the emulator has not published one yet (timestamp 0). The
 * region then holds the size and a black frame. The MuMu-shaped shim uses
 * it, as the DLL it replaces did: a failed capture makes MaaCore give up on
 * this path or rerun its screencap speed test. */
#define MX_CAPTURE_UNPUBLISHED (1u << 31)

/* ---- log.c ---- */
void mx_log(int level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* ---- shm.c ---- */
typedef struct mx_shm {
    void* handle; /* Windows: HANDLE of the mapping; POSIX: unused */
    const uint8_t* view;
    size_t size;
} mx_shm;

/* Platform name of the shared frame of a console port:
 * "SHM_videmulator<port>" on Windows, "videmulator<port>" elsewhere. */
void mx_shm_name(int port, char* buf, size_t cap);
/* MODAVD_OK, MODAVD_E_NOT_READY (no such region) or MODAVD_E_IO. */
int mx_shm_open(int port, mx_shm* out);
void mx_shm_close(mx_shm* shm);

/* ---- console.c ---- */
/* Sends one command to the emulator console on 127.0.0.1:port, after the
 * auth command when ~/.emulator_console_auth_token is not empty.
 * MODAVD_OK: the console answered OK. MODAVD_E_NOT_FOUND: could not connect.
 * MODAVD_E_IO: auth failed, the command answered KO, or a timeout. */
int mx_console_command(int port, const char* command, int timeout_ms);

/* A console session kept open across commands. sock is the platform socket,
 * -1 (INVALID_SOCKET) when closed. */
typedef struct mx_console {
    intptr_t sock;
} mx_console;

/* Connects and authenticates. MODAVD_OK, MODAVD_E_NOT_FOUND (could not
 * connect) or MODAVD_E_IO (auth failed). timeout_ms bounds each reply. */
int mx_console_open(int port, int timeout_ms, mx_console* c);
/* MODAVD_OK, MODAVD_E_IO (the console answered KO) or MODAVD_E_NOT_FOUND
 * (the session broke; it is closed and may be opened again). */
int mx_console_send(mx_console* c, const char* command);
/* mx_console_send() that also copies what the console answered (the lines up
 * to and including OK / KO, truncated to cap) into reply. */
int mx_console_query(mx_console* c, const char* command, char* reply, size_t cap);
/* Says quit and closes; nothing happens when already closed. */
void mx_console_close(mx_console* c);

/* The longest command line a session sends, without the line end. */
#define MX_CONSOLE_LINE_MAX 768

/* ---- input.c ---- */
#define MX_MAX_CONTACTS 10
#define MX_ABS_MAX 32767        /* the virtio touch screen's axis range (INPUT_EVENT_ABS_MAX) */
#define MX_TOUCH_PRESSURE 0x400 /* what "event mouse" presses with (MTS_PRESSURE_RANGE_MAX) */
#define MX_TOUCH_SIZE 0x500     /* ABS_MT_TOUCH_MAJOR / MINOR of "event mouse" */

enum { MX_TOUCH_DOWN = 0, MX_TOUCH_MOVE = 1, MX_TOUCH_UP = 2 };

/* A display pixel coordinate (0 .. size-1, clamped) on the touch screen's
 * 0 .. MX_ABS_MAX axis, scaled as the emulator scales "event mouse". */
int mx_touch_axis(int v, int size);
/* Writes the console command for one touch action on contact (slot) with
 * axis coordinates ax, ay. Returns the length, or -1 if it does not fit. */
int mx_touch_command(char* buf, size_t cap, int action, int contact, int ax, int ay, int pressure);

/* ---- input.c: keys and text ---- */
#define MX_KEYBOARD_CHUNK 4  /* characters per "event text": a longer burst overruns the guest keyboard's queue */
#define MX_KEYBOARD_GAP_MS 4 /* pause after each of them, for the guest to read the keys */
#define MX_COMMIT_CHUNK 480  /* bytes of text per "ime commit" */

/* The Linux input code (KEY_*) of an Android key code (KEYCODE_*), 0 if the
 * emulator's keyboard has no such key. */
int mx_key_linux_code(int android_keycode);
/* The character an Android key code types unshifted ('a', '1', '\n', ...), 0 if none. */
int mx_key_char(int android_keycode);
/* 1 when the keyboard can type all of text: printable ASCII, '\n' and '\t'. */
int mx_text_is_keyboard(const char* utf8);
/* Writes "event text ..." for the next characters of *text (at most
 * MX_KEYBOARD_CHUNK) and advances *text. Returns the length, 0 at the end of
 * the text, -1 if it does not fit. */
int mx_text_keyboard_command(char* buf, size_t cap, const char** text);
/* Writes "ime commit ..." for the next bytes of *text, up to a line end or
 * MX_COMMIT_CHUNK bytes (whole UTF-8 sequences), and advances *text; a line
 * end is taken with it and *newline set to 1. Returns the length (0 when only
 * a line end was taken, or at the end of the text), -1 if it does not fit. */
int mx_text_commit_command(char* buf, size_t cap, const char** text, int* newline);

struct mx_input; /* lock, console session and pressed contacts of one handle */
/* MODAVD_CAP_KEY when the console has "event key", else 0. Asks the console once per session. */
uint32_t mx_input_capabilities(struct mx_input* in);
struct mx_input* mx_input_create(int console_port);
/* Lifts the contacts still down, closes the session and frees. */
void mx_input_destroy(struct mx_input* in);

/* ---- core.c ---- */
struct modavd_instance {
    int console_port;
    int simd; /* mx_cpu_simd() at open */
    mx_shm shm;
    struct mx_input* input;
};

/* 1 when this CPU runs the vector row conversion (SSSE3 on x86, NEON on
 * ARM64), else 0. */
int mx_cpu_simd(void);

/* Converts one row of width BGRA pixels into format (MODAVD_FMT_*), writing
 * exactly width * bytes-per-pixel bytes. src and dst need no alignment.
 * simd: the value of mx_cpu_simd(), or 0 for the portable code only. */
void mx_convert_row(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t format, int simd);

/* Copies one frame; the work behind modavd_capture(). MODAVD_OK, a MODAVD_E_*
 * code, or MX_TORN. */
int mx_copy_frame(const mx_shm* shm, uint32_t format, uint32_t flags, int simd, void* buf, size_t buf_size,
                  modavd_frame_info* info);

/* Tests only: when set, mx_copy_frame() calls it after each copy, before it
 * reads the frame number again, so a test can publish a frame mid-copy. NULL
 * everywhere else. */
extern void (*mx_test_after_copy)(void* user);
extern void* mx_test_after_copy_user;

#ifdef __cplusplus
}
#endif

#endif /* MX_INTERNAL_H */
