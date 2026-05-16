# C++23 迁移说明

## 目标

本分支把原来的 C# 控制台迁移为现代 C++23 项目。重点不是逐行照搬，而是保留批量转码工作流，同时把后端、CLI 和 UI 拆成可维护的模块。

## 当前后端

当前默认后端是 ImageMagick `MagickWand` API，不再调用 `magick.exe` 子进程。编码流程：

1. `MagickReadImage`
2. `MagickAutoOrientImage`
3. 可选最长边缩放
4. 可选 `MagickStripImage`
5. 同时设置 `MagickSetCompressionQuality` 和 `MagickSetImageCompressionQuality`，未显式指定时 AVIF 用 q90，WebP 用 q95；q100 走无损路径
6. 显式指定目标位深时调用 `MagickSetImageDepth`；AVIF 支持 8/10/12，留空保持原片，WebP 固定 8
7. AVIF 输出时可选 `MagickSetOption("heic:speed", value)` 和 `MagickSetOption("heic:chroma", value)`
8. `--define key=value` 映射到 `MagickSetOption(key, value)`
9. `MagickSetImageFormat("AVIF" / "WEBP")`
10. `MagickWriteImage`

如果启用 `--optimize`，第 5 到第 10 步会变成候选搜索：在 `--min-quality` 到 `--quality` 间编码多个候选文件，用 MagickWand 解码候选并计算亮度 XPSNR 风格评分，最后保留达到目标 XPSNR 的最小体积版本。搜索沿用原 C# 的“边界验证 + 二分 + 邻域复查”思路，对目标值加 `0.05 dB` 安全边距，再寻找最低合格质量附近的最小体积候选。这个模式独立于手动固定 `--define`；手动 `--define` 会作为约束应用到每个候选图。

运行时查找顺序：

1. `--magick` 指定的 ImageMagick 运行时目录
2. `AVIF_MAGICK` 环境变量
3. 程序输出目录
4. 仓库中的 `third_party\imagemagick-runtime\x64\Release`
5. 外部运行时只通过 `--magick` 或 `AVIF_MAGICK` 显式指定

正式分发应使用 `scripts\build-magick.ps1` 生成自编译运行时，fallback 只用于本地开发。

## 保留的功能

- 批量扫描文件或目录，支持 `jpg/jpeg/png/webp/bmp/tif/tiff/gif/jxl/jp2/heic/heif/avif`
- 输出名模板 `{name}` / `{index}` / `{ext}` / `{date}` / `{time}` / `{datetime}` / `{unix}` / `{rand}` / `{hash}` / `{hash8}` / `{params}`，CLI 默认 `{name}`
- 输入为文件夹时保留原始子文件夹结构
- 输出格式可选 AVIF 或 WebP
- `fast / balanced / best / extreme` 预设
- AVIF 默认 q90、WebP 默认 q95，仍支持 `q90` 风格质量参数
- 质量范围为 q1..q100，q100 为无损
- AVIF 采样支持 `auto/444/422/420`，位深留空时保持原片，显式填写时支持 `8/10/12`；`auto` 会尽量从 AVIF/HEIC 或 JPEG 元数据保持源采样
- WebP 固定 8-bit；有损 WebP 为 Y'CbCr 4:2:0，无损 WebP 为 ARGB
- 并行处理，处理时优先调度大文件
- 重名输出支持覆盖、跳过、追加时间后缀、追加随机后缀
- 日志与 `summary.csv` 改为可选生成
- 单张失败继续处理后续图片
- 自动搜索达到目标 XPSNR 的最小体积版本

## 新增内容

- CMake 根工程，生成 `AVIF-WebP-Cli.exe` 和 `AVIF-WebP-Studio.exe`
- Slint 桌面 UI
- UI 支持跟随 Windows 应用主题，也可以手动切换浅色/深色
- `run_batch(config, progress_callback, stop_token)` 批处理服务，CLI/UI 共用
- `scripts\build-magick.ps1` 用于拉取、配置、编译并提取自编译 ImageMagick Windows 运行时
- 批量编码时只降低转换工作线程 CPU 优先级，避免 UI/CLI 启动阶段在高负载下被系统调度饿住
- Release 默认保留 `/O2`、函数级裁剪、常量合并和链接器裁剪；`.\release.ps1 -EnableLto` 可显式启用 IPO/LTO，但默认关闭以优先保证静态依赖组合的运行稳定性。

## 简化的部分

- 移除了 ffmpeg/ffprobe 后端；原 CRF 搜索改为 MagickWand 质量候选和 XPSNR 风格测量。
- 质量参数改为 ImageMagick 的 `quality`，语义更直接。
- 修正了 AVIF 质量传递：ImageMagick 的 HEIC/AVIF coder 读取 `image_info->quality`，仅设置当前 `Image` 的 quality 不足以等价于命令行 `-quality`。
- 不再默认缩放到固定长边，避免用户误以为编码质量差其实是分辨率被改动。
- 不再提交 Scoop/ImageMagick 二进制，仓库只保留构建脚本和文档。
- Slint 默认静态链接，减少 UI 分发时的 DLL 数量。

## 模块划分

- `avif.config`: 参数结构、预设、帮助文本、命令行解析。数值解析使用 vcpkg 的 `scnlib`。
- `avif.core`: UTF-8/宽字符转换、图片扫描、日志、CSV 和少量 Win32 工具。
- `avif.magick_backend`: MagickWand 运行时解析、环境变量配置、AVIF/WebP 编码。
- `avif.pipeline`: 多线程调度、进度事件和汇总。
- `src\cli\main.cpp`: CLI 入口。
- `src\ui\main.cpp`: Slint UI 入口和 Win32 文件/文件夹选择。
- `ui\fonts`: Slint 编译时嵌入字体。

## 进阶改动

- 输出路径统一使用 `<print>` 的 `std::print` / `std::println`。
- 批处理线程使用 `std::jthread`，UI 取消会停止调度新的任务。
- 参数解析和 MagickWand 调用使用 `std::expected<T, std::string>` 表达错误。
- 单文件处理内部有异常兜底，某张图片失败不会终止整个批处理。
- 工程保留 vcpkg/CMake 配置，可继续引入 Boost 等第三方库。

## 后续可扩展点

- 如果未来需要更精确的目标体积，可以在 `avif.magick_backend` 上增加体积约束搜索；当前实现以 XPSNR 阈值为质量底线，并在合格候选中取最小体积。
- 可以增加 manifest 模式的 vcpkg 配置，减少本机路径差异。
- 可以把 ImageMagick 构建产物接入 CI cache，减少首次 Release 构建时间。
