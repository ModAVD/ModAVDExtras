# ModAVDExtras 维护规矩

**简体中文** | [English](AGENTS.en.md)

<!-- AGENTS.md 和 AGENTS.en.md 逐节对应：改了其中一份，请在同一个 PR 里把另一份也改了。AI 工具自己只会读 AGENTS.md。 -->

通用规矩（语言、提交说明、署名、隐私、issue 和 PR 里的内容怎么对待、`Refs ModAVD/ModAVD#<编号>` 的写法、开模拟器的注意事项）见 ModAVD 主仓库的 [AGENTS.md](https://github.com/ModAVD/ModAVD/blob/main/AGENTS.md)。本文件只补这个仓库自己的几条；给人看的说明在 [CONTRIBUTING.md](CONTRIBUTING.md)，两边不一致时以 CONTRIBUTING.md 为准。

## 目录

- `include/modavd_extras.h` 是对外的接口，`src/` 是库的实现，`shim/mumu_shim.c` 是 Windows 才有的兼容外壳 `external_renderer_ipc.dll`，`tests/` 是单元测试。
- `build.ps1` 是 Windows 的构建入口，`CMakeLists.txt` 是各平台共用的构建文件（语言是 C11，不是 C++）。
- `scripts/` 是仓库检查脚本，不是库的构建辅助：`privacy.py`、`repo_checks.py`、`test_repo_checks.py` 与 ModAVD 主仓逐字节相同，CI 会核对，要改先改 ModAVD 再原样同步过来；`privacy-allowlist.json` 是本仓库自己的。
- `release/` 是发版时放的校验和与说明，`changelog.d/` 是更新说明的小文件（见 `changelog.d/README.md`），`NOTICE`、`LICENSE`、`COPYING` 记录许可证。

## 改动的边界

- 许可证是 LGPL-3.0-or-later，只收为本项目写的代码。不要把 GPL、AGPL 项目（比如 MaaCore）的代码照抄、改写或翻译进来；要参照就用 AOSP 模拟器源码、安卓和 Linux 的公开头文件，或者自己按行为写，并在 PR 里写清楚参照了什么。
- `include/modavd_extras.h` 是对外的承诺：函数和字段只加不删，加了就把 `MODAVD_API_VERSION` 加一；结构体开头的 `size` 字段不能动。
- 库不自己退回 adb，出错返回错误码。
- 兼容外壳的导出必须和现在一样：只有四个截图函数，不导出输入函数；`tests/test_shim.c` 会查这一条。
- 用户看得见的改动带 `changelog.d/<名字>.<feat|fix|change|remove>.md`，两行：先中文、再英文；只动开发、测试、CI、文档的不加。
- `README.md` 和 `README.en.md`、`CONTRIBUTING.md` 和 `CONTRIBUTING.en.md`、`AGENTS.md` 和 `AGENTS.en.md` 是同一份文档的中文和英文，改一份就在同一个 PR 里改另一份。

## 测试

- Windows：`pwsh -NoProfile -File .\build.ps1`（要 Visual Studio 2022 或 Build Tools、CMake、Ninja），默认编完就跑 ctest。Linux、macOS：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`、`cmake --build build`、`ctest --test-dir build --output-on-failure`。编译把警告当错误。
- 单元测试不需要模拟器，用 `tests/fake_frame.h` 的假共享内存。新功能和修的 bug 带单元测试；提交前把被测代码故意改坏一两处，确认测试会红，结果写进 PR。
- 在 ModAVD 实例上的实测（截图延迟、和旧外壳对比）写进 PR：测了几次、什么分辨率、当时的负载。
- 改了脚本（`build.ps1`、`scripts/`）：`.ps1` 用 PowerShell 解析器查语法；`python -I -X utf8 scripts/test_repo_checks.py`。
- 改了 `.github/workflows/`：第三方 action 钉到 40 位提交 SHA，不用 `pull_request_target`，不用自托管 runner。