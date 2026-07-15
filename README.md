# AWJimage

English: [README.en.md](README.en.md)

0.10.3 发布说明：[RELEASE_NOTES_0.10.3.md](RELEASE_NOTES_0.10.3.md)

AWJimage 是一个 C++23 / Slint 批量图片转换工具。Windows 与 Linux 现已合并到同一主线。Windows 保留完整 shell/WIC/D3D11 支持；Linux 提供 Vulkan visual metrics 与 GCC Release ELF。当前内置转换路径只保留 native codec：

- AVIF：libavif/AOM、实验 `zenrav1e` 与 `svt-av1-hdr`；Windows 和 Linux GCC Release 均静态链接
- WebP：libwebp
- JXL：libjxl
- JPGLI：google/jpegli；Windows 与 Linux GCC Release 均可用，生成 JPEG 兼容 bitstream，默认扩展名仍为 `.jpg`

内置 ImageMagick/MagickWand 后端已经移除，Release 输出不再携带 ImageMagick XML、许可文件或模块目录。Magick 与 ffmpeg 以后只能作为外部集成重新引入；当前版本不处理该环节。

Linux 首版保留 Slint UI 与 CLI 共用单个 ELF `AWJ`；visual_quality GPU 指标路径使用 Vulkan，失败、小图或资源超限时自动回退 CPU。WIC、JXR、`AWJ.com` shim 和 Windows 注册表 shell 集成仅限 Windows；Linux 上 WIC 兜底会被忽略并在界面中隐藏。Linux 右键入口使用用户级 Nautilus Scripts 与 Thunar UCA，不需要 sudo。

如需使用ffmpeg作为后端可用参考下面两个仓库

[AVIF-Console](https://github.com/CialloKing/AVIF-Console)

[FFmpegPictureUI](https://github.com/luoye-cpu/ffmpegPictureUI)

## 构建

Windows 推荐使用 MSVC 预设：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
```

Linux 在 WSL 本地路径（推荐 `/home/dominic/Code/Cpp/AWJimage`，不要直接编译 `/mnt/d`）使用 vcpkg `x64-linux` 与 GCC 预设：

```bash
cd /home/dominic/Code/Cpp/AWJimage
cmake --preset linux-gcc-x64-release
cmake --build --preset linux-gcc-x64-release --target AWJ
```

`linux-gcc-x64-debug` 用于 Debug；`linux-gcc-x64-release` 使用 `-O3`、IPO/LTO、`-march=x86-64-v3`、section GC/strip，并静态链接 `libstdc++` / `libgcc`；只生成单个 `AWJ` ELF，没有 `AWJ.com`。0.10.3 GCC 16.1 Release 实测约 53.1 MiB，主要来自静态 Slint、SVT-AV1-HDR、JPGLI/libjxl/libavif 等 native codec 依赖；`readelf -d bin/x64/Release/AWJ` 不应出现 `libstdc++.so.6` 或 `libgcc_s.so.1`。

Windows 也可使用脚本：

```powershell
.\release.ps1
```

Windows 脚本会配置 native 依赖并清理 Release 输出目录，只保留发行文件；若目录中已有 Linux 构建，也会保留 `AWJ` 与 `AWJ.sha256`：

- `bin\x64\Release\AWJ.exe`
- `bin\x64\Release\AWJ.com`
- `bin\x64\Release\AWJ.exe.sha256`
- `bin\x64\Release\AWJ.com.sha256`
- `bin\x64\Release\AWJ`（已有 Linux 构建时）
- `bin\x64\Release\AWJ.sha256`（已有 Linux 构建时）
- `bin\x64\Release\LICENSE`
- `bin\x64\Release\THIRD_PARTY_NOTICES.txt`
- `bin\x64\Release\BUILD_INFO.txt`

版本号由仓库根目录的 `VERSION` 文件控制，`CMakeLists.txt` 构建时自动读取，`scripts/Update-VcpkgVersion.ps1` 可同步到 `vcpkg.json`。发版流程：

```powershell
# 1. 更新版本号
Set-Content VERSION "0.10.3"
.\scripts\Update-VcpkgVersion.ps1

# 2. 提交并打 tag
git add VERSION vcpkg.json
git commit -m "release: 0.10.3"
git tag 0.10.3

# 3. 构建
.\release.ps1
```

`svt-av1-hdr` 与实验 `zenrav1e` 均静态链接进主程序，不需要 sidecar。当前 SVT 路径仍限制为 420 色度采样和 8/10-bit AVIF 输出。

`AWJ_ENABLE_JPEGLI` 默认开启，CMake 会拉取 google/jpegli 并静态链接 `jpegli-static`。JPGLI 没有独立容器格式，AWJ 的 UI、CLI、summary 和日志统一显示 `JPGLI` / `jpegli`，但输出文件扩展名默认保持 `.jpg`，以兼容系统缩略图和常见图片查看器。

构建 visual_quality GPU 指标路径时，Windows 使用 Windows SDK `fxc.exe` 预编译 Direct3D 11 shader；Linux 使用 vcpkg `directx-dxc` 生成 Vulkan SPIR-V。shader 会内嵌进程序，最终用户运行 Release 不需要 shader sidecar。

## visual_quality GPU 指标路径

`--visual-quality` 的 1..99 自动搜索会重复编码候选、从内存解码候选并计算视觉指标。默认启用的 `--visual-quality-gpu` 加速的是指标分析路径：Windows 走 Direct3D 11 compute shader，Linux 走 Vulkan compute shader，负责 luma、GMSD、MS-SSIM 和 MS-SSIM downsample。候选 codec 编码/解码、候选选择和最终 encoded bytes 返回仍是 native CPU memory pipeline；这不是端到端 GPU 转码。

GPU 不可用、小图低于收益阈值、资源超限或 GPU readback/shader 创建失败时会自动回退 CPU。启用 `--summary` 或 `--log` 后，可通过 `visual_quality_gpu_requested`、`visual_quality_gpu_used`、`visual_quality_gpu_path`、`visual_quality_gpu_fallback_reason`、`visual_quality_gpu_fallback_count` 以及各阶段耗时判断实际路径。

## CLI 示例

Windows 命令行建议直接输入 `AWJ ...`；终端中会优先使用同目录的 `AWJ.com` shim，并转发到 `AWJ.exe`，从而保留等待、stdout/stderr 和退出码。Linux 直接运行单个 ELF `AWJ`，没有 `AWJ.com`。无参数启动 Slint Studio，带 CLI 参数进入命令行转换。

```powershell
AWJ -i "D:\图片" -o Avifoutput --format avif -q q90
AWJ -i input.png --format webp --template "{name}-{date}"
AWJ -i input.png -o output.jxl --format jxl -q 90
AWJ -i input.png --format jpgli -q 95 --summary
AWJ -i input.png --format avif --avif-encoder aom --chroma 444 --bit-depth 10
AWJ -i input.png --format avif --avif-encoder zenrav1e --experimental-encoders
```

`zenrav1e` 是实验编码器：普通单图必须显式选择 `--avif-encoder zenrav1e` 并加 `--experimental-encoders`；自动大图链路可按资源使用它。Windows 与 Linux GCC Release 均可用。

### AVIF 大图与队列策略

AVIF 普通单图会尽量留在普通编码队列：AOM/libaom 上限为宽高各 1..65536 且总像素不超过 `2^30`（1,073,741,824）；`svt-av1-hdr` 上限为 16384×8704。超过 1000 万像素但仍在单图上限内的图片会排到普通队列末尾，避免单张大图的内存估算降低全部小图的并发；它们仍使用普通编码器，不会强制进入大图链路。总批次超过 12 张时，普通、延后和大图阶段都保持每张图片单编码线程。

AVIF 输入需要保留透明通道时，颜色与 alpha 会一起自动使用 AOM q100/4:4:4 无损编码，用户设置的 `speed` 保持不变；`--alpha off` 仍按请求的质量与 speed 编码。

超过 AOM 单图上限后自动进入大图链路：
1. 默认优先 `zenrav1e`，失败或不支持再回退 `grid`；
2. 参数页“大图优先”可改为 `grid` 优先再回退 `zenrav1e`；
3. 两条路径都不可用/都失败，或触达输入/运行时内存上限时明确报错。

Studio 不再提供独立大图页；自动处理状态直接显示在主队列。CLI：
- `--large-image-priority zenrav1e|grid`
- `--unlock-max-input-file-bytes` / `--unlock-20gib-limit`：仅当前会话解除默认 20 GiB 输入/运行时上限，不写入 `AWJ.jsonc`；过大图片可能 OOM。
参数设置页其余编码参数同样只在本次运行内保持，不写入 `AWJ.jsonc`。

`--format jpgli` 和 `--format jpegli` 是 JPGLI 入口；`--format jpg` 不作为新增入口。启用 `--summary` 后，`summary.csv` 会记录 `format=JPGLI`、`encoder_id=jpegli`，输出路径仍通常是 `.jpg`。

`--allow-wic-fallback` 仅 Windows 有效；Linux 会接受但忽略该参数。Linux UI 的“选择”按钮会调用 `zenity`/`yad`/`kdialog`。右键菜单为 AVIF、WebP、JXL、JPGLI、PNG 分别写入 Nautilus 用户脚本，并同步写入 Thunar UCA；必要时重启文件管理器。

多帧 WebP/GIF/APNG/JXL/TIFF/AVIF、Windows WIC 多帧和 JPEG MPF 输入只转换合成后的第一帧；无法可靠提取时直接报错。AVIF grid 支持非整数倍布局，右列和底行使用实际剩余尺寸，不会改变输出宽高。

Studio 的编码队列支持拖拽文件/文件夹导入，也可以继续使用“选择输入”按钮。拖入目录时会按现有扫描规则批量处理图片；主页队列中未开始的项目可直接拖动调整顺序，右键仍可打开队列菜单。0.10.3 增加待处理、处理中、成功和失败计数、仅看失败、重试失败项及选中项详情；详情保留完整错误、输入/输出路径、编码器、线程数和 decode/prepare/encode/write 阶段耗时。

SoftComboBox、SoftButton、左侧导航与队列右键菜单提供 Tab 焦点、可见焦点环、Enter/Space、方向键、Home/End、Esc 以及可访问角色/名称。字体下拉框仍最多显示 10 行，支持滚轮和滚动条，不提供搜索或手动输入。参数页按常用参数、资源限制、格式高级选项分组，危险警告常驻，其余长说明收进帮助提示。

Windows Explorer 多选图片或文件夹后执行同一个 AWJ 右键命令时，启动请求会合并到一个右键窗口和同一队列。右键窗口提供普通取消与强制终止；本体和右键窗口点击右上角关闭时会先终止活动任务。

## 基准测试

Windows Release 的 canonical 基准使用固定 AVIF/AOM `quality=80`、`speed=6`、`chroma=420`、8-bit 和当前电源方案：

```powershell
& .\scripts\benchmark.ps1 -PowerSchemeGuid 381b4222-f694-41f0-9685-ff5bb260df2e -Surface cli
```

脚本先预热一次，再运行五次（P95 使用 nearest-rank）；混合输入覆盖 1、4、12、13、613 张，透明与不透明输入分别覆盖 1、13 张。0.10.3 按当前测试约定只运行 CLI；`studio` 仍只是同一个 CLI worker 的 manifest policy，`shell` 也是 CLI policy，不需要窗口自动化。`report.md`、`summary.csv`、`runs.csv`、`metadata.json` 和输入 SHA-256 清单写入 `build/benchmarks/`。canonical 运行要求源码工作树干净（允许 `release.ps1` 重建的已跟踪 Release 产物变化）、匹配当前 commit 的 `BUILD_INFO.txt`、Release 构建和显式电源方案；`-Smoke` 只用于验证基准工具，不可作为性能结论。

报告中的 `Process CPU` 是 Windows 进程 CPU 时间；`Item seconds sum` 是 `summary.csv` 逐图 `seconds` 之和，仅用于和下方历史数据比较。两者以及整批墙钟时间不得混用。

### 0.10.3 canonical CLI 结果

固定输入清单 SHA-256 为 `4CDFC310837192A7BDAC149C1C212ECD8F720AEE6EECB954D218D66D1E424628`。0.10.2 与 0.10.3 使用相同输入、AOM 3.13.3、libavif commit、电源方案、Release 构建、q80、speed6、4:2:0、8-bit 和 12 线程总预算；613 张均为 612 成功、1 个已知空 WebP 失败。

早期记录的 1367.171 与 1660.888 核秒使用了不同色度/协议，不能互相比较，也不作为下表的回归基线。

| 613 张 CLI | 0.10.2 `ec61551` | 0.10.3 `2798db2` |
|---|---:|---:|
| 墙钟中位数 / P95 | 214.361 / 226.493 s | 162.660 / 166.395 s |
| 进程 CPU 中位数 / P95 | 1613.906 / 1624.469 s | 1381.844 / 1399.391 s |
| 峰值内存中位数 / P95 | 无效（旧采样缓存） | 2807.7 / 2822.4 MiB |
| 吞吐量中位数 | 2.860 img/s | 3.769 img/s |
| decode / prepare / encode / write 中位数 | 51.107 / 0.346 / 2189.732 / 15.396 s | 40.749 / 0.314 / 1668.552 / 10.198 s |
| encode 阶段占比 | 97.0% | 97.0% |

0.10.3 五轮墙钟为 158.326、166.395、162.366、162.660、163.967 s。旧 0.10.2 峰值内存值来自未刷新 `Process` 缓存的首次采样，不能用于版本比较；0.10.3 已修正采样，墙钟、CPU 和阶段计时不受该问题影响。本版没有修改 CLI 编码路径、AOM 参数、quality、speed、色度、位深或线程规则，因此表中更快的中位数只作为本机观测结果，不归因于 UI 改动，也不据此实施 libaom 微优化。严格回归未复现，未启动 ETW/perf。

| 0.10.3 CLI 小矩阵 | 墙钟中位 / P95 (s) | CPU 中位 / P95 (s) | 峰值中位 / P95 (MiB) | 吞吐量中位 (img/s) | decode / prepare / encode / write 中位 (s) |
|---|---:|---:|---:|---:|---:|
| mixed-1 | 1.577 / 3.648 | 2.656 / 3.703 | 104.7 / 105.0 | 0.634 | 0.054 / 0.000 / 1.264 / 0.008 |
| mixed-4 | 1.804 / 2.447 | 8.094 / 8.625 | 317.2 / 321.5 | 2.217 | 0.430 / 0.001 / 3.495 / 0.028 |
| mixed-12 | 6.295 / 6.600 | 25.406 / 26.031 | 826.1 / 860.8 | 1.906 | 1.290 / 0.007 / 27.062 / 0.093 |
| mixed-13 | 11.004 / 12.692 | 30.312 / 33.734 | 786.6 / 801.8 | 1.181 | 1.146 / 0.017 / 32.912 / 0.133 |
| opaque-1 | 0.605 / 0.635 | 1.875 / 2.000 | 119.9 / 120.3 | 1.653 | 0.086 / 0.000 / 0.370 / 0.005 |
| transparent-1 | 1.150 / 1.249 | 1.969 / 2.141 | 121.7 / 121.8 | 0.870 | 0.018 / 0.000 / 0.995 / 0.007 |
| opaque-13 | 5.887 / 6.433 | 26.500 / 27.531 | 897.0 / 918.3 | 2.208 | 1.806 / 0.014 / 29.310 / 0.120 |
| transparent-13 | 6.733 / 7.040 | 27.734 / 28.250 | 882.9 / 902.0 | 1.931 | 0.463 / 0.000 / 32.402 / 0.146 |

以下为历史基准，AWJ 使用固定 `quality=80`、`speed=6`，不是当前 AVIF `quality=70` 的默认参数。612 张成功项中 610 张使用 AOM/libavif，2 张超过普通单图路径后使用 zenrav1e；另有 1 张空 WebP 按预期失败。AWJ 自动多核并行，ffmpeg 为单线程每实例。

### 固定 q80 / speed6，FFmpeg 传递同样参数（613 张混合分辨率图片）

| 指标 | AWJ | ffmpeg 8.1.1（scoop） |
|------|-----|----------------------|
| 并发方式 | 自动多核并行 | 单线程 × 12 并发 |
| 总耗时 | 1,367.2 核秒 | 2,316 核秒 |
| 平均每张 | 2.23 核秒 | 3.78 核秒 |

> 表中总耗时为逐图 `seconds` 之和，不是整批墙钟时间。ffmpeg 测试通过 PowerShell 传参，可能含额外传参与启动开销。

### 2560×1600 固定分辨率（20 张，AWJ）

平均每张 2.264 核秒。

### 质量与体积

同视觉质量下，ffmpeg 编码体积大于 AWJ。AWJ 指定同视觉质量所需的 CRF 值比 ffmpeg 高 1–3。该差异部分与 AOM 版本有关——AOM 3.14.0 包含重要质量改进，此处的质量优势不全是 AWJ 自身贡献。

### visual_quality 视觉质量评估

AWJ 的 `--visual-quality` 选项通过 IQA 算法自动搜索最优编码参数：**GMSD**（对模糊与结构失真敏感）与 **MS-SSIM**（多尺度结构相似性）混合评分。

该混合算法兼顾稳定性与速度，在以 3–4 倍于普通编码的时间消耗下，能为指定批次找到同视觉质量下的最小体积。`--visual-quality-gpu` 在 Windows 使用 Direct3D 11、在 Linux 使用 Vulkan 加速 luma、GMSD、MS-SSIM 及 MS-SSIM downsample 计算路径；GPU 不可用或小图低于收益阈值时自动回退 CPU。

## 测试

普通构建/验证只构建面向平台的发布目标：Windows 为 AWJ 和 AWJ-com，Linux 为 AWJ，不构建测试可执行文件。只有明确需要测试验证时，才单独构建测试目标并运行：

```powershell
ctest --test-dir build/x64/Release -C Release --output-on-failure
pwsh -NoProfile -File .\scripts\cli-worker-smoke.ps1
```

`cli-worker-smoke.ps1` 不启动 Studio 窗口：它通过真实 CLI manifest 验证 ITEM/DETAIL、失败项重试输入、命名事件普通取消和 Job Object 强制终止进程树。Slint 的小型 component smoke 使用 testing backend，无窗口验证页面切换、键盘、字体滚动、深浅色、820×560 与 100%/150%/200% scale。0.10.3 Windows MSVC Release 为 31/31 通过。

Linux GCC Release 测试：

```bash
cmake --preset linux-gcc-x64-release -DBUILD_TESTING=ON
cmake --build --preset linux-gcc-x64-release
ctest --test-dir build/linux-gcc-x64-release --output-on-failure
```

0.10.3 Linux GCC 16.1 Release 为 16/16 通过；ELF 不依赖动态 `libstdc++.so.6` / `libgcc_s.so.1`，并确认 `-O3`、LTO、`x86-64-v3` 与静态 C++/GCC runtime 标志。
