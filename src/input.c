/*
 * Touch input through the emulator console: "event send" with raw multi-touch
 * events (protocol B: slot, tracking id, position, pressure), which the
 * emulator hands to the virtio touch screen of display 0 as they are. They
 * reach Android as touches on that screen, like a finger on the window, not
 * as events injected by an app.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mx_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK mx_mutex;
#define mx_mutex_init(m) InitializeSRWLock(m)
#define mx_mutex_destroy(m) ((void)(m))
#define mx_mutex_lock(m) AcquireSRWLockExclusive(m)
#define mx_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#else
#include <pthread.h>
typedef pthread_mutex_t mx_mutex;
#define mx_mutex_init(m) pthread_mutex_init((m), NULL)
#define mx_mutex_destroy(m) pthread_mutex_destroy(m)
#define mx_mutex_lock(m) pthread_mutex_lock(m)
#define mx_mutex_unlock(m) pthread_mutex_unlock(m)
#endif

/* Linux input codes (input-event-codes.h); the console takes numbers as well as names. */
#define EV_SYN_ 0
#define EV_ABS_ 3
#define ABS_MT_SLOT_ 0x2f
#define ABS_MT_TOUCH_MAJOR_ 0x30
#define ABS_MT_TOUCH_MINOR_ 0x31
#define ABS_MT_POSITION_X_ 0x35
#define ABS_MT_POSITION_Y_ 0x36
#define ABS_MT_TRACKING_ID_ 0x39
#define ABS_MT_PRESSURE_ 0x3a

#define MX_INPUT_TIMEOUT_MS 1000

struct mx_input {
    mx_mutex lock;
    int port;
    mx_console console;
    uint32_t down; /* bit n: contact n is pressed */
};

int mx_touch_axis(int v, int size)
{
    if (size <= 0 || v <= 0) {
        return 0;
    }
    if (v >= size) {
        v = size - 1;
    }
    /* qemu_input_scale_axis(v, 0, size, 0, MX_ABS_MAX), as for "event mouse" */
    return (int)((int64_t)v * MX_ABS_MAX / size);
}

int mx_touch_command(char* buf, size_t cap, int action, int contact, int ax, int ay, int pressure)
{
    int n = -1;
    switch (action) {
    case MX_TOUCH_DOWN:
        n = snprintf(buf, cap, "event send %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:0",
                     EV_ABS_, ABS_MT_SLOT_, contact, EV_ABS_, ABS_MT_TRACKING_ID_, contact, EV_ABS_,
                     ABS_MT_POSITION_X_, ax, EV_ABS_, ABS_MT_POSITION_Y_, ay, EV_ABS_, ABS_MT_PRESSURE_, pressure,
                     EV_ABS_, ABS_MT_TOUCH_MAJOR_, MX_TOUCH_SIZE, EV_ABS_, ABS_MT_TOUCH_MINOR_, MX_TOUCH_SIZE, EV_SYN_,
                     0);
        break;
    case MX_TOUCH_MOVE:
        n = snprintf(buf, cap, "event send %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:%d %d:%d:0", EV_ABS_, ABS_MT_SLOT_,
                     contact, EV_ABS_, ABS_MT_POSITION_X_, ax, EV_ABS_, ABS_MT_POSITION_Y_, ay, EV_ABS_,
                     ABS_MT_PRESSURE_, pressure, EV_SYN_, 0);
        break;
    case MX_TOUCH_UP:
        n = snprintf(buf, cap, "event send %d:%d:%d %d:%d:0 %d:%d:-1 %d:%d:0", EV_ABS_, ABS_MT_SLOT_, contact, EV_ABS_,
                     ABS_MT_PRESSURE_, EV_ABS_, ABS_MT_TRACKING_ID_, EV_SYN_, 0);
        break;
    default: break;
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

struct mx_input* mx_input_create(int console_port)
{
    struct mx_input* in = (struct mx_input*)calloc(1, sizeof(*in));
    if (!in) {
        return NULL;
    }
    mx_mutex_init(&in->lock);
    in->port = console_port;
    in->console.sock = -1;
    return in;
}

/* Sends one command on the session, opening it first and once more if it broke
 * (the emulator restarted, or closed an idle session). Caller holds the lock. */
static int send_locked(struct mx_input* in, const char* command)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (in->console.sock == -1) {
            int rc = mx_console_open(in->port, MX_INPUT_TIMEOUT_MS, &in->console);
            if (rc != MODAVD_OK) {
                return rc;
            }
        }
        int rc = mx_console_send(&in->console, command);
        if (rc != MODAVD_E_NOT_FOUND) {
            if (rc != MODAVD_OK) {
                mx_log(MODAVD_LOG_WARN, "console %d: %s -> KO", in->port, command);
            }
            return rc;
        }
    }
    return MODAVD_E_NOT_FOUND;
}

/* Lifts every pressed contact. Caller holds the lock. */
static int release_all_locked(struct mx_input* in)
{
    int result = MODAVD_OK;
    for (int c = 0; c < MX_MAX_CONTACTS; ++c) {
        if (!(in->down & (1u << c))) {
            continue;
        }
        char cmd[256];
        mx_touch_command(cmd, sizeof(cmd), MX_TOUCH_UP, c, 0, 0, 0);
        int rc = send_locked(in, cmd);
        if (rc != MODAVD_OK) {
            result = rc;
        }
        in->down &= ~(1u << c);
    }
    return result;
}

void mx_input_destroy(struct mx_input* in)
{
    if (!in) {
        return;
    }
    mx_mutex_lock(&in->lock);
    if (in->down) {
        release_all_locked(in);
    }
    mx_console_close(&in->console);
    mx_mutex_unlock(&in->lock);
    mx_mutex_destroy(&in->lock);
    free(in);
}

/* Display size from the shared frame header; 0 when nothing was published. */
static void display_size(modavd_handle h, int* w, int* hgt)
{
    const volatile mx_video_info* vi = (const volatile mx_video_info*)h->shm.view;
    *w = vi ? (int)vi->width : 0;
    *hgt = vi ? (int)vi->height : 0;
}

static int touch(modavd_handle h, int display, int action, int contact, int x, int y, int pressure)
{
    if (!h || !h->input) {
        return MODAVD_E_INVALID;
    }
    if (display != 0) {
        return MODAVD_E_UNSUPPORTED;
    }
    if (contact < 0 || contact >= MX_MAX_CONTACTS) {
        return MODAVD_E_INVALID;
    }
    struct mx_input* in = h->input;
    uint32_t bit = 1u << contact;
    char cmd[256];
    int rc = MODAVD_OK;

    mx_mutex_lock(&in->lock);
    if (action == MX_TOUCH_UP) {
        /* Lifting a contact that is not down sends nothing. */
        if (in->down & bit) {
            mx_touch_command(cmd, sizeof(cmd), MX_TOUCH_UP, contact, 0, 0, 0);
            rc = send_locked(in, cmd);
            in->down &= ~bit;
        }
        mx_mutex_unlock(&in->lock);
        return rc;
    }

    int w = 0, hgt = 0;
    display_size(h, &w, &hgt);
    if (w <= 0 || hgt <= 0) {
        mx_mutex_unlock(&in->lock);
        return MODAVD_E_NOT_READY;
    }
    if (action == MX_TOUCH_MOVE && !(in->down & bit)) {
        mx_mutex_unlock(&in->lock);
        return MODAVD_E_INVALID;
    }
    if (pressure <= 0 || pressure > MX_TOUCH_PRESSURE) {
        pressure = MX_TOUCH_PRESSURE;
    }
    /* Pressing a contact that is already down moves it, as MuMu's library does. */
    int kind = (action == MX_TOUCH_DOWN && !(in->down & bit)) ? MX_TOUCH_DOWN : MX_TOUCH_MOVE;
    mx_touch_command(cmd, sizeof(cmd), kind, contact, mx_touch_axis(x, w), mx_touch_axis(y, hgt), pressure);
    rc = send_locked(in, cmd);
    if (rc == MODAVD_OK && kind == MX_TOUCH_DOWN) {
        in->down |= bit;
    }
    mx_mutex_unlock(&in->lock);
    return rc;
}

int modavd_touch_down(modavd_handle h, int display, int contact, int x, int y, int pressure)
{
    return touch(h, display, MX_TOUCH_DOWN, contact, x, y, pressure);
}

int modavd_touch_move(modavd_handle h, int display, int contact, int x, int y, int pressure)
{
    return touch(h, display, MX_TOUCH_MOVE, contact, x, y, pressure);
}

int modavd_touch_up(modavd_handle h, int display, int contact)
{
    return touch(h, display, MX_TOUCH_UP, contact, 0, 0, 0);
}

int modavd_touch_release_all(modavd_handle h)
{
    if (!h || !h->input) {
        return MODAVD_E_INVALID;
    }
    mx_mutex_lock(&h->input->lock);
    int rc = release_all_locked(h->input);
    mx_mutex_unlock(&h->input->lock);
    return rc;
}

/* The console's "event send" delivers keys to the touch screen device, which
 * drops them, and "event text" silently drops what the keyboard map lacks:
 * neither is reliable enough yet. */
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
