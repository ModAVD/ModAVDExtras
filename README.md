# ModAVDExtras：给自动化框架用的 ModAVD 截图库

- [这是什么](#这是什么)
- [状态](#状态)
- [接口](#接口)
- [兼容外壳 external_renderer_ipc.dll](#兼容外壳-external_renderer_ipcdll)
- [编译和测试](#编译和测试)
- [许可证](#许可证)
- [English](#english)

## 这是什么

一个 C 库：从 [ModAVD](https://github.com/ModAVD/ModAVD) 实例直接取画面、在宿主这边点击，不经 adb。给 MAA（MaaCore）、MaaFramework 这类自动化框架接入用。

- 画面来自模拟器自己的共享内存：`screenrecord webrtc start`（或启动参数 `-share-vid`）之后，模拟器把每一帧写进一块按控制台端口命名的共享内存。
  - Windows 上叫 `SHM_videmulator<端口>`，Linux、macOS 上叫 `videmulator<端口>`；
  - 内容是 24 字节的头（宽、高、fps、帧号、时间戳），后面跟 BGRA 像素。
- 库负责：
  - 按 adb 地址找到实例的控制台端口；
  - 共享内存没开时通过控制台把它开起来；
  - 拷出一帧，换成调用方要的格式；
  - 遇到新帧写到一半时重拷；
  - 告诉调用方现在能不能用。
- 点击走模拟器控制台的 `event send`，发标准的多点触控事件（槽位、跟踪编号、坐标、压力），到达模拟器自己的虚拟触屏：安卓看到的是屏幕上的手指，和在模拟器窗口里点一样，不是应用注入的事件，客体里也不用装东西。最多 10 根手指。
- 按键和文字也走控制台（接口版本 3）：
  - 按键（`modavd_key`，安卓 KEYCODE）按的是模拟器键盘上的键，返回、主页、多任务这些也在内。要模拟器的控制台有 `event key`（比 ModAVD 0.40 新的模拟器，`MODAVD_CAP_KEY`）；更旧的模拟器上只有能打出字符的键和编辑框里的编辑键可用。
  - 文字（`modavd_input_text`，UTF-8）：编辑框有焦点、ModAVD 的宿主输入法在用时，原样交给编辑框，任意语言；否则 ASCII 用键盘打。两样都不满足时返回 `MODAVD_E_UNSUPPORTED`，什么都不打。
- 库**不自己退回 adb**：出错时返回错误码，让调用方按自己的规则换别的办法。
- 只依赖系统库：Windows 上是 `KERNEL32`、`WS2_32`，C 运行库静态链接；POSIX 上是 `shm_open`、`mmap` 和套接字。

## 状态

- 截图（接口版本 1）、点击（接口版本 2，`MODAVD_CAP_TOUCH`）、按键和文字（接口版本 3，`MODAVD_CAP_KEY`、`MODAVD_CAP_TEXT`）。
- Windows：截图这套逻辑在 ModAVD 上已经用了一个多月，形式是下面的兼容外壳。
- macOS（Apple Silicon）：11-09 有贡献者用官方 Emulator 37.2.13、API 35 ARM64 实测过截图。
- Linux：能编，单元测试在 CI 上跑；还没有在模拟器上实测。
- [Release](https://github.com/ModAVD/ModAVDExtras/releases) 从 v0.1.2 起带三个平台的库：Windows x64 的 `modavd_extras.dll`、macOS arm64 的 `libmodavd_extras.dylib`、Linux x64 的 `libmodavd_extras.so`，外壳 `external_renderer_ipc.dll` 只有 Windows 版。

## 接口

完整说明在 [include/modavd_extras.h](include/modavd_extras.h)。用法大致是：

```c
#include "modavd_extras.h"

modavd_open_params p = { sizeof(p) };
p.adb_address = "127.0.0.1:20011";   /* 或 "emulator-20010"；adb 端口 = 控制台端口 + 1 */
p.client_name = "my-tool 1.0";
modavd_handle h;
if (modavd_open(&p, &h) == MODAVD_OK) {
    modavd_frame_info info = { sizeof(info) };
    modavd_capture(h, 0, MODAVD_FMT_BGR, 0, NULL, 0, &info);          /* 先问尺寸 */
    void* buf = malloc((size_t)info.stride * info.height);
    int rc = modavd_capture(h, 0, MODAVD_FMT_BGR, 0, buf, (size_t)info.stride * info.height, &info);
    /* rc: MODAVD_OK；MODAVD_E_NOT_READY 还没有画面（刚开流、第一帧没到时也是这样，稍后再调）；
     *     MODAVD_E_RESIZED 要重新 open */
    free(buf);
    modavd_close(h);
}
```

- 所有函数返回 `MODAVD_OK`（0）或负的 `MODAVD_E_*`。
- Linux、macOS 上 `modavd_open` 默认先经控制台开流（`screenrecord webrtc start`），所以控制台要连得上、鉴权要能过；模拟器退出后旧的共享内存可能还在，不开流会拿到上次的旧画面。确定映射属于当前实例而且已经在开流的，可以带 `MODAVD_OPEN_NO_START` 跳过控制台。
- 结构体开头是 `size`，以后加字段不破坏旧的调用方。
- `modavd_api_version()` 每加一个函数就加一，函数从不删。`modavd_capabilities()` 说明这个实例能做哪些事。
- `modavd_frame_info` 里有帧号和时间戳：调用方可以据此跳过重复帧，也能分清「画面没变」和「模拟器卡住」。

## 兼容外壳 external_renderer_ipc.dll

还不认识 ModAVD 的 MaaCore 版本，截图只会走 MuMu 那条路：到 MuMu 安装目录下载入 `shell\sdk\external_renderer_ipc.dll`，再按名字取 `nemu_connect`、`nemu_disconnect`、`nemu_get_display_id`、`nemu_capture_display` 四个函数。

- 本仓库编出的同名 DLL 就导出这四个函数，内部用的是同一套截图代码（静态编进去，不依赖 `modavd_extras.dll`）。
- 它不导出输入函数，所以 MaaCore 照旧用 MaaTouch 点击。
- AUTO-MAS 现在就是这样让 MAA 接 ModAVD 的：把这个 DLL 放进一个目录，在 MAA 里把它当作 MuMu 的安装路径，实例编号填控制台端口。这个外壳替换的是 AUTO-MAS 仓库里原来的 `res/avdshim`，行为不变。
- 实例编号也可以直接填 ModAVD 的编号（0–9）。外壳放在 `<ModAVD 根目录>\mumu-shim\shell\sdk\` 时，按根目录 `mas-avd.json` 的 `portBase` 算控制台端口（没写就是 20000），即 `portBase + 10×编号`。
- 等 MaaCore 有了 ModAVD 自己的连接方式，这个外壳就退场。

只能在 Windows 上用。

## 编译和测试

**Windows**（Visual Studio 2022 或 Build Tools，加上 CMake 和 Ninja）：

```powershell
pwsh -NoProfile -File .\build.ps1          # 产物和 SHA-256 在 out\ 下
```

`-CMake`、`-Ninja` 可以指定这两个程序的路径。MSVC 编译带 `/Brepro`，同一份源码、同一套工具编出的 DLL 逐字节相同。

**Linux、macOS**：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

单元测试不需要模拟器：用一块按同样格式建的假共享内存，测格式转换、出错语义、地址解析。Windows 上另测兼容外壳：照 MaaCore 的方式载入它、按名字取函数、截图。

在 ModAVD 实例上量截图延迟、和旧外壳对比，由维护者在本机做，结果写在 PR 里。

## 许可证

- 本库按 GNU Lesser General Public License 第 3 版或（由你选择）任何更新的版本授权（LGPL-3.0-or-later）。
  - [LICENSE](LICENSE) 是 LGPL-3.0 的正文；
  - [COPYING](COPYING) 是它所补充的 GPL-3.0 的正文。
- 载入这个库的程序不受 LGPL 约束；改了库本身，改过的库要按 LGPL 开源。
- 本库只收我们自己写的代码。来自 GPL、AGPL 项目（比如 MaaCore）的代码不进这个仓库，见 [CONTRIBUTING.md](CONTRIBUTING.md)。
- `scripts/` 下的仓库检查脚本和许可证另算，见 [NOTICE](NOTICE)。

## English

ModAVDExtras is a small C library that reads the screen of a [ModAVD](https://github.com/ModAVD/ModAVD) instance directly, without adb, for automation frameworks such as MaaCore (MAA) and MaaFramework, and touches it from the host.

- Frames come from the emulator's own shared memory: after `screenrecord webrtc start` (or `-share-vid`), the emulator writes every frame to a region named after its console port: `SHM_videmulator<port>` on Windows, `videmulator<port>` elsewhere. The layout is a 24-byte header (width, height, fps, frame number, timestamp) followed by BGRA pixels.
- The library finds the console port from an adb address, starts frame sharing through the console when it is off, copies a frame in the format the caller asks for (copying again if a new frame arrived meanwhile), and reports whether capture is usable. It never falls back to adb by itself.
- Touch (API version 2, `MODAVD_CAP_TOUCH`) goes through the console's `event send` as standard multi-touch events (slot, tracking id, position, pressure) to the emulator's own virtual touch screen: Android sees a finger on the screen, as with a click on the emulator window, not an injected event, and nothing is installed in the guest. Up to 10 contacts.
- Keys and text (API version 3) go through the console too. `modavd_key` presses keys of the emulator's keyboard by Android key code, BACK, HOME and APP_SWITCH included; it needs the console's `event key` (ModAVD emulators newer than 0.40, `MODAVD_CAP_KEY`), and on older emulators only keys that type a character and the editing keys of a focused text field work. `modavd_input_text` hands UTF-8 text in any language to the focused text field through ModAVD's host input method, or types ASCII on the keyboard when there is none. See [include/modavd_extras.h](include/modavd_extras.h).
- `external_renderer_ipc.dll` is a Windows-only compatibility shell. It exports the four capture functions that MaaCore looks up in MuMu's DLL, on top of the same code, so MaaCore versions that do not know ModAVD can still capture from it. AUTO-MAS uses it today. It will be retired once MaaCore can connect to ModAVD directly.
- Status: Windows has used this capture code for over a month through the shell below; on Apple Silicon macOS a contributor tested capture with the official Emulator 37.2.13 and API 35 ARM64; Linux builds and passes the unit tests in CI but has not been tested with an emulator. From v0.1.2 the [releases](https://github.com/ModAVD/ModAVDExtras/releases) carry `modavd_extras.dll` (Windows x64), `libmodavd_extras.dylib` (macOS arm64) and `libmodavd_extras.so` (Linux x64), plus the Windows-only shell.
- Build: `build.ps1` on Windows (Visual Studio 2022 Build Tools, CMake, Ninja); CMake on Linux and macOS. The unit tests need no emulator.
- License: LGPL-3.0-or-later ([LICENSE](LICENSE), [COPYING](COPYING)). Only code written for this project goes in; code from GPL or AGPL projects such as MaaCore does not.
