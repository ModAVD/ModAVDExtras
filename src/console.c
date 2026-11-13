/*
 * A minimal client for the emulator console (telnet-style text on 127.0.0.1).
 * Used to start frame sharing (one-off connections) and to send touch input
 * (a session kept open per handle).
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
#include <winsock2.h>
#include <windows.h>
typedef SOCKET mx_socket;
#define MX_BAD_SOCKET INVALID_SOCKET
#define mx_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int mx_socket;
#define MX_BAD_SOCKET (-1)
#define mx_close_socket close
#endif

/* A write to a closed console must not raise SIGPIPE in the host process. */
#if defined(MSG_NOSIGNAL)
#define MX_SEND_FLAGS MSG_NOSIGNAL
#else
#define MX_SEND_FLAGS 0
#endif

/* Reads ~/.emulator_console_auth_token into buf (first line). Empty if absent. */
static void read_auth_token(char* buf, size_t cap)
{
    buf[0] = '\0';
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    if (!home || !home[0]) {
        return;
    }
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/.emulator_console_auth_token", home);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        return;
    }
    size_t r = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[r] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
}

/* Reads until a line "OK" (returns 1) or a line starting with "KO" (returns 0).
 * Returns -1 on a socket error, a timeout or an overlong reply. */
static int read_until_ok(mx_socket s, char* buf, int cap)
{
    int len = 0;
    for (;;) {
        int n = (int)recv(s, buf + len, cap - 1 - len, 0);
        if (n <= 0) {
            return -1;
        }
        len += n;
        buf[len] = '\0';
        if (strncmp(buf, "OK", 2) == 0 || strstr(buf, "\nOK")) {
            return 1;
        }
        if (strncmp(buf, "KO", 2) == 0 || strstr(buf, "\nKO")) {
            return 0;
        }
        if (len >= cap - 1) {
            return -1;
        }
    }
}

static int send_line(mx_socket s, const char* line)
{
    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "%s\r\n", line);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return -1;
    }
    return send(s, cmd, n, MX_SEND_FLAGS) == n ? 0 : -1;
}

int mx_console_open(int port, int timeout_ms, mx_console* c)
{
    c->sock = (intptr_t)MX_BAD_SOCKET;
    char token[256];
    read_auth_token(token, sizeof(token));

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return MODAVD_E_IO;
    }
#endif
    mx_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == MX_BAD_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        return MODAVD_E_IO;
    }
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
#else
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char buf[4096];
    int rc = MODAVD_OK;
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 || read_until_ok(s, buf, sizeof(buf)) != 1) {
#ifdef _WIN32
        int err = WSAGetLastError();
#else
        int err = 0;
#endif
        mx_log(MODAVD_LOG_WARN, "console %d connect failed (%d)", port, err);
        rc = MODAVD_E_NOT_FOUND;
    }
    else if (token[0]) {
        char auth[300];
        snprintf(auth, sizeof(auth), "auth %s", token);
        if (send_line(s, auth) != 0 || read_until_ok(s, buf, sizeof(buf)) != 1) {
            mx_log(MODAVD_LOG_WARN, "console %d auth failed", port);
            rc = MODAVD_E_IO;
        }
    }
    if (rc != MODAVD_OK) {
        mx_close_socket(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return rc;
    }
    c->sock = (intptr_t)s;
    return MODAVD_OK;
}

int mx_console_send(mx_console* c, const char* command)
{
    if (c->sock == (intptr_t)MX_BAD_SOCKET) {
        return MODAVD_E_NOT_FOUND;
    }
    mx_socket s = (mx_socket)c->sock;
    char buf[4096];
    if (send_line(s, command) != 0) {
        mx_console_close(c);
        return MODAVD_E_NOT_FOUND;
    }
    int r = read_until_ok(s, buf, sizeof(buf));
    if (r < 0) {
        /* The emulator went away or stopped answering: the session is unusable. */
        mx_console_close(c);
        return MODAVD_E_NOT_FOUND;
    }
    return r == 1 ? MODAVD_OK : MODAVD_E_IO;
}

void mx_console_close(mx_console* c)
{
    if (c->sock == (intptr_t)MX_BAD_SOCKET) {
        return;
    }
    mx_socket s = (mx_socket)c->sock;
    send_line(s, "quit");
    mx_close_socket(s);
    c->sock = (intptr_t)MX_BAD_SOCKET;
#ifdef _WIN32
    WSACleanup();
#endif
}
