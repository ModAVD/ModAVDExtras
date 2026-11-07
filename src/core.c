/*
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "mx_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define MX_X86 1
#include <tmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define MX_NEON 1
#include <arm_neon.h>
#endif

#ifndef MODAVD_EXTRAS_VERSION
#define MODAVD_EXTRAS_VERSION "0.0.0"
#endif

uint32_t modavd_api_version(void) { return MODAVD_API_VERSION; }

const char* modavd_version_string(void) { return MODAVD_EXTRAS_VERSION; }

const char* modavd_strerror(int err)
{
    switch (err) {
    case MODAVD_OK: return "ok";
    case MODAVD_E_INVALID: return "invalid argument";
    case MODAVD_E_NOT_FOUND: return "no emulator console on that port";
    case MODAVD_E_NOT_READY: return "frame sharing is not ready";
    case MODAVD_E_RESIZED: return "the shared frame changed size, reopen";
    case MODAVD_E_BUFFER: return "buffer too small";
    case MODAVD_E_IO: return "system call failed";
    case MODAVD_E_UNSUPPORTED: return "not supported";
    default: return "unknown error";
    }
}

/* Parses a decimal port 1..65535 that makes up all of s. */
static int parse_port(const char* s)
{
    if (!s || !*s) {
        return MODAVD_E_INVALID;
    }
    long v = 0;
    for (const char* p = s; *p; ++p) {
        if (!isdigit((unsigned char)*p) || v > 65535) {
            return MODAVD_E_INVALID;
        }
        v = v * 10 + (*p - '0');
    }
    return (v >= 1 && v <= 65535) ? (int)v : MODAVD_E_INVALID;
}

int modavd_parse_console_port(const char* adb_address)
{
    if (!adb_address) {
        return MODAVD_E_INVALID;
    }
    static const char kSerial[] = "emulator-";
    if (strncmp(adb_address, kSerial, sizeof(kSerial) - 1) == 0) {
        return parse_port(adb_address + sizeof(kSerial) - 1);
    }
    const char* colon = strrchr(adb_address, ':');
    if (!colon || colon == adb_address) {
        return MODAVD_E_INVALID;
    }
    int adb_port = parse_port(colon + 1);
    return adb_port > 1 ? adb_port - 1 : MODAVD_E_INVALID;
}

int modavd_open(const modavd_open_params* params, modavd_handle* out)
{
    if (!params || params->size < sizeof(modavd_open_params) || !out) {
        return MODAVD_E_INVALID;
    }
    *out = NULL;
    int port = params->console_port > 0 ? params->console_port : modavd_parse_console_port(params->adb_address);
    if (port <= 0 || port > 65535) {
        return MODAVD_E_INVALID;
    }
    int timeout_ms = params->timeout_ms > 0 ? params->timeout_ms : MX_DEFAULT_TIMEOUT_MS;

    mx_shm shm = { 0 };
    int rc = mx_shm_open(port, &shm);
    if (rc == MODAVD_E_NOT_READY && !(params->flags & MODAVD_OPEN_NO_START)) {
        int console = mx_console_command(port, "screenrecord webrtc start", 3000);
        if (console == MODAVD_E_NOT_FOUND) {
            return MODAVD_E_NOT_FOUND;
        }
        for (int waited = 0; rc == MODAVD_E_NOT_READY && waited < timeout_ms; waited += 50) {
            sleep_ms(50);
            rc = mx_shm_open(port, &shm);
        }
    }
    if (rc != MODAVD_OK) {
        mx_log(MODAVD_LOG_WARN, "port=%d: shared memory unavailable", port);
        return rc;
    }

    struct modavd_instance* inst = (struct modavd_instance*)calloc(1, sizeof(*inst));
    if (!inst) {
        mx_shm_close(&shm);
        return MODAVD_E_IO;
    }
    inst->console_port = port;
    inst->simd = mx_cpu_simd();
    inst->shm = shm;
    const volatile mx_video_info* vi = (const volatile mx_video_info*)shm.view;
    mx_log(MODAVD_LOG_INFO, "open port=%d %ux%u fps=%u frame=%u size=%zu client=%s", port, vi->width, vi->height,
           vi->fps, vi->frame_number, shm.size, params->client_name ? params->client_name : "-");
    *out = inst;
    return MODAVD_OK;
}

void modavd_close(modavd_handle h)
{
    if (!h) {
        return;
    }
    mx_shm_close(&h->shm);
    free(h);
}

uint32_t modavd_capabilities(modavd_handle h) { return h ? MODAVD_CAP_SCREENCAP : 0; }

int modavd_console_port(modavd_handle h) { return h ? h->console_port : MODAVD_E_INVALID; }

static int bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case MODAVD_FMT_BGRA:
    case MODAVD_FMT_RGBA: return 4;
    case MODAVD_FMT_BGR: return 3;
    default: return 0;
    }
}

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "the pixel conversion below assumes a little-endian CPU"
#endif

/* BGRA -> BGR, portable: four pixels (16 bytes) in, three little-endian words (12 bytes) out,
 * then one pixel at a time for the last 0-3. */
static void bgr_row_portable(const uint8_t* src, uint8_t* dst, uint32_t width)
{
    uint32_t x = 0;
    for (; x + 4 <= width; x += 4, src += 16, dst += 12) {
        uint32_t p[4], o[3];
        memcpy(p, src, sizeof(p));
        o[0] = (p[0] & 0x00FFFFFFu) | (p[1] << 24);         /* B0 G0 R0 B1 */
        o[1] = ((p[1] >> 8) & 0x0000FFFFu) | (p[2] << 16);  /* G1 R1 B2 G2 */
        o[2] = ((p[2] >> 16) & 0x000000FFu) | (p[3] << 8);  /* R2 B3 G3 R3 */
        memcpy(dst, o, sizeof(o));
    }
    for (; x < width; ++x, src += 4, dst += 3) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    }
}

#if MX_X86
/* BGRA -> BGR, 16 pixels at a time: pshufb packs each 16-byte load to 12 bytes,
 * then shifts and ORs splice four of those into three 16-byte stores.
 * Returns how many pixels it converted (a multiple of 16). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
static uint32_t bgr_row_ssse3(const uint8_t* src, uint8_t* dst, uint32_t width)
{
    const __m128i pack = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
    uint32_t x = 0;
    for (; x + 16 <= width; x += 16, src += 64, dst += 48) {
        __m128i a = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(const void*)src), pack);
        __m128i b = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(const void*)(src + 16)), pack);
        __m128i c = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(const void*)(src + 32)), pack);
        __m128i d = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(const void*)(src + 48)), pack);
        _mm_storeu_si128((__m128i*)(void*)dst, _mm_or_si128(a, _mm_slli_si128(b, 12)));
        _mm_storeu_si128((__m128i*)(void*)(dst + 16), _mm_or_si128(_mm_srli_si128(b, 4), _mm_slli_si128(c, 8)));
        _mm_storeu_si128((__m128i*)(void*)(dst + 32), _mm_or_si128(_mm_srli_si128(c, 8), _mm_slli_si128(d, 4)));
    }
    return x;
}
#elif MX_NEON
/* BGRA -> BGR, 16 pixels at a time: split the channels, store three of them interleaved. */
static uint32_t bgr_row_neon(const uint8_t* src, uint8_t* dst, uint32_t width)
{
    uint32_t x = 0;
    for (; x + 16 <= width; x += 16, src += 64, dst += 48) {
        uint8x16x4_t p = vld4q_u8(src);
        uint8x16x3_t o;
        o.val[0] = p.val[0];
        o.val[1] = p.val[1];
        o.val[2] = p.val[2];
        vst3q_u8(dst, o);
    }
    return x;
}
#endif

int mx_cpu_simd(void)
{
#if MX_X86
#if defined(__SSSE3__) || defined(__AVX__)
    return 1;
#elif defined(_MSC_VER)
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 9) & 1; /* CPUID.1:ECX.SSSE3 */
#else
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && ((c >> 9) & 1);
#endif
#elif MX_NEON
    return 1; /* part of every ARM64 CPU */
#else
    return 0;
#endif
}

void mx_convert_row(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t format, int simd)
{
    switch (format) {
    case MODAVD_FMT_BGRA:
        memcpy(dst, src, (size_t)width * 4);
        break;
    case MODAVD_FMT_RGBA:
        /* One 32-bit word per pixel (little-endian: B,G,R,A bytes = 0xAARRGGBB): swap R and B.
         * memcpy keeps it safe for an unaligned caller buffer; compilers turn it into plain loads. */
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t p;
            memcpy(&p, src + (size_t)x * 4, 4);
            p = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
            memcpy(dst + (size_t)x * 4, &p, 4);
        }
        break;
    case MODAVD_FMT_BGR: {
        uint32_t done = 0;
#if MX_X86
        if (simd) {
            done = bgr_row_ssse3(src, dst, width);
        }
#elif MX_NEON
        if (simd) {
            done = bgr_row_neon(src, dst, width);
        }
#else
        (void)simd;
#endif
        bgr_row_portable(src + (size_t)done * 4, dst + (size_t)done * 3, width - done);
        break;
    }
    default:
        break;
    }
}

int mx_copy_frame(const mx_shm* shm, uint32_t format, uint32_t flags, int simd, void* buf, size_t buf_size,
                  modavd_frame_info* info)
{
    int bpp = bytes_per_pixel(format);
    if (!bpp || !info || info->size < sizeof(modavd_frame_info)) {
        return MODAVD_E_INVALID;
    }
    const volatile mx_video_info* vi = (const volatile mx_video_info*)shm->view;
    uint32_t w = vi->width;
    uint32_t h = vi->height;
    info->width = (int32_t)w;
    info->height = (int32_t)h;
    info->stride = (int32_t)(w * (uint32_t)bpp);
    info->format = format;
    info->frame_number = vi->frame_number;
    info->timestamp_us = vi->ts_us;
    if (w == 0 || h == 0) {
        return MODAVD_E_NOT_READY;
    }
    if (MX_HEADER_SIZE + (uint64_t)w * h * 4 > shm->size) {
        return MODAVD_E_RESIZED;
    }
    if (!buf || buf_size == 0) {
        return MODAVD_OK;
    }
    if (buf_size < (size_t)info->stride * h) {
        return MODAVD_E_BUFFER;
    }

    const uint8_t* src = shm->view + MX_HEADER_SIZE;
    uint8_t* dst = (uint8_t*)buf;
    size_t src_stride = (size_t)w * 4;
    /* No lock on the writer's side: copy again if a new frame was published meanwhile. */
    for (int attempt = 0; attempt < MX_MAX_COPY_ATTEMPTS; ++attempt) {
        uint32_t before = vi->frame_number;
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = (flags & MODAVD_CAPTURE_BOTTOM_UP) ? h - 1 - y : y;
            mx_convert_row(src + (size_t)sy * src_stride, dst + (size_t)y * (size_t)info->stride, w, format, simd);
        }
        uint32_t after = vi->frame_number;
        info->frame_number = after;
        info->timestamp_us = vi->ts_us;
        if (after == before) {
            break;
        }
    }
    return MODAVD_OK;
}

int modavd_capture(modavd_handle h, int display, uint32_t format, uint32_t flags, void* buf, size_t buf_size,
                   modavd_frame_info* info)
{
    if (!h) {
        return MODAVD_E_INVALID;
    }
    if (display != 0) {
        return MODAVD_E_UNSUPPORTED;
    }
    return mx_copy_frame(&h->shm, format, flags, h->simd, buf, buf_size, info);
}

/* Input arrives in the next version. */
int modavd_touch_down(modavd_handle h, int display, int contact, int x, int y, int pressure)
{
    (void)h, (void)display, (void)contact, (void)x, (void)y, (void)pressure;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_touch_move(modavd_handle h, int display, int contact, int x, int y, int pressure)
{
    (void)h, (void)display, (void)contact, (void)x, (void)y, (void)pressure;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_touch_up(modavd_handle h, int display, int contact)
{
    (void)h, (void)display, (void)contact;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_touch_release_all(modavd_handle h)
{
    (void)h;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_key(modavd_handle h, int display, int android_keycode, int down)
{
    (void)h, (void)display, (void)android_keycode, (void)down;
    return MODAVD_E_UNSUPPORTED;
}

int modavd_input_text(modavd_handle h, const char* utf8)
{
    (void)h, (void)utf8;
    return MODAVD_E_UNSUPPORTED;
}
