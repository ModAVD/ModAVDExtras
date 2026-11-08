/*
 * external_renderer_ipc.dll: the MuMu-shaped compatibility shell.
 *
 * MaaCore versions that do not know ModAVD only capture through MuMu's
 * external_renderer_ipc.dll: they LoadLibrary it from <MuMu path>/shell/sdk/
 * and look up nemu_connect / nemu_disconnect / nemu_get_display_id /
 * nemu_capture_display by name. This DLL exports those four functions on top
 * of the ModAVDExtras core (built in statically), so frames come from the
 * ModAVD instance's shared frame. It replaces the earlier avdshim and behaves
 * the same way:
 *
 * - nemu_connect(path, index) maps index to the console port (index_to_port),
 *   writes its log to <path>\avdshim.log and starts frame sharing through the
 *   console when it is off.
 * - nemu_capture_display returns 0 on success, RGBA rows bottom-up (MaaCore
 *   converts RGBA to BGR and flips); with buffer_size 0 it only reports the size.
 * - Input functions are not exported, so MaaCore keeps adb / maatouch for input.
 *
 * Windows only. Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "mx_internal.h"

#define EXPORT __declspec(dllexport)
#define MAX_SLOTS 16
#define DEFAULT_PORT_BASE 20000
#define PORT_STEP 10

static modavd_handle g_slots[MAX_SLOTS];
static int g_slot_ports[MAX_SLOTS];
static CRITICAL_SECTION g_lock;
static wchar_t g_log_path[MAX_PATH];

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_lock);
    }
    return TRUE;
}

static void shim_write_log(const char* msg)
{
    if (!g_log_path[0]) {
        return;
    }
    FILE* f = _wfopen(g_log_path, L"a");
    if (!f) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

static void shim_log(const char* fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    shim_write_log(msg);
}

/* Only the library's warnings go to avdshim.log: one line per connect, like before. */
static void core_log(int level, const char* msg, void* user)
{
    (void)user;
    if (level <= MODAVD_LOG_WARN) {
        shim_write_log(msg);
    }
}

/*   index >= 20000                     -> index is the emulator console port (what AUTO-MAS passes)
 *   5554 + 2 * index in 20000..20100   -> MAA's GUI derives index = (port - 5554) / 2 from "emulator-<port>"
 *   index >= 5554                      -> index is the emulator console port
 *   otherwise                          -> AUTO-MAS native index, console port = 20000 + 10 * index */
static int index_to_port(int index)
{
    if (index >= 20000) {
        return index;
    }
    if (index >= 5554) {
        int maa = 5554 + 2 * index;
        return (maa >= 20000 && maa <= 20100) ? maa : index;
    }
    return DEFAULT_PORT_BASE + PORT_STEP * index;
}

static modavd_handle get_slot(int handle)
{
    if (handle < 1 || handle > MAX_SLOTS) {
        return NULL;
    }
    return g_slots[handle - 1];
}

EXPORT int nemu_connect(const wchar_t* path, int index)
{
    if (path && path[0] && wcslen(path) < MAX_PATH - 16) {
        wcscpy(g_log_path, path);
        wcscat(g_log_path, L"\\avdshim.log");
    }
    modavd_set_log(core_log, NULL);
    int port = index_to_port(index);

    EnterCriticalSection(&g_lock);
    int handle = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (!g_slots[i]) {
            handle = i + 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (!handle) {
        shim_log("connect index=%d: no free slot", index);
        return 0;
    }

    modavd_open_params params;
    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);
    params.console_port = port;
    params.client_name = "external_renderer_ipc";
    modavd_handle h = NULL;
    int rc = modavd_open(&params, &h);
    if (rc != MODAVD_OK) {
        shim_log("connect index=%d port=%d: shared memory unavailable (%s)", index, port, modavd_strerror(rc));
        return 0;
    }
    const mx_video_info* vi = (const mx_video_info*)h->shm.view;
    shim_log("connect index=%d port=%d handle=%d %ux%u fps=%u frame=%u size=%zu", index, port, handle, vi->width,
             vi->height, vi->fps, vi->frame_number, h->shm.size);

    EnterCriticalSection(&g_lock);
    if (g_slots[handle - 1]) {
        /* Another thread took this slot meanwhile; find another one. */
        handle = 0;
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (!g_slots[i]) {
                handle = i + 1;
                break;
            }
        }
    }
    if (handle) {
        g_slots[handle - 1] = h;
        g_slot_ports[handle - 1] = port;
    }
    LeaveCriticalSection(&g_lock);
    if (!handle) {
        modavd_close(h);
        shim_log("connect index=%d: no free slot", index);
    }
    return handle;
}

EXPORT void nemu_disconnect(int handle)
{
    EnterCriticalSection(&g_lock);
    modavd_handle h = get_slot(handle);
    if (h) {
        shim_log("disconnect handle=%d port=%d", handle, g_slot_ports[handle - 1]);
        g_slots[handle - 1] = NULL;
        g_slot_ports[handle - 1] = 0;
    }
    LeaveCriticalSection(&g_lock);
    modavd_close(h);
}

EXPORT int nemu_get_display_id(int handle, const char* pkg, int app_index)
{
    (void)pkg;
    (void)app_index;
    return get_slot(handle) ? 0 : -1;
}

/* Returns 0 on success (MuMu convention), -1 for a bad handle, -2 when no
 * frame is available, -3 when the buffer is too small. */
EXPORT int nemu_capture_display(int handle, unsigned int display_id, int buffer_size, int* width, int* height,
                                unsigned char* pixels)
{
    (void)display_id;
    modavd_handle h = get_slot(handle);
    if (!h) {
        return -1;
    }
    modavd_frame_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    size_t size = (buffer_size > 0 && pixels) ? (size_t)buffer_size : 0;
    /* mx_copy_frame, not modavd_capture: when a new frame arrived during every
     * copy, hand the last copy over as the DLL this replaces did, instead of
     * failing (MaaCore would drop to its screencap speed test again). */
    int rc = mx_copy_frame(&h->shm, MODAVD_FMT_RGBA, MODAVD_CAPTURE_BOTTOM_UP, h->simd, size ? pixels : NULL, size,
                           &info);
    if (rc == MX_TORN) {
        rc = MODAVD_OK;
    }
    if (rc == MODAVD_OK || rc == MODAVD_E_BUFFER) {
        if (width) {
            *width = info.width;
        }
        if (height) {
            *height = info.height;
        }
    }
    switch (rc) {
    case MODAVD_OK: return 0;
    case MODAVD_E_BUFFER: return -3;
    default: return -2;
    }
}
