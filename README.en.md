# ModAVDExtras: a screen capture library for automation frameworks on ModAVD

[简体中文](README.md) | **English**

<!-- README.md and README.en.md correspond section by section: if you change one, change the other in the same pull request. -->

- [What this is](#what-this-is)
- [Status](#status)
- [API](#api)
- [Compatibility shell external_renderer_ipc.dll](#compatibility-shell-external_renderer_ipcdll)
- [Build and test](#build-and-test)
- [License](#license)

## What this is

A C library that takes frames straight from a [ModAVD](https://github.com/ModAVD/ModAVD) instance and clicks on the host side, without adb. It is meant for automation frameworks such as MAA (MaaCore) and MaaFramework.

- Frames come from the emulator's own shared memory: after `screenrecord webrtc start` (or the launch option `-share-vid`), the emulator writes every frame into a shared memory region named after its console port.
  - The region is `SHM_videmulator<port>` on Windows and `videmulator<port>` on Linux and macOS.
  - It holds a 24-byte header (width, height, fps, frame number, timestamp) followed by BGRA pixels.
- The library:
  - finds the instance's console port from an adb address;
  - turns shared memory on through the console when it is off;
  - copies out a frame and converts it to the format the caller asks for;
  - copies again if a new frame was being written during the copy;
  - tells the caller whether capture is usable right now.
- Clicks go through the emulator console's `event send` as standard multi-touch events (slot, tracking id, position, pressure) to the emulator's own virtual touch screen. Android sees fingers on the screen, as with a click in the emulator window; these are not events injected by an app, and nothing has to be installed in the guest. Up to 10 contacts.
- Keys and text go through the console too (API version 3):
  - A key (`modavd_key`, Android KEYCODE) presses a key of the emulator's keyboard, BACK, HOME and APP_SWITCH included. This needs the console's `event key` (emulators newer than ModAVD 0.40, `MODAVD_CAP_KEY`); on older emulators only the keys that type a character and the editing keys of a focused text field work.
  - Text (`modavd_input_text`, UTF-8): when a text field has focus and ModAVD's host input method is active, the text is handed to the field as is, in any language; otherwise ASCII is typed on the keyboard. When neither is possible it returns `MODAVD_E_UNSUPPORTED` and types nothing.
- The library **never falls back to adb by itself**: on failure it returns an error code and the caller switches to another method by its own rules.
- It depends only on system libraries: `KERNEL32` and `WS2_32` on Windows, with the C runtime linked statically; `shm_open`, `mmap` and sockets on POSIX.

## Status

- Capture (API version 1), touch (API version 2, `MODAVD_CAP_TOUCH`), keys and text (API version 3, `MODAVD_CAP_KEY`, `MODAVD_CAP_TEXT`).
- Windows: this capture logic has been in use with ModAVD for over a month, in the form of the compatibility shell below.
- macOS (Apple Silicon): on 9 November a contributor tested capture with the official Emulator 37.2.13 and API 35 ARM64.
- Linux: it builds and the unit tests run in CI; it has not been tested on an emulator yet.
- From v0.1.2 the [releases](https://github.com/ModAVD/ModAVDExtras/releases) carry the library for three platforms: `modavd_extras.dll` (Windows x64), `libmodavd_extras.dylib` (macOS arm64) and `libmodavd_extras.so` (Linux x64). The shell `external_renderer_ipc.dll` exists for Windows only.

## API

The full description is in [include/modavd_extras.h](include/modavd_extras.h). Roughly:

```c
#include "modavd_extras.h"

modavd_open_params p = { sizeof(p) };
p.adb_address = "127.0.0.1:20011";   /* or "emulator-20010"; adb port = console port + 1 */
p.client_name = "my-tool 1.0";
modavd_handle h;
if (modavd_open(&p, &h) == MODAVD_OK) {
    modavd_frame_info info = { sizeof(info) };
    modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info);          /* ask for the size first */
    void* buf = malloc((size_t)info.stride * info.height);
    int rc = modavd_capture(h, 0, MODAVD_FMT_BGR, 0, buf, (size_t)info.stride * info.height, &info);
    /* rc: MODAVD_OK; MODAVD_E_NOT_READY means no frame yet (also right after the stream starts, before the
     *     first frame arrives; call again later); MODAVD_E_RESIZED means open again */
    free(buf);
    modavd_close(h);
}
```

- Every function returns `MODAVD_OK` (0) or a negative `MODAVD_E_*`.
- On Linux and macOS, `modavd_open` starts the stream through the console by default (`screenrecord webrtc start`), so the console must be reachable and authentication must pass. After the emulator exits, an old shared memory region may remain, and without starting the stream you would get the previous frame. If you know the mapping belongs to the current instance and the stream is already on, pass `MODAVD_OPEN_NO_START` to skip the console.
- Structs begin with `size`, so fields can be added later without breaking old callers.
- `modavd_api_version()` goes up by one with every function added, and functions are never removed. `modavd_capabilities()` tells what this instance can do.
- `modavd_frame_info` carries the frame number and timestamp: callers can skip duplicate frames and tell "the picture did not change" from "the emulator is stuck".

## Compatibility shell external_renderer_ipc.dll

MaaCore versions that do not know ModAVD can only capture through the MuMu path: they load `shell\sdk\external_renderer_ipc.dll` from the MuMu install directory and look up four functions by name: `nemu_connect`, `nemu_disconnect`, `nemu_get_display_id` and `nemu_capture_display`.

- The same-named DLL built from this repository exports those four functions and uses the same capture code inside (linked statically, no dependency on `modavd_extras.dll`).
- It exports no input functions, so MaaCore still clicks with MaaTouch.
- This is how AUTO-MAS lets MAA use ModAVD today: put this DLL in a directory, give that directory to MAA as the MuMu install path, and enter the console port as the instance number. The shell replaces `res/avdshim` in the AUTO-MAS repository, with the same behavior.
- The instance number can also be ModAVD's own number (0-9). When the shell sits in `<ModAVD root>\mumu-shim\shell\sdk\`, the console port is worked out from `portBase` in the root's `mas-avd.json` (20000 if absent) as `portBase + 10 x number`.
- Once MaaCore has its own way to connect to ModAVD, this shell goes away.

It works on Windows only.

## Build and test

**Windows** (Visual Studio 2022 or Build Tools, plus CMake and Ninja):

```powershell
pwsh -NoProfile -File .\build.ps1          # the output and SHA-256 are under out\
```

`-CMake` and `-Ninja` set the paths of those two programs. MSVC builds with `/Brepro`, so the same source and the same tools give byte-identical DLLs.

**Linux and macOS**:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The unit tests need no emulator: they use a fake shared memory built in the same format, and test format conversion, error semantics and address parsing. On Windows the compatibility shell is tested too: it is loaded the way MaaCore loads it, the functions are looked up by name, and a frame is captured.

Measuring capture latency on a ModAVD instance and comparing it with the old shell is done by the maintainer on a local machine, and the result is written in the pull request.

## License

- The library is licensed under the GNU Lesser General Public License, version 3 or (at your option) any later version (LGPL-3.0-or-later).
  - [LICENSE](LICENSE) is the text of LGPL-3.0;
  - [COPYING](COPYING) is the text of the GPL-3.0 that it supplements.
- Programs that load the library are not bound by the LGPL; if you change the library itself, the changed library must be open-sourced under the LGPL.
- The library only takes code we wrote ourselves. Code from GPL or AGPL projects (such as MaaCore) does not go into this repository; see [CONTRIBUTING.en.md](CONTRIBUTING.en.md).
- The repository check scripts under `scripts/` have their own license; see [NOTICE](NOTICE).