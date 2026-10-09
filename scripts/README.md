# 本地构建

构建工具只保存在项目 `.tools` 中，不安装到系统、不改 PATH；工具链不会进入发布文件。
目标为 Windows 10/11 x64，采用系统 UCRT 与 Windows API，C++ 标准库和项目依赖静态链接。

首次准备（需要网络，官方 ZIP 约 191 MB，解压后占用更多磁盘空间）：

```powershell
pwsh -File scripts/setup-toolchain.ps1
```

构建便携版：

```powershell
pwsh -File scripts/build.ps1
```

输出 `dist/TokenScope.exe` 和供开发验证的 `dist/build-report.json`。
Release 采用 `-Oz`、LTO、section 回收、静态 C++ 标准库与 strip；如果最终 EXE 大于
10,000,000 字节，或导入已知需要外部分发的 C++ 运行库 DLL，构建会失败。
`build-report.json` 记录真实字节数、SHA-256 和导入 DLL。

自动测试：

```powershell
pwsh -File scripts/test.ps1
```

`-Configuration Debug` 可生成带符号的诊断版本；体积发布门槛仅适用于 Release。
`-ToolchainRoot <目录>` 可指定相同目标的已有 LLVM-MinGW 工具链。
`build.ps1 -SkipApp -Tests` 编译核心、国产来源、CLI、规范导出和持久集成测试；`-ViewTests` 增加日期/筛选/绘制测试。

SQLite、yyjson 与 Zstandard 只解压库的固定版源码、许可证及文件摘要位于 `vendor/`，构建不会联网下载依赖。
必要的第三方声明作为资源编入 EXE。历史数据库、用户设置和图标缓存不属于构建产物。
