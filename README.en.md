# ModAVDExtras

ModAVDExtras is a small C library that captures frames from a [ModAVD](https://github.com/ModAVD/ModAVD) instance without adb, for automation frameworks such as MaaCore and MaaFramework.

Frames come from the emulator's shared memory after `screenrecord webrtc start` or `-share-vid`. Windows uses `SHM_videmulator<console-port>`, while Linux and macOS use `videmulator<console-port>`. The library resolves the console port from an adb address, starts sharing through the console when needed, copies a frame in the requested format and reports capture errors. It never falls back to adb by itself.

Version 0.1 provides screen capture. The declared touch, key and text functions return `MODAVD_E_UNSUPPORTED`. The Windows `external_renderer_ipc.dll` compatibility shell exports the capture functions expected by older MaaCore builds.

## Build and test

On Windows, use Visual Studio 2022 Build Tools, CMake, Ninja and PowerShell 7:

```powershell
pwsh -NoProfile -File .\build.ps1
```

On Linux or macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The unit tests use fake shared memory and do not require an emulator. See [include/modavd_extras.h](include/modavd_extras.h) for the C API.

## License

The library is licensed under LGPL-3.0-or-later. See [LICENSE](LICENSE), [COPYING](COPYING) and [NOTICE](NOTICE).
