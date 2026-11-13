/*
 * One console command on its own connection (used to start frame sharing).
 * Kept apart from console.c so that a test can replace it while the touch
 * session functions still come from the library.
 *
 * Copyright 2026 qiyinxi
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "mx_internal.h"

int mx_console_command(int port, const char* command, int timeout_ms)
{
    mx_console c;
    int rc = mx_console_open(port, timeout_ms, &c);
    if (rc != MODAVD_OK) {
        return rc;
    }
    rc = mx_console_send(&c, command);
    mx_log(MODAVD_LOG_INFO, "console %d %s -> %s", port, command, rc == MODAVD_OK ? "OK" : "fail");
    mx_console_close(&c);
    return rc == MODAVD_OK ? MODAVD_OK : MODAVD_E_IO;
}
