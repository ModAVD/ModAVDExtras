# ModAVDExtras 维护规矩

通用参与、分支、提交、隐私和发版规矩见 ModAVD 主仓库的 [AGENTS.md](https://github.com/ModAVD/ModAVD/blob/main/AGENTS.md)。本文件只补充截图库仓库自己的约定。

## 目录

- `src/` 和 `include/` 是截图库，`shim/` 是宿主外壳，`tests/` 是测试，`scripts/` 保存构建辅助脚本。
- `build.ps1` 和 `CMakeLists.txt` 是构建入口，`NOTICE` 和 `COPYING` 记录许可证。

## 测试

C++ 改动运行仓库现有构建和测试。脚本改动至少做 PowerShell 解析检查。构建产物和本机路径不进提交。

## 做事

日志、截图和性能数据只写到调用方指定的临时目录。不要在库里加入联网下载、自动更新或读取私有密钥的逻辑。
