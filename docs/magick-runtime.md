# ImageMagick 运行时构建与打包

项目运行时直接链接 `MagickWand`。仓库不提交 ImageMagick 二进制，统一由脚本生成本地 runtime/development bundle。

## 构建命令

```powershell
.\scripts\build-magick.ps1 -Configuration Release -Arch x64
```

脚本默认使用 `-Linkage Static`，并把 ImageMagick Windows 源码固定到默认 ref `6ad8928f61d4abf3fe17646d7083bb6866eae92e`。构建流程会先构建 ImageMagick 官方 `Configure.exe`，生成 `IM7.Static.x64.sln`，再优先只编译 MagickCore/MagickWand 与 AVIF(WebP/HEIC)、WebP coder。需要完整输入格式支持时加 `-FullBuild`；如果静态 delegate 链接不顺，可改用：

```powershell
.\scripts\build-magick.ps1 -Configuration Release -Arch x64 -Linkage Dynamic -FullBuild
```

源码默认放在仓库内的 `third_party\imagemagick-src`。脚本会给当前进程内的 git 子进程启用 `core.longpaths=true`，以处理 ImageMagick 依赖仓库中部分测试文件的超长文件名；同时会把 GitHub 相关 URL 固定为 `https://github.com/`，避免本机全局 SSH URL rewrite 干扰自动构建。运行时产物提取到仓库的 `third_party\imagemagick-runtime`，源码和产物都不会提交进 Git。脚本会在生成的 runtime 目录写入 `.avif-webp-studio-generated` 标记；后续重新提取时只会递归删除带该标记且位于仓库 `third_party\imagemagick-runtime` 下的目录，避免误传路径时删除非预期文件。需要验证新的 ImageMagick 版本时，先传 `-ImageMagickRef <commit-or-tag>`；通过完整构建与烟测后，再更新脚本默认 ref 和发布记录。

ImageMagick 官方 `Configure.sln` 需要 Visual Studio MFC。脚本会在拉取源码和编译前检查 `Microsoft.VisualStudio.Component.VC.ATLMFC`，缺少时直接给出修复命令；确认允许修改本机 Visual Studio 安装时，可以显式传：

```powershell
.\scripts\build-magick.ps1 -Configuration Release -Arch x64 -InstallMfc
```

默认输出：

```text
third_party\imagemagick-runtime\x64\Release
```

该目录按实际链接方式包含：

- `include\MagickWand` / `include\MagickCore`
- Release: `lib\CORE_RL_MagickWand_.lib` / `lib\CORE_RL_MagickCore_.lib`
- Debug: `lib\CORE_DB_MagickWand_.lib` / `lib\CORE_DB_MagickCore_.lib`
- 静态构建时同一 flavor 的 `CORE_RL_*.lib` 或 `CORE_DB_*.lib` coder/delegate libs
- 动态构建时的 `CORE_RL_*.dll`、`modules\coders\IM_MOD_RL_heic_.dll`、`IM_MOD_RL_webp_.dll`
- `configure.xml`、`delegates.xml`、`policy.xml` 等配置
- `License` / `NOTICE`

## CMake 使用

默认构建脚本会优先寻找：

```text
third_party\imagemagick-runtime\x64\Release
```

也可以显式指定：

```powershell
.\release.ps1 -MagickRoot ".\third_party\imagemagick-runtime\x64\Release"
```

静态 ImageMagick runtime 不带 DLL，`release.ps1` / `debug.ps1` 会自动切换到 MSVC `/MT` / `/MTd`，并使用 `x64-windows-static` vcpkg triplet。仓库通过根目录的 `vcpkg.json` 和 `vcpkg-configuration.json` 固定 `scnlib` 依赖与 baseline；若本机缺少 `scnlib:x64-windows-static`，脚本仍可调用 vcpkg 自动安装；不希望自动安装时可传：

```powershell
.\release.ps1 -NoVcpkgInstall
```

如果要手动指定 triplet：

```powershell
.\release.ps1 -VcpkgTriplet x64-windows-static
```

也可以使用标准 CMake presets：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release
```

脚本会在每次配置时清理 `scn_DIR` / `FastFloat_DIR` 这类 CMake 包路径缓存，避免之前用 `x64-windows` 配置过的构建目录继续链接动态 CRT 版本的 `scn.lib`。
同时也会清理 `MAGICKWAND_LIBRARY` / `MAGICKCORE_LIBRARY`，避免构建目录曾经使用过 Scoop ImageMagick 时继续链接 DLL import library，导致在无 ImageMagick 的机器上启动时报缺少 `CORE_RL_MagickWand_.dll`。
当 `MAGICK_ROOT` 中没有 DLL 时，CMake 会把 AVIF/WebP 相关的静态 delegate libs 追加到链接末尾，避免链接器先看到 `aom` / `brotli` 等叶子库后又从 `heif` 等库产生新引用，最终留下 `aom.dll`、`libde265.dll`、`brotli*.dll` 等运行时依赖。

如果用 VS Code CMake Tools 直接配置 `build` 目录，CMake 也会检查 `MAGICK_ROOT` 是否为静态 runtime。检测到静态 ImageMagick 时会自动把旧缓存从 `/MD + x64-windows` 修正为 `/MT + x64-windows-static`，避免 MagickWand 头文件生成 `__declspec(dllimport)` 后在链接阶段出现 `__imp_Magick...` 未解析符号。使用 Ninja 生成器时仍需要选择 Visual Studio x64 Kit，或先进入 Visual Studio Developer PowerShell，否则 MSVC 标准库路径不会进入环境，可能连 `<algorithm>` / `stdio.h` 都找不到。

构建完成后，CMake 会把存在的运行时 DLL、modules 和配置文件复制到 `AVIF-WebP-Cli.exe` / `AVIF-WebP-Studio.exe` 所在目录。静态 ImageMagick 构建没有对应 DLL 时，不会额外复制。程序启动时只会自动检查程序输出目录及其祖先目录中的 `third_party\imagemagick-runtime\x64\Release`，不会从当前工作目录或其祖先隐式加载 runtime；显式 `--magick` 和 `AVIF_MAGICK` 仍可指向外部 runtime。选中的路径会设置到：

- `MAGICK_HOME`
- `MAGICK_CONFIGURE_PATH`
- `MAGICK_CODER_MODULE_PATH`
- `MAGICK_FILTER_MODULE_PATH`

## 本地 fallback

`debug.ps1` / `release.ps1` 默认不会再悄悄使用 Scoop。没有对应配置的自编译 runtime 时，它们会自动运行 `scripts\build-magick.ps1`。程序运行时也不会自动探测 Scoop ImageMagick，也不会从当前工作目录隐式加载外部 runtime；只有 `--magick` 或 `AVIF_MAGICK` 会显式使用外部 runtime。如果只是本机临时调试，并且本机存在：

```text
D:\Scoop\apps\imagemagick\current
```

可以显式传入：

```powershell
.\release.ps1 -UseScoopFallback
.\debug.ps1 -UseScoopFallback
```

Scoop fallback 只用于快速开发；发布前应以 `release.ps1` 自动生成的自编译 runtime 为准。

## 验证

构建后可以运行：

```powershell
.\bin\x64\Release\AVIF-WebP-Cli.exe -i input -o Avifoutput -q q90
```

动态构建时，如果想确认 AVIF/WebP coder 是否随 runtime 一起复制，可以检查：

```powershell
Get-ChildItem .\bin\x64\Release\modules\coders\IM_MOD_RL_*heic*.dll
Get-ChildItem .\bin\x64\Release\modules\coders\IM_MOD_RL_*webp*.dll
```

## 单文件分发

CMake 默认静态链接 Slint，因此 UI 不再需要 `slint_cpp.dll`。真正单 exe 还要求 ImageMagick、AVIF/WebP delegate、scnlib 和 CRT 都能静态链接；推荐顺序是：

```powershell
.\scripts\build-magick.ps1 -Configuration Release -Arch x64 -Linkage Static
.\release.ps1 -MagickRoot ".\third_party\imagemagick-runtime\x64\Release"
```

脚本会自动选择 `/MT` 和 `x64-windows-static`。如果静态 delegate 或 CRT 与本机 vcpkg triplet 不兼容，保留少量 DLL 是更稳妥的发布方式，可以改用 `-Linkage Dynamic` 或显式传 `-DynamicRuntime`。静态 exe 会优先使用同目录的 ImageMagick XML/ICC 配置；只复制单个 exe 时会尝试使用 ImageMagick 内置默认值，但推荐一起分发 release 输出目录中的配置文件以减少格式识别差异。
