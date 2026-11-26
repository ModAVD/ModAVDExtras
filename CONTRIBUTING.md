# 参与 ModAVDExtras

**简体中文** | [English](CONTRIBUTING.en.md)

ModAVD 组织的通用规矩见 [ModAVD 的 CONTRIBUTING.md](https://github.com/ModAVD/ModAVD/blob/main/CONTRIBUTING.md) 和文档站的[参与开发](https://modavd.github.io/develop/)：先开 issue 商量，从自己的 fork 开 PR，署名用 GitHub 的 noreply 邮箱，不写 `Co-authored-by`，用 AI 工具要让它自己测过并在 PR 里写明。

下面是这个仓库自己的几条。

## 许可证的边界

- 本库是 LGPL-3.0-or-later，**只收为本项目写的代码**。
- 不要把 GPL、AGPL 项目的代码放进来，照抄、改写、翻译都不行。MaaCore（MAA）是 AGPL-3.0，它的代码（比如 `MumuExtras`、`MumuController`、`SwipeHelper`）只能留在交给 MAA 上游的适配里，不能进这个仓库。
- 需要参照的东西，用更干净的出处：AOSP 模拟器源码（Apache-2.0），安卓、Linux 的公开头文件，或者自己按行为写。PR 说明里写清楚参照了什么。
- 审 PR 时专门看这一条。

## 接口

- `include/modavd_extras.h` 是对外的承诺：函数和字段只加不删，加了就把 `MODAVD_API_VERSION` 加一。结构体开头的 `size` 字段不能动。
- 库不自己退回 adb，出错返回错误码。
- 兼容外壳 `shim/` 的导出必须和现在的一样：只有四个截图函数，不导出输入函数。`tests/test_shim.c` 会查这一条。

## 测试

- 改了代码，在 PR 里写明跑过什么：`build.ps1`（Windows）或 CMake 加 `ctest`（Linux、macOS）。
- 新功能和修的 bug 带单元测试。测试不需要模拟器，用 `tests/fake_frame.h` 的假共享内存。
- 提交前把被测的代码故意改坏一两处，确认测试会红，结果写进 PR。
- 在 ModAVD 实例上的实测（截图延迟、和旧外壳对比）写进 PR：测了几次、什么分辨率、当时的负载。

## workflow

第三方 action 钉到 40 位提交 SHA，不用 `pull_request_target`，不用自托管 runner。改 workflow 的 PR 由另一个人审。

`scripts/privacy.py` 与 ModAVD 主仓逐字节相同，CI 会核对。修改扫描器时先改 ModAVD，再原样同步过来；`privacy-allowlist.json` 仍由本仓库维护。
