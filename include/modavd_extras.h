/*
 * ModAVDExtras: screen capture (and, later, input) for ModAVD instances,
 * for automation frameworks that want to skip adb.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Conventions
 * - Every function returns MODAVD_OK (0) or a negative MODAVD_E_* code,
 *   unless its comment says otherwise.
 * - Strings are UTF-8. Structs start with a `size` field: set it to
 *   sizeof(the struct) so that later versions can append fields.
 * - The library never falls back to adb by itself. It tells the caller what
 *   went wrong; the caller decides whether to use another method.
 * - A handle may be used from several threads at once (capture on one,
 *   input on another), but must not be closed while another call on it runs.
 */
#ifndef MODAVD_EXTRAS_H
#define MODAVD_EXTRAS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MODAVD_EXTRAS_STATIC)
#define MODAVD_API
#elif defined(_WIN32)
#if defined(MODAVD_EXTRAS_BUILD)
#define MODAVD_API __declspec(dllexport)
#else
#define MODAVD_API __declspec(dllimport)
#endif
#else
#define MODAVD_API __attribute__((visibility("default")))
#endif

/* Bumped whenever a function or field is added. Nothing is ever removed. */
#define MODAVD_API_VERSION 1

typedef struct modavd_instance* modavd_handle;

enum {
    MODAVD_OK = 0,
    MODAVD_E_INVALID = -1,     /* bad argument */
    MODAVD_E_NOT_FOUND = -2,   /* nothing answers on that console port */
    MODAVD_E_NOT_READY = -3,   /* frame sharing is off, no frame was composed yet, or (capture) new frames
                                  kept arriving during the copy: try again */
    MODAVD_E_RESIZED = -4,     /* the shared frame no longer fits the mapping: close and reopen */
    MODAVD_E_BUFFER = -5,      /* buffer too small; the frame info says how big it must be */
    MODAVD_E_IO = -6,          /* an OS call failed */
    MODAVD_E_UNSUPPORTED = -7, /* not in this version, on this platform or for this display */
};

/* modavd_capabilities() */
enum {
    MODAVD_CAP_SCREENCAP = 1u << 0,
    MODAVD_CAP_TOUCH = 1u << 1, /* not yet */
    MODAVD_CAP_KEY = 1u << 2,   /* not yet */
    MODAVD_CAP_TEXT = 1u << 3,  /* not yet */
};

/* Pixel formats for modavd_capture(). Rows are packed: stride = width * bytes per pixel. */
enum {
    MODAVD_FMT_BGRA = 1, /* what the emulator shares, copied as is */
    MODAVD_FMT_BGR = 2,  /* OpenCV's default */
    MODAVD_FMT_RGBA = 3,
};

/* modavd_open_params.flags */
enum {
    /* Do not ask the emulator console to start frame sharing when it is off. */
    MODAVD_OPEN_NO_START = 1u << 0,
};

/* modavd_capture() flags */
enum {
    MODAVD_CAPTURE_BOTTOM_UP = 1u << 0, /* first row of the buffer = bottom row of the screen */
};

/* modavd_set_log() levels */
enum {
    MODAVD_LOG_ERROR = 1,
    MODAVD_LOG_WARN = 2,
    MODAVD_LOG_INFO = 3,
    MODAVD_LOG_DEBUG = 4,
};

typedef struct modavd_open_params {
    uint32_t size;           /* sizeof(modavd_open_params) */
    const char* adb_address; /* "emulator-20010" or "127.0.0.1:20011"; may be NULL if console_port is set */
    int console_port;        /* the emulator console port; 0 = derive it from adb_address */
    const char* client_name; /* e.g. "MaaCore v6.21.2", only for logs; may be NULL */
    uint32_t flags;          /* MODAVD_OPEN_* */
    int timeout_ms;          /* how long to wait for frame sharing after starting it; 0 = 2000 */
} modavd_open_params;

typedef struct modavd_frame_info {
    uint32_t size;         /* sizeof(modavd_frame_info), set by the caller */
    int32_t width;
    int32_t height;
    int32_t stride;        /* bytes per row in the caller's buffer */
    uint32_t format;       /* MODAVD_FMT_* */
    uint32_t frame_number; /* increments when the emulator writes a new frame */
    uint64_t timestamp_us; /* when the emulator wrote it, emulator clock */
} modavd_frame_info;

/* ---- version and errors ---- */

/* Returns MODAVD_API_VERSION of the loaded library. */
MODAVD_API uint32_t modavd_api_version(void);
/* Returns the library version, e.g. "0.1.0". */
MODAVD_API const char* modavd_version_string(void);
/* Returns a short English description of a MODAVD_* code. Never NULL. */
MODAVD_API const char* modavd_strerror(int err);

/* Log sink for the whole library; NULL turns logging off (the default).
 * The callback may be called from any thread; msg has no trailing newline. */
typedef void (*modavd_log_fn)(int level, const char* msg, void* user);
MODAVD_API void modavd_set_log(modavd_log_fn fn, void* user);

/* ---- instances ---- */

/* Console port for an adb address: "emulator-<port>" is the console port;
 * "<host>:<port>" is the adb port, which the emulator always puts at console + 1.
 * Returns the port (> 0) or MODAVD_E_INVALID. */
MODAVD_API int modavd_parse_console_port(const char* adb_address);

/* Connects to an instance. On success *out is a handle for modavd_close().
 * MODAVD_E_NOT_FOUND: frame sharing is off and nothing answers on the console port.
 * MODAVD_E_NOT_READY: the console answered (or MODAVD_OPEN_NO_START is set) but
 *                     frame sharing did not come up in time. */
MODAVD_API int modavd_open(const modavd_open_params* params, modavd_handle* out);
MODAVD_API void modavd_close(modavd_handle h);

MODAVD_API uint32_t modavd_capabilities(modavd_handle h); /* MODAVD_CAP_*; 0 for a NULL handle */
MODAVD_API int modavd_console_port(modavd_handle h);      /* > 0, or MODAVD_E_INVALID */

/* ---- screen capture ---- */

/* Copies the latest frame of `display` (only 0 for now) into buf.
 * With buf == NULL or buf_size == 0 it only fills info (a size probe).
 * MODAVD_E_NOT_READY: no frame yet, or the emulator published a new frame
 *                     during each of the copies, so buf may mix two frames;
 *                     call again. MODAVD_E_RESIZED: reopen the handle.
 * MODAVD_E_BUFFER: buf_size < info->stride * info->height. */
MODAVD_API int modavd_capture(modavd_handle h, int display, uint32_t format, uint32_t flags, void* buf,
                              size_t buf_size, modavd_frame_info* info);

/* ---- input: declared for the next version, MODAVD_E_UNSUPPORTED for now ---- */

/* Coordinates are display pixels; contact is 0-based (at most 10 at once). */
MODAVD_API int modavd_touch_down(modavd_handle h, int display, int contact, int x, int y, int pressure);
MODAVD_API int modavd_touch_move(modavd_handle h, int display, int contact, int x, int y, int pressure);
MODAVD_API int modavd_touch_up(modavd_handle h, int display, int contact);
/* Lifts every contact this handle pressed. modavd_close() will do the same. */
MODAVD_API int modavd_touch_release_all(modavd_handle h);
/* Android key code (KEYCODE_*); down = 1 press, 0 release. */
MODAVD_API int modavd_key(modavd_handle h, int display, int android_keycode, int down);
MODAVD_API int modavd_input_text(modavd_handle h, const char* utf8);

#ifdef __cplusplus
}
#endif

#endif /* MODAVD_EXTRAS_H */
