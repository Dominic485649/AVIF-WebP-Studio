# AWJimage

AWJimage 是一个 Windows C++23 / Slint 批量图片转换工具。当前内置转换路径只保留 native codec：

- AVIF：libavif/AOM；实验 `zenrav1e` 静态链接进主程序但仍需显式启用；`svt-av1-hdr` 通过 libavif SVT backend 静态链接进主程序
- WebP：libwebp
- JXL：libjxl

内置 ImageMagick/MagickWand 后端已经移除，Release 输出不再携带 ImageMagick XML、许可文件或模块目录。Magick 与 ffmpeg 以后只能作为外部集成重新引入；当前版本不处理该环节。

## 构建

推荐使用预设：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ
```

或使用脚本：

```powershell
.\release.ps1
```

脚本会配置 native 依赖并清理 Release 输出目录，只保留面向用户的：

- `bin\x64\Release\AWJ.exe`

`svt-av1-hdr` 已作为源码依赖构建并静态链接进主程序，Release 不需要 `SvtAv1EncApp.exe`、DLL 或其他 SVT sidecar。当前 SVT 路径仍限制为 420 色度采样和 8/10-bit AVIF 输出。

构建 visual_quality GPU 指标路径时，CMake 会调用 Windows SDK 的 `fxc.exe` 预编译 Direct3D 11 compute shader，并把 bytecode 内嵌进程序。构建机需要安装 Windows SDK；最终用户运行 Release 不需要 `fxc.exe`、`d3dcompiler` 或 `.cso` sidecar。

## visual_quality GPU 指标路径

`--visual-quality` 的 1..99 自动搜索会重复编码候选、从内存解码候选并计算视觉指标。默认启用的 `--visual-quality-gpu` 加速的是指标分析路径：Direct3D 11 compute shader 负责 luma、GMSD、MS-SSIM 和 MS-SSIM downsample。候选 codec 编码/解码、候选选择和最终 encoded bytes 返回仍是 native CPU memory pipeline；这不是端到端 GPU 转码。

GPU 不可用、小图低于收益阈值、资源超限或 GPU readback/shader 创建失败时会自动回退 CPU。启用 `--summary` 或 `--log` 后，可通过 `visual_quality_gpu_requested`、`visual_quality_gpu_used`、`visual_quality_gpu_path`、`visual_quality_gpu_fallback_reason`、`visual_quality_gpu_fallback_count` 以及各阶段耗时判断实际路径。

## CLI 示例

```powershell
AWJ.exe -i "D:\图片" -o Avifoutput --format avif -q q90
AWJ.exe -i input.png --format webp --template "{name}-{date}"
AWJ.exe -i input.png -o output.jxl --format jxl -q 90
AWJ.exe -i input.png --format avif --avif-encoder aom --chroma 444 --bit-depth 10
AWJ.exe -i input.png --format avif --avif-encoder zenrav1e --experimental-encoders
```

`zenrav1e` 是实验编码器：必须显式选择 `--avif-encoder zenrav1e` 并加 `--experimental-encoders`。默认 `auto` 不会选择实验编码器。

## 测试

普通构建/验证只构建 AWJ 目标，不构建测试可执行文件。只有明确需要测试验证时，才单独构建测试目标并运行：

```powershell
ctest --test-dir build/x64/Release -C Release --output-on-failure
```
