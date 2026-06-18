# AWJimage

AWJimage 是一个 Windows C++23 / Slint 批量图片转换工具。当前内置转换路径只保留 native codec：

- AVIF：libavif/AOM；实验 `zenrav1e` 静态链接进主程序但仍需显式启用；`svt-av1-hdr` 通过 libavif SVT backend 静态链接进主程序
- WebP：libwebp
- JXL：libjxl
- JPGLI：google/jpegli；使用 Jpegli 生成 JPEG 兼容 bitstream，默认扩展名仍为 `.jpg`

内置 ImageMagick/MagickWand 后端已经移除，Release 输出不再携带 ImageMagick XML、许可文件或模块目录。Magick 与 ffmpeg 以后只能作为外部集成重新引入；当前版本不处理该环节。

## 构建

推荐使用预设：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
```

或使用脚本：

```powershell
.\release.ps1
```

脚本会配置 native 依赖并清理 Release 输出目录，只保留面向用户的：

- `bin\x64\Release\AWJ.exe`
- `bin\x64\Release\AWJ.com`
- `bin\x64\Release\AWJ.exe.sha256`
- `bin\x64\Release\AWJ.com.sha256`
- `bin\x64\Release\LICENSE`
- `bin\x64\Release\THIRD_PARTY_NOTICES.txt`
- `bin\x64\Release\BUILD_INFO.txt`

版本号由仓库根目录的 `VERSION` 文件控制，`CMakeLists.txt` 构建时自动读取，`scripts/Update-VcpkgVersion.ps1` 可同步到 `vcpkg.json`。发版流程：

```powershell
# 1. 更新版本号
Set-Content VERSION "0.8.5"
.\scripts\Update-VcpkgVersion.ps1

# 2. 提交并打 tag
git add VERSION vcpkg.json
git commit -m "release: 0.8.5"
git tag 0.8.5

# 3. 构建
.\release.ps1
```

`svt-av1-hdr` 已作为源码依赖构建并静态链接进主程序，Release 不需要 `SvtAv1EncApp.exe`、DLL 或其他 SVT sidecar。当前 SVT 路径仍限制为 420 色度采样和 8/10-bit AVIF 输出。

`AWJ_ENABLE_JPEGLI` 默认开启，CMake 会拉取 google/jpegli 并静态链接 `jpegli-static`。JPGLI 没有独立容器格式，AWJ 的 UI、CLI、summary 和日志统一显示 `JPGLI` / `jpegli`，但输出文件扩展名默认保持 `.jpg`，以兼容系统缩略图和常见图片查看器。

构建 visual_quality GPU 指标路径时，CMake 会调用 Windows SDK 的 `fxc.exe` 预编译 Direct3D 11 compute shader，并把 bytecode 内嵌进程序。构建机需要安装 Windows SDK；最终用户运行 Release 不需要 `fxc.exe`、`d3dcompiler` 或 `.cso` sidecar。

## visual_quality GPU 指标路径

`--visual-quality` 的 1..99 自动搜索会重复编码候选、从内存解码候选并计算视觉指标。默认启用的 `--visual-quality-gpu` 加速的是指标分析路径：Direct3D 11 compute shader 负责 luma、GMSD、MS-SSIM 和 MS-SSIM downsample。候选 codec 编码/解码、候选选择和最终 encoded bytes 返回仍是 native CPU memory pipeline；这不是端到端 GPU 转码。

GPU 不可用、小图低于收益阈值、资源超限或 GPU readback/shader 创建失败时会自动回退 CPU。启用 `--summary` 或 `--log` 后，可通过 `visual_quality_gpu_requested`、`visual_quality_gpu_used`、`visual_quality_gpu_path`、`visual_quality_gpu_fallback_reason`、`visual_quality_gpu_fallback_count` 以及各阶段耗时判断实际路径。

## CLI 示例

命令行建议直接输入 `AWJ ...`；在 Windows 终端中会优先使用同目录的 `AWJ.com` shim，并转发到 `AWJ.exe`，从而保留等待、stdout/stderr 和退出码。双击 `AWJ.exe` 会直接打开 Studio，不弹出命令行窗口。

```powershell
AWJ -i "D:\图片" -o Avifoutput --format avif -q q90
AWJ -i input.png --format webp --template "{name}-{date}"
AWJ -i input.png -o output.jxl --format jxl -q 90
AWJ -i input.png --format jpgli -q 95 --summary
AWJ -i input.png --format avif --avif-encoder aom --chroma 444 --bit-depth 10
AWJ -i input.png --format avif --avif-encoder zenrav1e --experimental-encoders
```

`zenrav1e` 是实验编码器：必须显式选择 `--avif-encoder zenrav1e` 并加 `--experimental-encoders`。默认 `auto` 不会选择实验编码器。

`--format jpgli` 和 `--format jpegli` 是 JPGLI 入口；`--format jpg` 不作为新增入口。启用 `--summary` 后，`summary.csv` 会记录 `format=JPGLI`、`encoder_id=jpegli`，输出路径仍通常是 `.jpg`。

Studio 的编码队列支持拖拽文件/文件夹导入，也可以继续使用“选择输入”按钮。拖入目录时会按现有扫描规则批量处理图片。

## 基准测试

以下测试均使用 AOM AV1 编码器（libavif）。AWJ 自动多核并行，ffmpeg 为单线程每实例。

### 默认参数（613 张混合分辨率图片）

| 指标 | AWJ | ffmpeg 8.1.1（scoop） |
|------|-----|----------------------|
| 并发方式 | 自动多核并行 | 单线程 × 12 并发 |
| 总耗时 | 1,367.2 核秒 | 2,316 核秒 |
| 平均每张 | 2.23 s | 3.78 s |

> ffmpeg 测试通过 PowerShell 传参，可能含额外传参与启动开销。

### 2560×1600 固定分辨率（20 张，AWJ）

平均每张 2.264 s。

### 质量与体积

同视觉质量下，ffmpeg 编码体积大于 AWJ。AWJ 指定同视觉质量所需的 CRF 值比 ffmpeg 高 1–3。该差异部分与 AOM 版本有关——AOM 3.14.0 包含重要质量改进，此处的质量优势不全是 AWJ 自身贡献。

### visual_quality 视觉质量评估

AWJ 的 `--visual-quality` 选项通过 IQA 算法自动搜索最优编码参数：**GMSD**（对模糊与结构失真敏感）与 **MS-SSIM**（多尺度结构相似性）混合评分。

该混合算法兼顾稳定性与速度，在以 3–4 倍于普通编码的时间消耗下，能为指定批次找到同视觉质量下的最小体积。`--visual-quality-gpu` 利用 Direct3D 11 compute shader 加速 luma、GMSD、MS-SSIM 及 MS-SSIM downsample 计算路径；GPU 不可用或小图低于收益阈值时自动回退 CPU。

## 测试

普通构建/验证只构建 AWJ 和 AWJ-com 目标，不构建测试可执行文件。只有明确需要测试验证时，才单独构建测试目标并运行：

```powershell
ctest --test-dir build/x64/Release -C Release --output-on-failure
```
