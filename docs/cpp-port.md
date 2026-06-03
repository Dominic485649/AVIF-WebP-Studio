# C++23 迁移说明

## 目标

本项目已经从早期控制台原型收敛为 Windows C++23 / Slint 批量图片转换工具。迁移目标不是逐行保留旧实现，而是保留批量转码工作流，并把配置、扫描、调度、native codec、CLI 和 Studio UI 拆成可维护的模块。

## 当前后端

当前转换核心只保留 native codec，不再依赖内置 ImageMagick/MagickWand 后端。

- AVIF：libavif/AOM；libavif SVT backend；实验 `zenrav1e` 通过隔离 helper 路径显式启用。
- WebP：libwebp。
- JXL：libjxl。
- PNG/JPEG/TIFF/GIF/WIC/RAW：作为输入解码路径参与批处理和 metadata 透传。

Magick 与 ffmpeg 以后只能作为显式配置的外部 exe/runtime 集成方向，不应恢复为默认内部后端或随 Release 输出携带。

## 当前转换流程

1. native decoder registry 按输入格式选择 PNG/JPEG/WebP/TIFF/GIF/JXL/AVIF/RAW/WIC 等解码器。
2. 解码器返回像素、ICC、EXIF/XMP、色彩/HDR 和 alpha 语义。
3. 根据配置执行可选缩放、alpha 策略、色度采样、位深选择和视觉质量搜索。
4. AVIF auto 只选择允许参与自动选择的稳定 encoder；实验 encoder 必须显式开启并选择。
5. 大图模式按资源规划拆分 grid/tile，并对支持的 AVIF encoder 走专用路径。
6. 编码器写回 ICC、EXIF/XMP、色彩和 HDR metadata；不支持的组合返回明确错误。
7. 输出写入先经过临时文件与碰撞策略，再落到目标路径。
8. 日志与 `summary.csv` 记录实际后端、质量、fallback 和诊断信息。

## 保留的功能

- 批量扫描文件或目录，支持 `jpg/jpeg/png/webp/bmp/dib/rle/tif/tiff/gif/jxl/avif/awsraw`、常见 RAW 扩展以及 `heic/heif/jxr/wdp/hdp`。
- 输出名模板 `{name}` / `{index}` / `{ext}` / `{date}` / `{time}` / `{datetime}` / `{unix}` / `{rand}` / `{hash}` / `{hash8}` / `{params}`，CLI 默认 `{name}`。
- 输入为文件夹时保留原始子文件夹结构。
- 输出格式可选 AVIF、WebP 或 JXL。
- `fast` / `balanced` / `best` / `extreme` 预设。
- AVIF 默认 q90、WebP/JXL 默认 q95，仍支持 `q90` 风格质量参数。
- 质量范围为 q1..q100；JXL q100 对 JPEG 输入在未请求剥离元数据或改写色彩/HDR 时使用原始码流级无损转封装，其他 WebP/JXL q100 为编码器无损；AVIF auto/q100 走直通或 AOM 严格无损，显式 SVT q100/visual-quality 100 为非像素级无损/最高质量路径，允许 RGB/YUV 与 420 chroma 转换损耗。
- AVIF 采样支持 `auto/444/422/420`，位深留空时按源图和编码器能力选择，显式填写时支持 `8/10/12`；显式 SVT 始终实际使用 420 chroma，且只支持 8/10-bit。
- JXL 不支持手动 chroma sampling，位深留空保持原片，可通过 native libjxl effort/speed 控制压缩成本。
- WebP 固定 8-bit；有损 WebP 为 Y'CbCr 4:2:0，无损 WebP 为 ARGB。
- 并行处理，处理时优先调度大文件。
- 重名输出支持覆盖、跳过、追加时间后缀、追加随机后缀。
- 日志与 `summary.csv` 可选生成。
- 单张失败继续处理后续图片。

## 新增内容

- CMake 根工程，生成 `AWJ-cli.exe` 和 `AWJ-studio.exe`。
- Slint 桌面 UI。
- UI 支持跟随 Windows 应用主题，也可以手动切换浅色/深色。
- `run_batch(config, progress_callback, stop_token)` 批处理服务，CLI/UI 共用。
- 大图模式、视觉质量搜索、AVIF encoder registry、资源规划和 GPU 视觉指标路径。
- 批量编码时只降低转换工作线程 CPU 优先级，避免 UI/CLI 启动阶段在高负载下被系统调度饿住。
- Release 默认保留 `/O2`、函数级裁剪、常量合并、链接器裁剪和 x64-v3/AVX2 代码生成；`.\release.ps1 -EnableLto` 可显式启用 IPO/LTO，但默认关闭以优先保证静态依赖组合的运行稳定性。

## 简化的部分

- 移除了内置 ImageMagick/MagickWand 和 ffmpeg/ffprobe 后端；当前质量搜索以 native 视觉质量评分为主。
- 质量参数改为各 native encoder 的质量入口，AVIF/WebP/JXL 由统一配置再映射到具体库。
- 不再默认缩放到固定长边，避免用户误以为编码质量差其实是分辨率被改动。
- 不再提交 Scoop/ImageMagick 二进制，仓库只保留外部集成所需的说明和历史脚本。
- Slint 默认静态链接，减少 UI 分发时 DLL 数量。

## 模块划分

- `awj.config`：参数结构、预设、帮助文本、命令行解析。数值解析使用 vcpkg 的 `scnlib`。
- `awj.core`：UTF-8/宽字符转换、图片扫描、输出路径规划、日志、CSV 和少量 Win32 工具。
- `awj.image` / `awj.codec`：共享图像缓冲、metadata、codec capability 和 encode/decode 数据结构。
- `awj.decoder_registry` / `awj.*_codec`：native 格式解码、编码与 metadata 透传。
- `awj.avif_registry`：AVIF encoder 能力注册与选择策略。
- `awj.native_backend`：native decoder/encoder 调度、AVIF helper、临时文件和输出写入边界。
- `awj.pipeline`：多线程调度、进度事件和汇总。
- `awj.native_visual_search` / `awj.visual_metrics` / `awj.visual_metrics_gpu`：视觉质量搜索、CPU/GPU 质量指标和 fallback 诊断。
- `src\cli\main.cpp`：CLI 入口。
- `src\ui\main.cpp`：Slint UI 入口和 Win32 文件/文件夹选择。

## 进阶改动

- CLI 文本输出使用 C++23 `std::println`，避免继续扩散 `cout`/`printf` 风格输出。
- 批处理线程使用 `std::jthread`；UI 的“强制终止”只请求停止当前任务，不再终止后自动启动新任务。native encoder 接口接收 `std::stop_token`，在进入编码、可控循环、输出收集和写入前后尽早响应取消；第三方同步编码调用本身仍以调用前后的可控边界为准。
- C API、Win32 和 COM 交界处统一用 `std::unique_ptr` 自定义 deleter 或局部 RAII 类型表达拥有关系，覆盖 `Release`、`CoTaskMemFree`、`LocalFree`、libavif/libjxl/libwebp/libraw 等资源释放路径。
- 参数解析、文件系统、decoder/encoder 入口和重要分配路径使用 `std::expected<T, std::string>` 表达错误。
- 索引型循环优先使用 `std::views::iota`，只有依赖迭代器失效、外部 C API 或更清晰的资源生命周期时才保留传统循环。
- 只在所有权、跨线程时序或格式边界不直观的地方保留中文注释，避免普通业务流程被注释噪音淹没。
- 单文件处理内部有异常兜底，某张图片失败不会终止整个批处理。
- 工程保留 vcpkg/CMake 配置，可继续引入高质量第三方库，但优先保持 native codec 路径清晰可验证。

## 构建与验证约束

日常验证只构建 CLI 和 Studio 目标；Release preset 默认启用 `AWJ_ENABLE_X64_V3=ON`，MSVC Release 会为项目目标添加 `/arch:AVX2`：

```powershell
cmake --build --preset windows-msvc-x64-release --target AWJ-cli AWJ-studio
```

测试可执行文件只在明确需要测试验证时单独构建，不能混入普通构建步骤。

## 后续可扩展点

- 如果未来需要更精确的目标体积，可以在 native 视觉质量搜索之外增加体积约束策略。
- 可以把体积较大的源码依赖接入 CI cache，减少首次 Release 构建时间。
- 如需重新接入 Magick/ffmpeg，应以外部 exe/runtime 形式隔离，不能恢复为默认内部后端。
