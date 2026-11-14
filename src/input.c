/*
 * Input through the emulator console. Touch: "event send" with raw multi-touch
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
#include <time.h>
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
    int key_cmd;   /* the console of this session has "event key": 1 yes, -1 no, 0 not asked */
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
static int query_locked(struct mx_input* in, const char* command, char* reply, size_t cap)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (in->console.sock == -1) {
            int rc = mx_console_open(in->port, MX_INPUT_TIMEOUT_MS, &in->console);
            if (rc != MODAVD_OK) {
                return rc;
            }
            in->key_cmd = 0; /* maybe another emulator now */
        }
        int rc = mx_console_query(&in->console, command, reply, cap);
        if (rc != MODAVD_E_NOT_FOUND) {
            return rc;
        }
    }
    return MODAVD_E_NOT_FOUND;
}

static int send_locked(struct mx_input* in, const char* command)
{
    int rc = query_locked(in, command, NULL, 0);
    if (rc == MODAVD_E_IO) {
        mx_log(MODAVD_LOG_WARN, "console %d: %s -> KO", in->port, command);
    }
    return rc;
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

/* ---- keys and text ----
 *
 * Three console commands carry them:
 * - "event key <linux code> <0|1>" presses or releases a key of the emulator's
 *   keyboard. ModAVD emulators newer than 0.40 have it; it is the only way to
 *   BACK, HOME and the other keys that type nothing.
 * - "event text <text>" types on that keyboard, every emulator has it. It can
 *   type printable ASCII, and drops a whole burst when more keys arrive than
 *   the guest keyboard's queue holds, so the text goes a few characters at a
 *   time.
 * - "ime commit <text>" / "ime key <code>" hand text and editing keys to the
 *   focused text field through the host input method (org.modavd.ime, ModAVD
 *   0.35.0 and later). Any language; needs that input method to be the
 *   current one and a text field to have the focus ("ime status").
 */

/* The emulator's keyboard, as the guest reads it (/vendor/usr/keylayout/qwerty.kl of the
 * goldfish images): the Android key code, the Linux code of its key, and the character
 * it types unshifted. Generic.kl does not apply to it: HOME is key 102 here. */
static const struct {
    short android;
    short linux_code;
    char ch;
} kKeys[] = {
    { 1, 60, 0 },     /* SOFT_LEFT */
    { 2, 230, 0 },    /* SOFT_RIGHT */
    { 3, 102, 0 },    /* HOME */
    { 4, 158, 0 },    /* BACK */
    { 5, 231, 0 },    /* CALL */
    { 6, 107, 0 },    /* ENDCALL */
    { 7, 11, '0' },   { 8, 2, '1' },    { 9, 3, '2' },    { 10, 4, '3' },   { 11, 5, '4' },
    { 12, 6, '5' },   { 13, 7, '6' },   { 14, 8, '7' },   { 15, 9, '8' },   { 16, 10, '9' },
    { 17, 227, 0 },   /* STAR */
    { 18, 228, 0 },   /* POUND */
    { 19, 103, 0 },   /* DPAD_UP */
    { 20, 108, 0 },   /* DPAD_DOWN */
    { 21, 105, 0 },   /* DPAD_LEFT */
    { 22, 106, 0 },   /* DPAD_RIGHT */
    { 23, 232, 0 },   /* DPAD_CENTER */
    { 24, 115, 0 },   /* VOLUME_UP */
    { 25, 114, 0 },   /* VOLUME_DOWN */
    { 26, 116, 0 },   /* POWER */
    { 27, 212, 0 },   /* CAMERA */
    { 29, 30, 'a' },  { 30, 48, 'b' },  { 31, 46, 'c' },  { 32, 32, 'd' },  { 33, 18, 'e' },
    { 34, 33, 'f' },  { 35, 34, 'g' },  { 36, 35, 'h' },  { 37, 23, 'i' },  { 38, 36, 'j' },
    { 39, 37, 'k' },  { 40, 38, 'l' },  { 41, 50, 'm' },  { 42, 49, 'n' },  { 43, 24, 'o' },
    { 44, 25, 'p' },  { 45, 16, 'q' },  { 46, 19, 'r' },  { 47, 31, 's' },  { 48, 20, 't' },
    { 49, 22, 'u' },  { 50, 47, 'v' },  { 51, 17, 'w' },  { 52, 45, 'x' },  { 53, 21, 'y' },
    { 54, 44, 'z' },
    { 55, 51, ',' },  { 56, 52, '.' },
    { 57, 56, 0 },    /* ALT_LEFT */
    { 58, 100, 0 },   /* ALT_RIGHT */
    { 59, 42, 0 },    /* SHIFT_LEFT */
    { 60, 54, 0 },    /* SHIFT_RIGHT */
    { 61, 15, '\t' }, { 62, 57, ' ' },
    { 64, 150, 0 },   /* EXPLORER */
    { 65, 155, 0 },   /* ENVELOPE */
    { 66, 28, '\n' },
    { 67, 14, 0 },    /* DEL (backspace) */
    { 68, 399, '`' }, { 69, 12, '-' },  { 70, 13, '=' },  { 71, 26, '[' },  { 72, 27, ']' },
    { 73, 43, '\\' }, { 74, 39, ';' },  { 75, 40, '\'' }, { 76, 53, '/' },
    { 77, 215, 0 },   /* AT */
    { 82, 139, 0 },   /* MENU */
    { 84, 217, 0 },   /* SEARCH */
    { 85, 164, 0 },   /* MEDIA_PLAY_PAUSE */
    { 86, 166, 0 },   /* MEDIA_STOP */
    { 87, 163, 0 },   /* MEDIA_NEXT */
    { 88, 165, 0 },   /* MEDIA_PREVIOUS */
    { 89, 168, 0 },   /* MEDIA_REWIND */
    { 111, 1, 0 },    /* ESCAPE: the keyboard's Esc key, which this layout reads as BACK */
    { 113, 29, 0 },   /* CTRL_LEFT */
    { 114, 97, 0 },   /* CTRL_RIGHT */
    { 115, 58, 0 },   /* CAPS_LOCK */
    { 187, 580, 0 },  /* APP_SWITCH */
    { 223, 142, 0 },  /* SLEEP */
};

int mx_key_linux_code(int android_keycode)
{
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        if (kKeys[i].android == android_keycode) {
            return kKeys[i].linux_code;
        }
    }
    return 0;
}

int mx_key_char(int android_keycode)
{
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        if (kKeys[i].android == android_keycode) {
            return kKeys[i].ch;
        }
    }
    return 0;
}

/* Keys the host input method may pass to the focused text field on an emulator
 * without "event key": the editing keys. The others either type a character
 * (the keyboard does that) or mean nothing to a text field. */
static int is_editing_key(int android_keycode)
{
    switch (android_keycode) {
    case 19: case 20: case 21: case 22: /* DPAD */
    case 67:                            /* DEL */
    case 112:                           /* FORWARD_DEL */
    case 122: case 123:                 /* MOVE_HOME, MOVE_END */
        return 1;
    default: return 0;
    }
}

int mx_text_is_keyboard(const char* utf8)
{
    for (const unsigned char* p = (const unsigned char*)utf8; *p; ++p) {
        if (!((*p >= 0x20 && *p <= 0x7e) || *p == '\n' || *p == '\t')) {
            return 0;
        }
    }
    return 1;
}

int mx_text_keyboard_command(char* buf, size_t cap, const char** text)
{
    const char* p = *text;
    if (!*p) {
        return 0;
    }
    int n = snprintf(buf, cap, "event text ");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    for (int taken = 0; *p && taken < MX_KEYBOARD_CHUNK; ++p, ++taken) {
        /* The console skips leading spaces and reads '\' as an escape. */
        const char* fmt = "%c";
        if (*p == ' ' || *p == '\t') {
            fmt = "\\x%02x";
        }
        else if (*p == '\n') {
            fmt = "\\n";
        }
        else if (*p == '\\') {
            fmt = "\\\\";
        }
        int w = snprintf(buf + n, cap - (size_t)n, fmt, (unsigned char)*p);
        if (w < 0 || (size_t)w >= cap - (size_t)n) {
            return -1;
        }
        n += w;
    }
    *text = p;
    return n;
}

int mx_text_commit_command(char* buf, size_t cap, const char** text, int* newline)
{
    const char* p = *text;
    size_t len = 0;
    *newline = 0;
    while (p[len] && p[len] != '\n' && len < MX_COMMIT_CHUNK) {
        ++len;
    }
    if (p[len] && p[len] != '\n') {
        /* Cut in front of a UTF-8 sequence, not inside one. */
        while (len > 0 && ((unsigned char)p[len] & 0xc0) == 0x80) {
            --len;
        }
    }
    int n = 0;
    if (len > 0) {
        n = snprintf(buf, cap, "ime commit %.*s", (int)len, p);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
    }
    if (p[len] == '\n') {
        *newline = 1;
        ++len;
    }
    *text = p + len;
    return n;
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* 1 when the console has "event key" (asked once per session), 0 when not, or a MODAVD_E_* code. */
static int key_command_locked(struct mx_input* in)
{
    if (in->console.sock != -1 && in->key_cmd != 0) {
        return in->key_cmd > 0;
    }
    /* Without arguments it presses nothing: the usage if the command is there, "bad sub-command" if not. */
    char reply[512];
    int rc = query_locked(in, "event key", reply, sizeof(reply));
    if (rc == MODAVD_E_NOT_FOUND) {
        return rc;
    }
    in->key_cmd = strstr(reply, "usage: event key") ? 1 : -1;
    return in->key_cmd > 0;
}

/* 1 when the host input method is connected and a text field has the focus, 0 when not, or a MODAVD_E_* code. */
static int ime_ready_locked(struct mx_input* in)
{
    char reply[512];
    int rc = query_locked(in, "ime status", reply, sizeof(reply));
    if (rc == MODAVD_E_NOT_FOUND) {
        return rc;
    }
    return rc == MODAVD_OK && strstr(reply, "connected=1") && strstr(reply, "active=1");
}

uint32_t mx_input_capabilities(struct mx_input* in)
{
    mx_mutex_lock(&in->lock);
    int has = key_command_locked(in);
    mx_mutex_unlock(&in->lock);
    return has > 0 ? MODAVD_CAP_KEY : 0;
}

int modavd_key(modavd_handle h, int display, int android_keycode, int down)
{
    if (!h || !h->input || android_keycode <= 0) {
        return MODAVD_E_INVALID;
    }
    if (display != 0) {
        return MODAVD_E_UNSUPPORTED;
    }
    struct mx_input* in = h->input;
    char cmd[64];
    int rc;

    mx_mutex_lock(&in->lock);
    int has = key_command_locked(in);
    if (has < 0) {
        rc = has;
    }
    else if (has && mx_key_linux_code(android_keycode)) {
        snprintf(cmd, sizeof(cmd), "event key %d %d", mx_key_linux_code(android_keycode), down ? 1 : 0);
        rc = send_locked(in, cmd);
    }
    else {
        /* No "event key", or a key the keyboard lacks: the press types the
         * key's character or goes to the focused text field as a whole; the
         * release has nothing left to do. */
        int ch = mx_key_char(android_keycode);
        if (ch) {
            const char one[2] = { (char)ch, '\0' };
            const char* p = one;
            rc = MODAVD_OK;
            if (down && mx_text_keyboard_command(cmd, sizeof(cmd), &p) > 0) {
                rc = send_locked(in, cmd);
            }
        }
        else if (is_editing_key(android_keycode)) {
            int ready = ime_ready_locked(in);
            if (ready < 0) {
                rc = ready;
            }
            else if (!ready) {
                rc = MODAVD_E_UNSUPPORTED;
            }
            else if (down) {
                snprintf(cmd, sizeof(cmd), "ime key %d", android_keycode);
                rc = send_locked(in, cmd);
            }
            else {
                rc = MODAVD_OK;
            }
        }
        else {
            rc = MODAVD_E_UNSUPPORTED;
        }
    }
    mx_mutex_unlock(&in->lock);
    return rc;
}

int modavd_input_text(modavd_handle h, const char* utf8)
{
    if (!h || !h->input || !utf8) {
        return MODAVD_E_INVALID;
    }
    for (const unsigned char* p = (const unsigned char*)utf8; *p; ++p) {
        if (*p < 0x20 && *p != '\n' && *p != '\t') {
            return MODAVD_E_INVALID; /* the console reads a line; other control characters have no key */
        }
    }
    if (!utf8[0]) {
        return MODAVD_OK;
    }
    struct mx_input* in = h->input;
    char cmd[MX_CONSOLE_LINE_MAX + 1];
    const char* p = utf8;
    int rc = MODAVD_OK;

    mx_mutex_lock(&in->lock);
    int ready = ime_ready_locked(in);
    if (ready < 0) {
        rc = ready;
    }
    else if (ready) {
        /* The input method hands the text to the field as it is. */
        while (*p && rc == MODAVD_OK) {
            int newline = 0;
            int n = mx_text_commit_command(cmd, sizeof(cmd), &p, &newline);
            if (n < 0) {
                rc = MODAVD_E_INVALID;
                break;
            }
            if (n > 0) {
                rc = send_locked(in, cmd);
            }
            if (rc == MODAVD_OK && newline) {
                rc = send_locked(in, "ime key 66"); /* ENTER */
            }
        }
    }
    else if (mx_text_is_keyboard(utf8)) {
        for (;;) {
            int n = mx_text_keyboard_command(cmd, sizeof(cmd), &p);
            if (n <= 0) {
                rc = n < 0 ? MODAVD_E_INVALID : MODAVD_OK;
                break;
            }
            rc = send_locked(in, cmd);
            if (rc != MODAVD_OK) {
                break;
            }
            sleep_ms(MX_KEYBOARD_GAP_MS);
        }
    }
    else {
        rc = MODAVD_E_UNSUPPORTED; /* no text field with the host input method, and not ASCII */
    }
    mx_mutex_unlock(&in->lock);
    return rc;
}
