# AWJ 1.0.4 / 1.0.5 发版

所有构建、暂存、归档与旧版测试样本必须位于仓库 `build/` 或 `bin/`。不在 `D:/` 根目录创建任何文件。Windows 的 1.0.3 测试样本只保留在 `bin/old-versions/1.0.3/`；除非另行指定，不保留后续旧版。

## 1.0.4 prerelease

1. 在 `codex/awj-1.0.4` 上完成 Windows Release CTest、Studio/CLI smoke，以及普通 WSL Linux Release 构建、CTest、CLI、ELF 和归档检查。不要在 WSL 或虚拟机执行自动更新端到端测试。
2. 直接合并到 `master`，从干净的 1.0.4 提交打 `1.0.4` tag；Windows 和 Linux 二进制必须来自该 tag 的锁定依赖。
3. 用仓库外 Ed25519 seed 编译签名工具和客户端公钥，然后生成、测试归档：

   ```powershell
   .\scripts\package-release.ps1 `
     -WindowsExePath .\bin\x64\Release\AWJ.exe `
     -WindowsComPath .\bin\x64\Release\AWJ.com `
     -LinuxBinaryPath .\bin\x64\Release\AWJ `
     -SkipManifests
   ```

   该步骤仅写入 `build/release/1.0.4/`，执行 `7z t`、全新解压、逐文件 SHA-256 和 Windows CLI `--help`。归档包含二进制、校验文件、`LICENSE`、`THIRD_PARTY_NOTICES.txt`、`THIRD_PARTY_LICENSES/` 和 `BUILD_INFO.txt`。Linux `AWJ --help` 与 `readelf -d` 在 WSL 普通验证中执行。
4. 创建 prerelease 并且只上传 `build/release/1.0.4/assets/AWJ_Win.7z` 与 `AWJ_Linux.7z`。先核对公开下载、大小、哈希和 Release prerelease 状态。
5. 资产可下载后，签名并提交 v2 manifest：

   ```powershell
   .\scripts\package-release.ps1 `
     -WindowsExePath .\bin\x64\Release\AWJ.exe `
     -WindowsComPath .\bin\x64\Release\AWJ.com `
     -LinuxBinaryPath .\bin\x64\Release\AWJ `
     -ArchiveManifestSequence 1 `
     -PublishedAtUtc 'YYYY-MM-DDTHH:MM:SSZ' `
     -UpdatePublicKeyHex $env:AWJ_UPDATE_PUBLIC_KEY_HEX `
     -UpdateSigningSeedFile $env:AWJ_UPDATE_SIGNING_SEED_FILE
   git add update-manifest-v2.json update-manifest-v2.json.sig
   git commit -m 'release: add 1.0.4 archive update manifest'
   git push origin master
   ```

   `update-manifest-v2.json` 单独维护 sequence，只含归档资产。每个成员都绑定归档 URL、大小和 SHA-256；客户端不回退到裸资产。

## 1.0.5 bridge prerelease

从 1.0.4 的锁定源码创建 1.0.5，只改版本、更新日志和发布说明。完成同一组构建和归档验证后，发布四个自定义资产：`AWJ_Win.7z`、`AWJ_Linux.7z`、`AWJ.exe`、`AWJ.com`。Release 正文必须明确：功能与 1.0.4 相同，仅用于 1.0.3 自动更新桥接测试；普通用户应下载 1.0.4。

资产公开可下载后，仅给旧 v1 manifest 写入 1.0.5：

```powershell
.\scripts\package-release.ps1 `
  -WindowsExePath .\bin\x64\Release\AWJ.exe `
  -WindowsComPath .\bin\x64\Release\AWJ.com `
  -LinuxBinaryPath .\bin\x64\Release\AWJ `
  -BridgeRelease `
  -LegacyManifestSequence 8 `
  -PublishedAtUtc 'YYYY-MM-DDTHH:MM:SSZ' `
  -UpdatePublicKeyHex $env:AWJ_UPDATE_PUBLIC_KEY_HEX `
  -UpdateSigningSeedFile $env:AWJ_UPDATE_SIGNING_SEED_FILE
```

它不会把 1.0.5 添加为 v2 候选。Windows 本机从 `bin/old-versions/1.0.3/` 的隔离副本执行 1.0.3→1.0.5 下载、校验、安装、健康检查和回滚测试；通过后停止。未经新的明确授权，不创建或发布 1.0.6。
