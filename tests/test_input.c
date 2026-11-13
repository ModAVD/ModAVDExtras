/*
 * Touch input against a fake emulator console: the commands sent for each
 * action, coordinate scaling, reconnecting after the console drops, lifting
 * contacts on close, and the auth line.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_frame.h"
#include "modavd_extras.h"

#ifdef _WIN32
#include <winsock2.h>
#include <direct.h>
typedef SOCKET sock_t;
typedef int socklen_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

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

#define CHECK_STR(a, b)                                                                                               \
    do {                                                                                                              \
        const char *sa_ = (a), *sb_ = (b);                                                                            \
        if (strcmp(sa_, sb_) != 0) {                                                                                  \
            fprintf(stderr, "%s:%d: CHECK_STR failed:\n  got  %s\n  want %s\n", __FILE__, __LINE__, sa_, sb_);        \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

/* ---- a fake console: replies OK to every line, records what it got ---- */

#define MAX_LINES 64

static struct {
    sock_t listener;
    int port;
    char lines[MAX_LINES][256];
    volatile int count;
    volatile int connections;
    volatile int drop_next; /* close without replying to the next command */
    volatile int stop;
} g_con;

static void record(const char* line)
{
    if (g_con.count < MAX_LINES) {
        size_t n = strlen(line);
        if (n >= sizeof(g_con.lines[0])) {
            n = sizeof(g_con.lines[0]) - 1;
        }
        memcpy(g_con.lines[g_con.count], line, n);
        g_con.lines[g_con.count][n] = '\0';
        g_con.count = g_con.count + 1;
    }
}

static void serve_client(sock_t c)
{
    static const char kBanner[] = "Android Console: fake\r\nOK\r\n";
    send(c, kBanner, (int)strlen(kBanner), 0);
    char buf[2048];
    int len = 0;
    for (;;) {
        int n = (int)recv(c, buf + len, (int)sizeof(buf) - 1 - len, 0);
        if (n <= 0) {
            return;
        }
        len += n;
        buf[len] = '\0';
        char* eol;
        while ((eol = strstr(buf, "\r\n")) != NULL) {
            *eol = '\0';
            record(buf);
            int quit = strcmp(buf, "quit") == 0;
            int drop = !quit && strncmp(buf, "auth ", 5) != 0 && g_con.drop_next;
            int rest = len - (int)(eol + 2 - buf);
            memmove(buf, eol + 2, (size_t)rest + 1);
            len = rest;
            if (quit) {
                return;
            }
            if (drop) {
                g_con.drop_next = 0;
                return;
            }
            send(c, "OK\r\n", 4, 0);
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI server_main(LPVOID arg)
#else
static void* server_main(void* arg)
#endif
{
    (void)arg;
    while (!g_con.stop) {
        sock_t c = accept(g_con.listener, NULL, NULL);
        if (c == BAD_SOCK || g_con.stop) {
            if (c != BAD_SOCK) {
                close_sock(c);
            }
            break;
        }
        g_con.connections = g_con.connections + 1;
        serve_client(c);
        close_sock(c);
    }
    return 0;
}

static int start_console(void)
{
    g_con.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_con.listener == BAD_SOCK) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof(addr);
    if (bind(g_con.listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(g_con.listener, 4) != 0 ||
        getsockname(g_con.listener, (struct sockaddr*)&addr, &alen) != 0) {
        return -1;
    }
    g_con.port = ntohs(addr.sin_port);
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, server_main, NULL, 0, NULL);
    if (!t) {
        return -1;
    }
    CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, server_main, NULL) != 0) {
        return -1;
    }
    pthread_detach(t);
#endif
    return 0;
}

static void stop_console(void)
{
    g_con.stop = 1;
    /* Wake the blocked accept() with a connection of our own. */
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_con.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(s, (struct sockaddr*)&addr, sizeof(addr));
    close_sock(s);
    sleep_ms(50);
    close_sock(g_con.listener);
}

/* Waits until the console has recorded n lines (the "quit" of a close is not replied to). */
static int wait_lines(int n)
{
    for (int i = 0; i < 200 && g_con.count < n; ++i) {
        sleep_ms(10);
    }
    return g_con.count;
}

/* Points the console auth token at a file holding "secret". */
static int use_auth_token(void)
{
    static const char kHome[] = "test_input_home";
#ifdef _WIN32
    _mkdir(kHome);
    char full[1024];
    if (!_fullpath(full, kHome, sizeof(full)) || _putenv_s("USERPROFILE", full) != 0) {
        return -1;
    }
#else
    mkdir(kHome, 0700);
    /* realpath() needs PATH_MAX bytes (4096 on Linux); fortified glibc aborts on less. */
    char full[PATH_MAX];
    if (!realpath(kHome, full) || setenv("HOME", full, 1) != 0) {
        return -1;
    }
#endif
    char path[sizeof(full) + 64];
    snprintf(path, sizeof(path), "%s/.emulator_console_auth_token", full);
    FILE* f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    fputs("secret\n", f);
    fclose(f);
    return 0;
}

/* ---- tests ---- */

static void test_axis_and_commands(void)
{
    CHECK_EQ(mx_touch_axis(0, 1280), 0);
    CHECK_EQ(mx_touch_axis(-5, 1280), 0);
    CHECK_EQ(mx_touch_axis(640, 1280), 16383);
    CHECK_EQ(mx_touch_axis(1100, 1280), 28159); /* what the emulator sent for "event mouse 1100 ..." */
    CHECK_EQ(mx_touch_axis(1279, 1280), 32741);
    CHECK_EQ(mx_touch_axis(5000, 1280), 32741); /* clamped to the last pixel */
    CHECK_EQ(mx_touch_axis(10, 0), 0);

    char buf[256];
    CHECK(mx_touch_command(buf, sizeof(buf), MX_TOUCH_DOWN, 2, 100, 200, 1024) > 0);
    CHECK_STR(buf, "event send 3:47:2 3:57:2 3:53:100 3:54:200 3:58:1024 3:48:1280 3:49:1280 0:0:0");
    CHECK(mx_touch_command(buf, sizeof(buf), MX_TOUCH_MOVE, 2, 300, 400, 512) > 0);
    CHECK_STR(buf, "event send 3:47:2 3:53:300 3:54:400 3:58:512 0:0:0");
    CHECK(mx_touch_command(buf, sizeof(buf), MX_TOUCH_UP, 2, 0, 0, 0) > 0);
    CHECK_STR(buf, "event send 3:47:2 3:58:0 3:57:-1 0:0:0");
    CHECK_EQ(mx_touch_command(buf, 10, MX_TOUCH_DOWN, 0, 1, 1, 1), -1);
    CHECK_EQ(mx_touch_command(buf, sizeof(buf), 9, 0, 1, 1, 1), -1);
}

static void test_touch_session(void)
{
    if (start_console() != 0 || use_auth_token() != 0) {
        fprintf(stderr, "fake console setup failed\n");
        ++g_failures;
        return;
    }
    fake_frame f;
    if (fake_frame_create(&f, g_con.port, 1280, 720, 1280, 720) != 0) {
        fprintf(stderr, "fake frame setup failed\n");
        ++g_failures;
        stop_console();
        return;
    }

    modavd_open_params p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.console_port = g_con.port;
    p.flags = MODAVD_OPEN_NO_START;
    modavd_handle h = NULL;
    CHECK_EQ(modavd_open(&p, &h), MODAVD_OK);
    if (!h) {
        fake_frame_destroy(&f);
        stop_console();
        return;
    }
    CHECK(modavd_capabilities(h) & MODAVD_CAP_TOUCH);
    CHECK_EQ(g_con.connections, 0); /* the session opens on the first touch */

    /* tap at the centre: auth, down, up on one session */
    CHECK_EQ(modavd_touch_down(h, 0, 0, 640, 360, 0), MODAVD_OK);
    CHECK_EQ(modavd_touch_up(h, 0, 0), MODAVD_OK);
    CHECK_EQ(g_con.connections, 1);
    CHECK_EQ(g_con.count, 3);
    CHECK_STR(g_con.lines[0], "auth secret");
    CHECK_STR(g_con.lines[1], "event send 3:47:0 3:57:0 3:53:16383 3:54:16383 3:58:1024 3:48:1280 3:49:1280 0:0:0");
    CHECK_STR(g_con.lines[2], "event send 3:47:0 3:58:0 3:57:-1 0:0:0");

    /* lifting it again sends nothing; moving a lifted contact is an error */
    CHECK_EQ(modavd_touch_up(h, 0, 0), MODAVD_OK);
    CHECK_EQ(modavd_touch_move(h, 0, 0, 1, 1, 0), MODAVD_E_INVALID);
    CHECK_EQ(g_con.count, 3);

    /* arguments */
    CHECK_EQ(modavd_touch_down(h, 0, MX_MAX_CONTACTS, 1, 1, 0), MODAVD_E_INVALID);
    CHECK_EQ(modavd_touch_down(h, 0, -1, 1, 1, 0), MODAVD_E_INVALID);
    CHECK_EQ(modavd_touch_down(h, 1, 0, 1, 1, 0), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_touch_down(NULL, 0, 0, 1, 1, 0), MODAVD_E_INVALID);
    CHECK_EQ(modavd_key(h, 0, 4, 1), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(modavd_input_text(h, "a"), MODAVD_E_UNSUPPORTED);
    CHECK_EQ(g_con.count, 3);

    /* a swipe with its own pressure, clamped to the screen; pressing again moves */
    CHECK_EQ(modavd_touch_down(h, 0, 3, 100, 100, 500), MODAVD_OK);
    CHECK_EQ(modavd_touch_move(h, 0, 3, 5000, 719, 0), MODAVD_OK);
    CHECK_EQ(modavd_touch_down(h, 0, 3, 640, 360, 0), MODAVD_OK);
    CHECK_STR(g_con.lines[3], "event send 3:47:3 3:57:3 3:53:2559 3:54:4550 3:58:500 3:48:1280 3:49:1280 0:0:0");
    CHECK_STR(g_con.lines[4], "event send 3:47:3 3:53:32741 3:54:32721 3:58:1024 0:0:0");
    CHECK_STR(g_con.lines[5], "event send 3:47:3 3:53:16383 3:54:16383 3:58:1024 0:0:0");

    /* two fingers, then release all lifts both */
    CHECK_EQ(modavd_touch_down(h, 0, 1, 10, 10, 0), MODAVD_OK);
    CHECK_EQ(modavd_touch_release_all(h), MODAVD_OK);
    CHECK_EQ(g_con.count, 9);
    CHECK_STR(g_con.lines[7], "event send 3:47:1 3:58:0 3:57:-1 0:0:0");
    CHECK_STR(g_con.lines[8], "event send 3:47:3 3:58:0 3:57:-1 0:0:0");

    /* nothing published yet: no size to scale with */
    fake_frame_set_size(&f, 0, 0);
    CHECK_EQ(modavd_touch_down(h, 0, 0, 1, 1, 0), MODAVD_E_NOT_READY);
    fake_frame_set_size(&f, 1280, 720);
    CHECK_EQ(g_con.count, 9);

    /* the console drops the session mid-command: reconnect once and resend */
    g_con.drop_next = 1;
    CHECK_EQ(modavd_touch_down(h, 0, 4, 0, 0, 0), MODAVD_OK);
    CHECK_EQ(g_con.connections, 2);
    CHECK_EQ(g_con.count, 12);
    CHECK_STR(g_con.lines[9], g_con.lines[11]);
    CHECK_STR(g_con.lines[10], "auth secret");

    /* closing lifts contact 4, then quits */
    modavd_close(h);
    CHECK_EQ(wait_lines(14), 14);
    CHECK_STR(g_con.lines[12], "event send 3:47:4 3:58:0 3:57:-1 0:0:0");
    CHECK_STR(g_con.lines[13], "quit");

    fake_frame_destroy(&f);
    stop_console();
}

int main(void)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_axis_and_commands();
    test_touch_session();
#ifdef _WIN32
    WSACleanup();
#endif
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_input: all passed\n");
    return 0;
}
