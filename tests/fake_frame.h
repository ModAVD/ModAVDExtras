/*
 * A fake shared frame, laid out like the emulator's, for tests that run
 * without an emulator. Uses console ports no emulator uses (65000 and up).
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef FAKE_FRAME_H
#define FAKE_FRAME_H

#include <stdint.h>
#include <string.h>

#include "mx_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct fake_frame {
    int port;
    uint8_t* view;
    size_t size;
#ifdef _WIN32
    HANDLE mapping;
#endif
} fake_frame;

/* Creates the region with room for max_w x max_h and writes a w x h frame:
 * pixel (x, y) is B = x, G = y, R = x ^ y, A = 0xFF (low bytes). */
static int fake_frame_create(fake_frame* f, int port, uint32_t max_w, uint32_t max_h, uint32_t w, uint32_t h)
{
    char name[64];
    mx_shm_name(port, name, sizeof(name));
    memset(f, 0, sizeof(*f));
    f->port = port;
    f->size = MX_HEADER_SIZE + (size_t)max_w * max_h * 4;
#ifdef _WIN32
    f->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)f->size, name);
    if (!f->mapping) {
        return -1;
    }
    f->view = (uint8_t*)MapViewOfFile(f->mapping, FILE_MAP_ALL_ACCESS, 0, 0, f->size);
    if (!f->view) {
        CloseHandle(f->mapping);
        return -1;
    }
#else
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)f->size) != 0) {
        close(fd);
        shm_unlink(name);
        return -1;
    }
    void* v = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (v == MAP_FAILED) {
        shm_unlink(name);
        return -1;
    }
    f->view = (uint8_t*)v;
#endif
    mx_video_info vi = { w, h, 60, 7, 123456789ull };
    memcpy(f->view, &vi, sizeof(vi));
    uint8_t* p = f->view + MX_HEADER_SIZE;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x, p += 4) {
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)y;
            p[2] = (uint8_t)(x ^ y);
            p[3] = 0xFF;
        }
    }
    return 0;
}

static void fake_frame_set_size(fake_frame* f, uint32_t w, uint32_t h)
{
    mx_video_info* vi = (mx_video_info*)f->view;
    vi->width = w;
    vi->height = h;
}

static void fake_frame_destroy(fake_frame* f)
{
#ifdef _WIN32
    if (f->view) {
        UnmapViewOfFile(f->view);
    }
    if (f->mapping) {
        CloseHandle(f->mapping);
    }
#else
    char name[64];
    mx_shm_name(f->port, name, sizeof(name));
    if (f->view) {
        munmap(f->view, f->size);
    }
    shm_unlink(name);
#endif
    memset(f, 0, sizeof(*f));
}

#ifdef _WIN32
/* Listens on 127.0.0.1:port without ever answering, so the library sees an
 * emulator (this process) on that console port. INVALID_SOCKET on failure. */
static SOCKET fake_listen(int port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return INVALID_SOCKET;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
        WSACleanup();
        return INVALID_SOCKET;
    }
    return s;
}

static void fake_unlisten(SOCKET s)
{
    if (s != INVALID_SOCKET) {
        closesocket(s);
        WSACleanup();
    }
}
#endif

#endif /* FAKE_FRAME_H */
