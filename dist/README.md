# TokenScope Windows 发行版

当前版本：**v0.5.0**，适用于 Windows 10 / 11 x64。

下载并运行 `TokenScope.exe`。程序大小为 **2,392,064 字节（约 2.39 MB）**，无需安装 Node、Python、.NET 或附带浏览器。此目录只包含正式 Release 产物与许可、校验文件，不包含测试数据库、账号凭据、开发工具链或旧构建。

请同时保留 `LICENSE` 与 `THIRD-PARTY-NOTICES.txt`。项目代码采用 MIT 协议，第三方代码和品牌资源保留各自许可与商标权。当前程序未进行代码签名。

程序首次启动会创建自己的本机数据目录：`%LOCALAPPDATA%\TokenScope\`。发布文件不包含任何人的用量历史或登录信息。若旧版已经运行，请通过其托盘菜单退出后，再运行此版本。

## 校验下载

在 PowerShell 中执行：

```powershell
Get-FileHash .\TokenScope.exe -Algorithm SHA256
```

预期 SHA-256：

```text
c3e1b074c6cf2a040b1a2c16b3695c10e2728d0d969b771e3771ca3c2a5f9c56
```

`SHA256SUMS.txt` 包含本目录发行文件摘要，构建信息见 `build-report.json`。完整功能、来源覆盖与限制请阅读仓库根目录的 [README](../README.md)，本版改动见 [CHANGELOG](../CHANGELOG.md)。界面可在设置中切换为 English。
