# AVIF-WebP-Studio C++23

这是原 C# AVIF/WebP 压缩控制台的 C++23 迁移版，现支持 AVIF、WebP、JXL 输出。项目现在以 CMake 为主构建系统，核心转换逻辑同时服务于两个入口：

- `AVIF-WebP-Studio.exe`: Slint 桌面 UI
- `AVIF-WebP-Cli.exe`: 保留原命令行批处理能力

默认后端已经从调用 `magick.exe` 进程改为直接链接 ImageMagick `MagickWand` API。仓库不再提交 ImageMagick 二进制；需要可分发运行时时，使用 `scripts\build-magick.ps1` 拉取并构建 ImageMagick Windows 源码。程序运行时不会自动查找 Scoop ImageMagick，也不会从当前工作目录隐式加载外部 runtime；只有 `--magick` / `AVIF_MAGICK` 会显式使用外部 runtime。

## 环境

- Windows + Visual Studio 2026 C++ 桌面开发组件
- Visual Studio MFC 组件 `Microsoft.VisualStudio.Component.VC.ATLMFC`，用于构建 ImageMagick 官方 `Configure.sln`
- CMake 3.30+
- Rust/Cargo，用于 Slint C++ 后端构建
- Git，用于拉取 Slint 和 ImageMagick 源码
- vcpkg 已安装并设置 `VCPKG_ROOT`；仓库通过 `vcpkg.json` 和 `vcpkg-configuration.json` 固定 `scnlib` 依赖与 baseline

默认 vcpkg 路径按 `VCPKG_ROOT` 或 `D:\Scoop\apps\vcpkg\current` 查找；路径不同可传 `-VcpkgRoot`。如需手动指定 triplet，可传 `-VcpkgTriplet`；如不希望脚本自动安装缺失的 `scnlib` triplet，可传 `-NoVcpkgInstall`。标准 CMake 入口可使用 `CMakePresets.json` 中的 `windows-msvc-x64-debug` / `windows-msvc-x64-release`。

## 自编译 ImageMagick

推荐先构建 ImageMagick 运行时：

```powershell
.\scripts\build-magick.ps1 -Configuration Release -Arch x64
```

脚本会拉取 `https://github.com/ImageMagick/Windows` 并固定到默认 ref `6ad8928f61d4abf3fe17646d7083bb6866eae92e`，先构建官方 `Configure.exe`，再生成并编译 IM7 方案。源码默认放在仓库内的 `third_party\imagemagick-src`，运行时产物提取到 `third_party\imagemagick-runtime`。脚本会为当前进程的 git 子进程启用 `core.longpaths=true`，并强制 GitHub 依赖拉取走 HTTPS，避免 ImageMagick 依赖仓库的超长文件名和本机 SSH URL rewrite 干扰构建。默认 `-Linkage Static`，只构建 MagickCore/MagickWand 与 AVIF/HEIC、WebP、JXL coder，尽量减少分发 DLL；JXL 依赖 ImageMagick 的 JPEG XL delegate。如果需要完整格式支持可加 `-FullBuild`，如果静态 delegate 链接不顺可改用 `-Linkage Dynamic`。需要升级 ImageMagick 时先传 `-ImageMagickRef <commit-or-tag>` 验证，再更新默认 ref 和发布记录。

如果本机缺少 MFC，脚本会在拉取和编译前给出明确错误。确认允许 Visual Studio Installer 修改本机安装时，可以显式加 `-InstallMfc` 自动安装该组件。

构建产物会把 headers、libs、可能存在的 DLL/modules、配置 XML 和许可文件提取到：

```text
third_party\imagemagick-runtime\x64\Release
```

该目录被 `.gitignore` 忽略，不提交进仓库。`debug.ps1` / `release.ps1` 默认会自动构建自编译 runtime；只有显式传 `-UseScoopFallback` 时才会临时使用 Scoop ImageMagick。

## 编译

Debug:

```powershell
.\debug.ps1
```

`debug.ps1` 会优先使用 `third_party\imagemagick-runtime\x64\Debug`。如果该目录不存在，会自动调用 `scripts\build-magick.ps1 -Configuration Debug -Arch x64 -Linkage Static` 构建自编译 ImageMagick。只是本机快速调试、允许临时使用 Scoop 时才传 `-UseScoopFallback`。

Release:

```powershell
.\release.ps1
```

也可以直接使用 CMake presets：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release
```

`release.ps1` 会优先使用 `third_party\imagemagick-runtime\x64\Release`。如果该目录不存在，会自动调用 `scripts\build-magick.ps1 -Configuration Release -Arch x64 -Linkage Static` 构建自编译 ImageMagick。需要完整 ImageMagick 输入格式支持时可加 `-FullMagickBuild`；只是本机快速调试、允许临时使用 Scoop 时才传 `-UseScoopFallback`。

默认静态链接 Slint，因此 `AVIF-WebP-Studio.exe` 不再需要单独的 `slint_cpp.dll`。自编译静态 ImageMagick 会自动切到 `/MT` 和 `x64-windows-static`，以避免 CRT 混链；如需调试 Slint 共享库，可传 `-SharedSlint`，如确实要强制动态 CRT 可传 `-DynamicRuntime`。

显式指定运行时：

```powershell
.\release.ps1 -MagickRoot ".\third_party\imagemagick-runtime\x64\Release"
```

输出位置：

- `bin\x64\Debug\AVIF-WebP-Cli.exe`
- `bin\x64\Debug\AVIF-WebP-Studio.exe`
- `bin\x64\Release\AVIF-WebP-Cli.exe`
- `bin\x64\Release\AVIF-WebP-Studio.exe`

## 测试

项目已接入 CTest。当前最小测试门槛覆盖 CLI 帮助输出和未知参数错误路径，不依赖图片样本或 ImageMagick runtime：

```powershell
ctest --test-dir build\x64\Debug -C Debug --output-on-failure
```

## 使用

启动 UI：

```powershell
.\bin\x64\Release\AVIF-WebP-Studio.exe
```

CLI 示例：

```powershell
.\bin\x64\Release\AVIF-WebP-Cli.exe -i input -o Avifoutput
.\bin\x64\Release\AVIF-WebP-Cli.exe -i "D:\图片" -o Avifoutput -q q90
.\bin\x64\Release\AVIF-WebP-Cli.exe -i pngs -o out --max-resolution 2560 --strip
.\bin\x64\Release\AVIF-WebP-Cli.exe -i input -o out --chroma 444 --bit-depth 10
.\bin\x64\Release\AVIF-WebP-Cli.exe -i photo.png --format webp --collision random
.\bin\x64\Release\AVIF-WebP-Cli.exe -i input.png -o output.jxl --format jxl -q 90
.\bin\x64\Release\AVIF-WebP-Cli.exe -i photo.png --optimize --target-xpsnr 42.0
```

默认质量是 AVIF `q90`、WebP `q95`、JXL `q95`。默认不设置速度参数，让 ImageMagick 使用自身默认值；显式传入 `--speed 0..10` 时，AVIF 会设置 `heic:speed`，JXL 会映射为 `jxl:effort`。
质量 `q` 可选 `1..100`，`q100` 在 ImageMagick 的 AVIF/WebP/JXL coder 中走对应 coder 的无损或最高质量路径。质量会同时写入 MagickWand 的 wand 级 `image_info->quality` 和当前图片的 `image->quality`，因此行为等价于 ImageMagick CLI 的 `-quality`。

`--optimize` 是独立的自动搜索模式，不等同于手动固定一组 `--define`。它会在 `--min-quality` 到 `--quality` 之间生成候选文件，用 MagickWand 解码候选并计算亮度 XPSNR 风格评分，再选出达到目标 XPSNR 的最小体积版本；内部会加 `0.05 dB` 安全边距，避免刚好踩线的候选图被选中。AVIF 候选在未显式设置 `--speed` / `heic:speed` 时会使用 `heic:speed=0`；WebP 候选在未显式设置 `webp:method` 时会使用 `webp:method=6`；JXL 复用同一质量搜索流程。用户传入的 `--define` 仍会作为约束应用到每个候选图。`--define` 会直接影响 ImageMagick coder 行为，可能影响性能、安全和兼容性；不要在其中放入不希望写入日志或 summary 的敏感信息。

如果输出文件重名，默认覆盖。也可以用 `--collision skip|time|random` 跳过或追加后缀；覆盖模式下批处理会按扫描顺序处理同名输出，最后写入的文件保留。如果同一目录里出现 `1.jpg`、`1.bmp` 这类同名不同扩展输入，默认模板会自动保留源扩展名，输出为 `1.jpg.avif`、`1.bmp.avif` 或对应 WebP/JXL，并在 UI/CLI 给出警告。`summary.csv` 和日志默认不生成，只有传 `--summary` / `--log` 或在 UI 中勾选时才写入；未启用日志时不会创建 `log` 目录或日志文件。取消任务时，未开始的文件会记为取消/待处理，不会被误算成失败。
输入是文件夹时会保留原始子文件夹结构，例如 `input\2026\a.png` 会输出到 `out\2026\a.avif`；输入是单个文件时仍直接输出到目标目录。

常用 CLI 参数：

- `-i, --input <path>`: 输入文件或目录，默认 `input`
- `-o, --output <dir>`: 输出目录，默认与输入同目录
- `-f, --format avif|webp|jxl`: 输出格式，默认 `avif`
- `-q, --quality <1-100>`: ImageMagick 质量，AVIF 默认 `90`，WebP/JXL 默认 `95`，`100` 为无损；也接受 `q90` 或 `0.9`
- `-d, --bit-depth <1-16>`: AVIF 显式填写时支持 `8/10/12`；JXL 留空保持原片，也可按 ImageMagick coder 能力指定；WebP 固定为 `8`
- `--chroma auto|444|422|420`: AVIF 色度采样，默认 `auto`；JXL/WebP 不支持手动采样；也可用 `--444`、`--422`、`--420`
- `-p, --preset fast|balanced|best|extreme`: 预设，默认 `best`
- `-t, --threads <n>`: 并发数量，默认 CPU 线程数
- `-m, --template <模板>`: 输出文件名模板，默认 `{name}`，扩展名由 `--format` 决定；模板只生成文件名，路径分隔符和 Windows 非法字符会替换为 `_`
- `--max-resolution <px>`: 限制最长边；`0` 表示不缩放
- `--speed <0-10>`: 可选，AVIF 映射到 `heic:speed`，JXL 映射到 `jxl:effort`
- `--optimize`: 自动搜索达到目标 XPSNR 的最小体积版本，速度会明显慢于直接编码
- `--target-xpsnr <dB>`: 自动搜索的最低 XPSNR，默认 `42.0`
- `--min-quality <1-100>`: 自动搜索的最低质量，默认 `50`
- `--define <key=value>`: 高级选项，额外映射到 `MagickSetOption(key, value)`，可重复；key 不能为空，不能包含控制字符，日志/summary 会隐藏 token、secret、password、credential、api-key 等疑似敏感值
- `--magick <path>`: 指定 ImageMagick 运行时目录，或其目录中的文件路径；不传时优先使用 exe 旁边的 bundled runtime/config
- `--strip`: 去除 EXIF/ICC 等元数据
- `--skip-existing`: 已有输出时跳过
- `--overwrite`: 覆盖已有输出，默认行为
- `--suffix-time` / `--suffix-random`: 输出名追加时间或随机后缀
- `--summary` / `--log`: 可选生成 `summary.csv` 或 `log\avif-console.log`

命名模板支持 `{index}`、`{name}`、`{ext}`、`{date}`、`{time}`、`{datetime}`、`{unix}`、`{rand}`、`{hash}`、`{hash8}`、`{params}`。`{params}` 会展开为编码参数，例如 `q95t5`；显式指定色度采样或位深时追加为 `q95t5_444_10`。

## AVIF/JXL 采样与位深

当前后端遵循 ImageMagick/libheif、JPEG XL delegate 与 WebP coder 的能力边界：

- AVIF 容器/AV1 支持 8-bit、10-bit、12-bit；本程序在留空位深时保持原片，显式填写时允许 `8/10/12`。`auto` 会尽量保持源采样：对 AVIF/HEIC 通过 libheif 读取原始 chroma，对 JPEG 读取 `jpeg:sampling-factor`；无法识别源采样时不强制写 `heic:chroma`。手动 `444/422/420` 会映射为 `heic:chroma`。
- JXL 不使用 `heic:chroma`，也不支持 `444/422/420` 手动采样。位深留空时保持原图语义；显式 `--bit-depth` 会调用 `MagickSetImageDepth`，高级行为可通过 `--define jxl:*` 按 ImageMagick 规范控制。
- WebP bitstream 是 8-bit：有损 WebP 是 8-bit Y'CbCr 4:2:0；无损 WebP 是 8-bit ARGB。因此选择 WebP 时位深固定为 `8`，也不提供 `444/422/420` 手动采样。
- 如果原图是 4:4:4、4:2:2、4:2:0：AVIF 在 `auto` 能识别源采样时会分别写入 444、422、420，手动选择时按 `--chroma` 写入；JXL 由 JPEG XL delegate 处理色彩和通道语义；WebP 有损会变成 4:2:0，WebP 无损保持 ARGB 语义。

JXL 输出依赖 ImageMagick runtime 带 JPEG XL delegate。若 runtime 不支持 JXL，程序会失败并提示需要重新构建带 JPEG XL delegate 的 ImageMagick。

## 项目结构

- `CMakeLists.txt`: CMake 主构建，拉取 Slint，链接 scnlib 和 MagickWand
- `scripts\build-magick.ps1`: 拉取、构建并提取 ImageMagick Windows 运行时
- `src\cli\main.cpp`: CLI 入口，控制台 UTF-8 初始化和异常兜底
- `src\ui\main.cpp`: Slint UI 入口，文件/文件夹选择、后台转换、取消和打开目录
- `ui\avif_studio.slint`: 桌面 UI
- `ui\fonts`: UI 嵌入字体，界面统一使用鸿蒙黑体
- `src\app\config.ixx`: 参数、预设、帮助文本，数值输入使用 scnlib
- `src\core\process.ixx`: 路径编码、图片扫描、日志、CSV 和少量 Win32 工具
- `src\backends\magick_backend.ixx`: MagickWand 运行时解析与 AVIF/WebP/JXL 编码
- `src\app\pipeline.ixx`: `run_batch` 批处理服务、多线程调度、进度回调、汇总
- `docs\cpp-port.md`: C# 到 C++ 的迁移说明
- `docs\magick-runtime.md`: ImageMagick 运行时构建与打包说明

## 实现要点

- CLI 输出统一使用 UTF-8 字节写入 `stdout` / `stderr`，避免 Windows 控制台代码页造成中文乱码
- 输入解析使用 vcpkg 中的 `scnlib`
- 批处理线程使用 `std::jthread`
- 错误路径使用 `std::expected<T, std::string>` 和异常兜底，单文件失败不会终止整批
- UI 通过 Slint event loop 投递后台线程进度，转换期间不会阻塞界面
- UI 默认字体为鸿蒙黑体，任务列表和技术性英文标签同样使用鸿蒙黑体
- UI 支持跟随 Windows 应用主题，也可以在标题栏手动切换浅色/深色
- Slint 默认静态链接；ImageMagick 可用静态脚本路径尽量减少 DLL，运行时不依赖 PATH 中的 `magick.exe`
- Release 构建默认启用 MSVC 的 `/O2`、函数级裁剪、常量合并和链接器 `/OPT:REF,ICF`；IPO/LTO 可用 `.\release.ps1 -EnableLto` 显式开启
- 转换工作线程会降低 CPU 调度优先级；主进程不再在启动阶段降优先级，避免高负载时窗口/CLI 初始化变慢

## 许可证与第三方组件

本项目源码以 GNU AGPL-3.0-only 发布，完整条款见 [LICENSE](LICENSE)。发布二进制时请同时提供对应源码、保留许可证文本，并确保交互界面或随附文档能让用户看到版权、无担保声明和许可证位置。

项目依赖的第三方组件遵循各自许可证：

- ImageMagick / MagickWand：Apache-2.0；本仓库不提交其源码或二进制，`scripts\build-magick.ps1` 会在本地构建并把 ImageMagick 的 `License.txt` / `NOTICE.txt` 复制到发布目录。
- Slint：通过 CMake `FetchContent` 拉取，当前固定到 `v1.16.1` 对应 ref `e9c1ca295f9356af71f1e251c287de18406b46f6`；发布时需遵守所选 Slint 许可证/商业许可要求，并保留其许可声明。
- scnlib 与其依赖：通过 vcpkg manifest 获取，当前 baseline 由 `vcpkg-configuration.json` 固定为 `56bb2411609227288b70117ead2c47585ba07713`；发布时保留 vcpkg 安装包随附的许可证信息。
- 嵌入字体位于 [ui/fonts/](ui/fonts/)；重新分发时需确认字体许可证允许随应用分发，并保留对应声明。

不要把 [third_party/](third_party/) 下本机构建得到的源码、库或运行时产物直接提交到仓库；发布包应随附实际打包进去的第三方许可文件。
