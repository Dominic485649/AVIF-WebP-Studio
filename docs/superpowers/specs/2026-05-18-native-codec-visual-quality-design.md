# Native codec 视觉质量与资源控制设计

## 背景

AVIF-WebP-Studio 将从 ImageMagick 转换后端迁移到专门编解码器后端。新后端同时支持 AVIF、WebP、JXL 输出，并保留现有公开参数语义：`quality`、`visual_quality`、`speed`、位深、色度采样、元数据处理、summary 和日志。

本设计同时合并视觉质量模式与更严格的 CPU/内存控制。`visual_quality` 是面向用户的观感目标；程序内部只搜索 encoder quality，计算 GMSD 与 MS-SSIM，归一化为 Qg/Qm，再合成为 `visual_score`，最终在达标候选中选择文件体积最小者。

移除 ImageMagick 的主要原因是资源与参数控制。ImageMagick delegate 会隐藏 codec 内部线程、内存行为与参数映射；native codec pipeline 必须让 encoder threads、内存预算、speed 映射与 visual quality 搜索都变得显式、可测试、可记录。

## 目标

- 优先使用现代 C++ 与当前项目可用的高版本 C++ 能力，包括 RAII、强类型 enum、`std::expected`、span/view、必要时使用 ranges、智能指针 + 自定义 deleter、move-only ownership 类型。
- 避免 C 风格所有权、手动清理、可避免的全局可变状态，以及 codec/image buffer 路径中的可避免拷贝。
- 用专门库 API 替代 Magick 转换路径，不在正式转换路径中启动外部编码器进程。
- 输入解码尽量使用专业解码器，WIC 只作为最后兜底。
- 输出编码全部使用专门编码器：
  - AVIF：libheif + `https://github.com/juliobbv-p/svt-av1-hdr.git` 的 SVT-AV1 HDR
  - WebP：libwebp
  - JXL：libjxl
- 保留现有公开参数语义。
- 保留 `quality=100` 表示无损的行为。
- 新增 `visual_quality=1..100`；`100` 为无损，`1..99` 只搜索 quality。
- 确保 visual quality 搜索永远不改变 speed、effort、preset、tune。
- 新增 CPU 和内存资源规划。
- native 后端替代成功后，删除不再需要的 Magick 依赖、构建脚本、运行时产物与文档，降低硬盘占用。
- 保留后续接入 FFmpeg/Magick 后端的接口，但第一版不实现它们。

## 非目标

- 不兼容任意 Magick `--define`。
- 不在正式转换路径使用 `SvtAv1EncApp.exe`、`cwebp.exe`、`cjxl.exe` 这类外部 CLI 编码器。
- 第一版不实现 FFmpeg 后端。
- 第一版不实现 Magick 后端。
- 不跨候选强行复用 SVT encoder handle；只复用 buffer、配置模板和输入帧包装。
- UI 不显示 `preset fast|balanced|best|extreme`。

## 架构

### 核心数据模型

新增 codec-neutral 数据结构：

- `ImageBuffer`
  - 宽高
  - 像素格式
  - 位深
  - alpha 模式
  - 帧数/动图信息
  - ICC、EXIF、XMP 元数据
  - 拥有型像素存储和非拥有 span/view 访问器
- `PreparedFrame`
  - encoder-specific 准备结果，例如 RGB、RGBA、YUV444/YUV422/YUV420、8-bit、10-bit、12-bit buffer
  - 按输入图片和编码设置组缓存
- `EncodeSettings`
  - 输出格式
  - quality
  - optional visual quality
  - speed
  - lossless flag
  - bit depth
  - chroma mode
  - metadata strip 行为
  - AVIF tune IQ 设置
  - 当前文件的资源分配结果
- `EncodeResult`
  - 输出路径与大小
  - 最终 encoder quality
  - speed 映射结果
  - encoder 线程数
  - memory budget
  - decoder/encoder backend ID
  - visual quality 指标字段

### 解码层

使用 `DecoderRegistry` 根据文件签名和扩展名选择解码器。专业解码器优先于 WIC。

优先覆盖：

- JPEG：libjpeg-turbo
- PNG：libpng
- TIFF：libtiff
- GIF：giflib 或等价专门 GIF 解码器
- BMP：轻量内置 decoder；如果不值得引入额外依赖，则 WIC 兜底
- WebP：libwebp decode
- JXL：libjxl decode
- AVIF/HEIC/HEIF：libheif decode
- JP2/J2K：OpenJPEG
- RAW：libraw，如果 vcpkg 接入成本可接受
- fallback：WIC，并在日志/summary 中记录 `decoder=wic`

允许 WIC 兜底，但必须可见记录，让用户知道该文件没有走专业解码器路径。

### 编码层

使用 `EncoderRegistry` 根据输出格式选择编码器。

- `SvtAvifEncoder`
  - libheif container 写入
  - SVT-AV1 HDR encoder plugin 或直接 libheif encoder 集成
  - 默认启用 `tune=iq`
  - UI 不允许关闭 `tune=iq`
  - CLI 可以显式关闭，例如 `--avif-tune-iq off`
- `WebpEncoder`
  - libwebp encode
  - lossy/lossless 路径
- `JxlEncoder`
  - libjxl encode
  - lossy/lossless 路径
  - libjxl thread runner 受 ResourcePlanner 控制

每个 encoder 暴露 `CodecCapabilities`：

- 支持的输出格式
- lossless 支持
- quality range
- speed range
- bit depth 支持
- alpha 支持
- chroma 支持
- thread control 支持
- 内存行为估算
- visual quality search 支持

### 后端接口

第一版实现 `NativeCodecBackend`。保留未来 provider 接口：

- `native`
- future `ffmpeg`
- future `magick`

`CodecBackend` 需要提供：

- 解码输入为 `ImageBuffer`
- 固定 quality 编码
- 无损编码
- visual quality 搜索编码
- 返回 diagnostics 和 capabilities

未来 FFmpeg/Magick 后端实现同一接口，不影响 UI、CLI、资源规划、视觉指标、summary 和日志。

## 公开参数语义

### `quality`

- 保持现有公开含义：encoder 输入质量。
- `quality=100` 对 AVIF/WebP/JXL 都表示无损。
- 未提供 `visual_quality` 时，`quality` 直接控制输出。

### `visual_quality`

- 范围 1..100。
- 存在时覆盖 `quality`。
- `visual_quality=100` 直接无损，跳过搜索。
- `visual_quality=1..99` 只搜索 encoder quality。
- 不改变 speed、effort、SVT preset、WebP method、JXL effort、AVIF tune。
- 记录 requested visual quality、final quality、raw metrics、normalized scores、attempt count、output size、lossless state。

### `speed`

UI 只显示 speed 0..10。UI 不显示 `preset fast|balanced|best|extreme`。

映射方向：数值越大，编码越快，压缩效率通常越低。

- AVIF/SVT：speed 映射到 SVT preset。
- WebP：speed 映射到 libwebp method 或等价速度档。
- JXL：speed 反向映射到 libjxl effort。

映射表集中定义并测试。visual quality 搜索期间 speed 映射结果固定不变。

### CLI `preset`

CLI 可以暂时保留 `--preset fast|balanced|best|extreme` 兼容旧脚本。它只设置默认 quality、speed、timeout。显式 `--quality`、`--visual-quality` 或 `--speed` 覆盖 preset 对应字段。UI 不显示 preset。

### Magick `--define`

Native 后端不支持任意 Magick define。

行为：

- 已知历史 key 给出迁移错误：
  - `heic:speed` -> 使用 `--speed`
  - `heic:chroma` -> 使用 `--chroma`
  - `jxl:effort` -> 使用 `--speed`
  - `webp:method` -> 使用 `--speed`
- 未知 key 明确失败：native codec backend 不支持 Magick define。
- 后续高级控制应新增明确 codec-specific CLI 参数。

## 视觉质量评分

使用既定评分模型：

- 先计算 GMSD。
- 再计算 MS-SSIM。
- GMSD 归一化为 Qg，范围 1..99。
- MS-SSIM 归一化为 Qm，范围 1..99。
- `visual_score = GMSD_WEIGHT * Qg + MSSSIM_WEIGHT * Qm`。
- `visual_score >= visual_quality` 的候选达标。
- 多个候选达标时选择输出体积最小者。

所有映射常量集中定义，便于后续校准。

### 搜索策略

`visual_quality` 通过 O(1) 映射到 quality 搜索区间。

- visual quality 越高，`q_min` 越高，搜索区间越窄。
- visual quality 越低，搜索区间越宽。
- `visual_quality=100` 返回 lossless。

搜索顺序：

1. 高视觉质量请求先测试高 quality anchor。
2. 区间粗扫。
3. 边界定位。
4. 在第一个达标 quality 附近二分。
5. 邻域微调。
6. 从达标候选中按最小体积选择。

所有尝试过的 quality 都缓存，避免重复编码。

如果高质量候选也无法达标，则在未启用 fallback 时尽早失败；启用 fallback 时选择最接近目标的候选，并标记未达标。

## 性能设计

### 避免外部进程开销

专门 codec 必须以库链接或库加载方式接入。正式转换路径不能每个文件或每个候选启动外部进程。外部工具只允许用于构建验证或诊断。

### 降低 encoder 初始化成本

- 输入只解码一次为 `ImageBuffer`。
- 每张输入图只准备一次参考指标数据。
- 当 speed、bit depth、chroma、tune 不变时，跨 quality 候选复用 `PreparedFrame`。
- 复用 candidate encode/decode/metrics 的内存 buffer 和 arena。
- 可行时复用 libjxl thread runner。
- SVT-AV1 不跨候选复用 encoder context；复用配置模板、输入帧包装、线程预算和 buffer。

### 降低数据传递成本

- 尽可能用 view/span 传递图像数据。
- 除非目标 encoder 要求，否则避免 RGB/YUV 转换拷贝。
- RGB->YUV、8-bit->10-bit 等转换按输入/settings group 缓存。
- visual quality 搜索优先走内存编码 buffer 和内存候选解码。
- 只把最终选中的候选写到目标输出路径。

### 候选搜索性能

高 visual quality 请求优先在高 quality 窄范围检索，避免浪费时间测试不可能达标的低质量候选。

## 资源规划

新增 `ResourcePlanner`。

输入：

- logical CPU count
- 当前自动 worker budget
- 文件数量
- 估算输入尺寸和位深
- 选择的输出 encoder
- 用户 memory limit 或 auto memory limit

线程规则：

`file_parallelism * encoder_threads_per_file <= global_worker_budget`

默认策略：

- 文件很多时优先增加文件级并行。
- 大批量时 encoder 内部线程保持低值。
- 单文件时给 AV1 更多线程。
- 示例：16 逻辑核心时自动预算可为 12；单文件 AV1 最多可拿 12 encoder threads；大量文件时可降到每文件 1 encoder thread。

内存规则：

- 用户可在 CLI/UI 设置 memory limit。
- auto memory limit = `min(total memory / 2, available memory * 0.8)`。
- planner 根据每个 active task 的估算内存限制文件并行数。
- 内存估算包括 input buffer、prepared frame、candidate output、decoded candidate、metric buffers、encoder working memory estimate。

## UI 设计

UI 显示：

- format
- quality
- visual quality
- speed
- bit depth
- 适用时显示 chroma
- memory limit，默认 auto
- thread/parallelism mode，默认 auto
- strip metadata
- summary/log

UI 不显示 `preset fast|balanced|best|extreme`。

填写 visual quality 后：

- quality 输入禁用或显示为自动控制。
- speed 仍可编辑，因为 speed 决定搜索时固定的 encoder 速度配置。

结果显示：

- decoder backend
- encoder backend
- encoder threads
- memory budget
- final quality
- speed mapping result
- visual score、GMSD、MS-SSIM、Qg、Qm
- output size
- lossless
- fallback state

## CLI 设计

新增或保留：

- `--visual-quality <1-100>`
- `--visual-quality-fallback`
- `--speed <0-10>`
- `--memory-limit <bytes|MiB|GiB|auto>`
- `--threads <n|auto>`，用于文件级 worker budget
- `--avif-tune-iq on|off`，默认 on

尽量保留旧公开参数：

- `--quality`
- `--format`
- `--bit-depth`
- `--chroma`
- `--strip`
- `--summary`
- `--log`
- `--preset`，仅 CLI 兼容

native 后端完成后废弃 Magick 专属运行时参数：

- `--magick`
- 任意 `--define`

## 依赖与硬盘占用清理策略

编解码依赖优先使用 vcpkg：

- libjpeg-turbo
- libpng
- libtiff
- giflib 或等价依赖
- libwebp
- libjxl
- libheif
- OpenJPEG
- libraw，如果可行

SVT-AV1 HDR 是特殊依赖，除非 vcpkg 提供完全匹配变体，否则从 `https://github.com/juliobbv-p/svt-av1-hdr.git` 构建。

native 后端替代 Magick 后：

- 从 CMake 必需依赖中移除 ImageMagick。
- 删除 Magick link libraries 和静态 leaf archive 处理逻辑。
- 删除 `scripts/build-magick.ps1`，或如果未来 Magick 后端可能需要则移到 archive 文档之外，不再作为主流程。
- 删除或替换 `docs/magick-runtime.md`。
- 从 README 移除 MFC/ImageMagick runtime 要求。
- 从活跃工作流文档移除 `third_party/imagemagick-src`、`third_party/imagemagick-runtime`。
- 保留安全的 `.gitignore` 条目，但不再要求用户构建或保存 Magick 产物。

该清理是降低硬盘占用的正式目标，但只能在 native 后端通过构建和测试后执行，避免项目失去可用转换路径。

## C++ 实现风格

native codec 实现应优先使用现代 C++，而不是 C 风格 wrapper 代码。

要求：

- 能用 C++23 modules 的地方继续使用 modules。
- 每个 C-library owned resource 都用 RAII 包装。
- codec context、frame、buffer、metadata object 使用 `std::unique_ptr` + custom deleter 或小型 move-only handle class。
- 可恢复错误使用 `std::expected<T, std::string>`。
- 非拥有 buffer 使用 `std::span`、`std::string_view` 和强类型 view。
- 可取消工作使用 `std::jthread` 和 stop token。
- pixel format、chroma mode、backend ID、speed mapping、lossless mode 使用 strong enum 和小型 value object。
- 优先预分配 buffer 和 move semantics，避免重复分配/拷贝。
- 只在系统边界做校验：CLI/UI 输入、文件解码、codec API 返回值、外部 metadata。
- codec C API 隔离在窄 C++ class 后面，pipeline 其他部分不直接处理 raw pointer。

## 测试

### 单元测试

- visual quality 归一化与评分。
- visual quality 搜索区间映射。
- 多候选达标时按最小体积选择。
- 各 codec 的 speed 映射。
- ResourcePlanner 线程预算公式。
- ResourcePlanner auto memory 计算。
- `quality=100` 与 `visual_quality=100` 的 lossless 决策。
- 参数优先级：visual quality 覆盖 quality，但不覆盖 speed。
- Magick define 迁移错误。

### 集成测试

- 解码代表性输入：JPEG、PNG、TIFF、GIF、WebP、JXL、AVIF/HEIC、启用 OpenJPEG 时的 JP2、WIC fallback case。
- AVIF/WebP/JXL 固定 quality 编码。
- AVIF/WebP/JXL lossless 编码。
- AVIF/WebP/JXL visual quality 搜索编码。
- 验证 summary/log 字段。
- 验证 UI 构建，并显示 speed、visual quality、memory limit、结果诊断信息。

### 性能测试

- 单文件 AVIF 使用自动规划下的高 encoder thread count。
- 大批量降低 encoder threads 并提升文件级并行。
- 大图根据 memory budget 限制并发。
- visual quality 搜索复用参考指标和候选缓存。
- 高 visual quality 从高 quality 区间开始搜索。

## 迁移计划

这是一个统一产品设计，但实现必须拆成小步骤。每一步结束时项目都应保持可构建、可测试。

1. 冻结 Magick-based 实现继续扩张。
2. 新增 native codec 接口和核心数据模型。
3. 新增 ResourcePlanner。
4. 新增独立于 codec 的 visual quality 评分模块。
5. 接入专业解码器和 WIC fallback。
6. 接入 WebP encoder。
7. 接入 JXL encoder。
8. 接入 libheif + SVT-AV1 HDR 的 AVIF encoder。
9. 将 CLI/UI pipeline 切到 `NativeCodecBackend`。
10. 在 native encoders 之上接入 visual quality 搜索。
11. 更新 CLI/UI/summary/log 字段。
12. 运行测试和性能检查。
13. 删除 Magick 依赖、脚本、构建 wiring、文档和运行时产物要求。

## 实现风险

- libheif + SVT-AV1 HDR 的集成细节可能需要 libheif encoder plugin 构建，或直接 SVT encode 后再做 AVIF mux。
- 全量专业解码器覆盖会增加依赖数量和构建时间。
- RAW 与动图行为可能需要分阶段支持。
- metadata 保留在不同 codec/library 中行为不完全一致；`--strip` 简单，完整 metadata round-trip 需要格式特定处理。
- 现有 Magick quality 与 libwebp/libjxl/libheif/SVT quality 无法做到 bit-identical；目标是公开语义兼容，不是字节级兼容。
