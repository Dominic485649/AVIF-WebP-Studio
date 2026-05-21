# AOM AVIF native encoder MVP 设计

## 背景

Phase 2 已完成 AVIF encoder registry/capability/diagnostics 骨架，CLI/UI 已支持 `auto | svt | aom | zenrav1e | rav1e`，SVT 的 420 限制已在 UI 和 CLI 层体现。Phase 3 的目标是把 registry 选中的 AOM 接到真实 native AVIF 输出路径，让 `--backend native --format avif --avif-encoder aom` 能产出标准 `.avif` 文件。

## 决策

所有 native AVIF encoder 都统一通过 libavif 接入。本阶段只实现 AOM；后续 SVT、rav1e、zenrav1e 也继续走 libavif 的 codec choice/encoder abstraction，不直接在项目里手写 AVIF container 或分别维护多套封装逻辑。

## 范围

本阶段包含：

- 在 vcpkg/CMake 中引入 `libavif` 并启用 AOM encoder backend。
- 新增 native AVIF AOM encoder 模块，实现现有 `ImageEncoder` 接口。
- native backend 在 AVIF 输出时继续先调用 registry；当 applied encoder 是 AOM 时调用真实 encoder，否则保留清晰的未实现/禁用错误。
- 支持 8-bit RGBA 输入。
- 支持 chroma `420`、`444`，`auto` 使用 registry 的 applied chroma。
- 保留 diagnostics：requested/applied encoder、chroma、bit-depth、license、fallback reason、speed mapping。
- 增加 codec 级和 native backend/CLI 路径测试。

本阶段不包含：

- SVT、rav1e、zenrav1e 的真实编码实现。
- 10/12-bit AVIF 输出。
- AVIF decode。
- 视觉质量搜索的 AVIF 专项优化。
- Debug JXL runtime mismatch 修复。

## 架构

新增 `src/codecs/avif_aom_codec.ixx`，导出 `AvifAomImageEncoder`。该 encoder 依赖 `avif.codec`、`avif.avif_registry` 和 libavif C API，只负责把项目内部 `ImageBuffer` 编码为 AVIF bytes。

新增 `AVIF-WebP-Native-Avif-Aom.exe` helper 进程承载 libavif/AOM 链接。主 CLI/Studio 进程继续保留 ImageMagick 静态 runtime，不直接链接 libavif/AOM，避免与 bundled Magick delegate 在同一进程内产生静态库初始化冲突。后续 SVT、rav1e、zenrav1e 仍按同一 libavif 抽象接入；如果继续存在进程级依赖冲突，也沿用 helper 隔离模式。

`src/codecs/avif_registry.ixx` 保持 registry/capability/diagnostics 职责，不承担真实编码。

`src/backends/native_backend.ixx` 的 AVIF 分支改为：

1. 解码输入为 `ImageBuffer`。
2. 调用 `select_avif_encoder()` 得到 selection 和 diagnostics。
3. 如果 selection 是 AOM，调用独立 helper 进程执行 `AvifAomImageEncoder` 编码。
4. helper 写出 `.avif` 文件，native backend 校验文件存在且非空，并把 diagnostics 写入 result metadata。
5. 如果 selection 是非 AOM，返回明确错误，说明该 encoder 已选择但当前实现尚不可用。

## 数据流

```text
WebP/JXL input
  -> native decoder in main process
  -> AVIF registry selection
  -> AVIF-WebP-Native-Avif-Aom helper
  -> ImageBuffer RGBA 8-bit in helper
  -> AvifAomImageEncoder
  -> libavif image + AOM codec
  -> .avif output file
  -> native backend validates output and records diagnostics
```

## 行为规则

- 显式 `--avif-encoder aom` 不 fallback；如果参数不支持，返回错误。
- `--avif-encoder auto` 可以沿用 registry fallback 规则；当前实现阶段只有 AOM 能真实输出，因此如果 auto applied 到 AOM 就编码，否则返回未实现提示。
- `--chroma auto` 按 registry applied chroma 写入 libavif。
- `--chroma 420` 映射为 `AVIF_PIXEL_FORMAT_YUV420`。
- `--chroma 444` 映射为 `AVIF_PIXEL_FORMAT_YUV444`。
- MVP 输入只接受 `PixelFormat::rgba`、8-bit、单平面 RGBA；其他输入返回清晰错误。
- bit depth 未指定时使用 8-bit；指定非 8-bit 时本阶段报错。
- speed 使用现有 AOM diagnostics mapping：`aom:cpu-used`。

## 测试

新增或扩展测试：

- codec 级：构造 2x2 RGBA `ImageBuffer`，用 AOM encoder 输出 AVIF bytes，断言非空且 diagnostics 为 AOM。
- native backend 级：用 WebP fixture 作为输入，`BackendKind::native + OutputFormat::avif + AvifEncoderMode::aom` 输出 `.avif`，断言文件存在且非空。
- CLI/CTest：覆盖 `--backend native --format avif --avif-encoder aom` 的成功路径；保留 `svt + 444` 和 disabled experimental encoder 的拒绝测试。
- Release CTest 全套通过。

## 风险与缓解

- libavif CMake target 和 vcpkg feature 名称可能随版本不同而变化；实现时优先使用 vcpkg 官方 CMake package target，必要时用 `find_path/find_library` 兜底。
- 静态链接可能引入额外 transitive libraries；libavif/AOM 放在独立 helper 进程中，主 CLI/Studio 不直接链接 libavif/AOM，避免重新耦合 ImageMagick 静态 delegate。
- `native_pipeline_core` 之前受 pipeline/Magick 静态组合影响发生启动期崩溃；本阶段 native AVIF 测试保持直接覆盖 native backend，避免把 Magick runtime 拉进最小 native codec 测试。
