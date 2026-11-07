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

/* ---- core.c ---- */
struct modavd_instance {
    int console_port;
    int simd; /* mx_cpu_simd() at open */
    mx_shm shm;
};

/* 1 when this CPU runs the vector row conversion (SSSE3 on x86, NEON on
 * ARM64), else 0. */
int mx_cpu_simd(void);

/* Converts one row of width BGRA pixels into format (MODAVD_FMT_*), writing
 * exactly width * bytes-per-pixel bytes. src and dst need no alignment.
 * simd: the value of mx_cpu_simd(), or 0 for the portable code only. */
void mx_convert_row(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t format, int simd);

/* Copies one frame; the work behind modavd_capture(). */
int mx_copy_frame(const mx_shm* shm, uint32_t format, uint32_t flags, int simd, void* buf, size_t buf_size,
                  modavd_frame_info* info);

#ifdef __cplusplus
}
#endif

#endif /* MX_INTERNAL_H */
