# AVIF native 全编码器与解码能力设计

## 背景

当前 native AVIF 已通过 libavif AOM helper 完成 MVP：`--backend native --format avif --avif-encoder aom` 可以输出 8-bit AVIF，Release CTest 已通过。剩余目标是把 AVIF native 能力从 AOM MVP 扩展为完整的 libavif 后端体系，并修复 Debug JXL 链接/运行时问题。

本阶段采用强依赖全启用路线：优先让 libavif 构建启用 AOM、SVT 相关后端；`zenrav1e` 不通过 libavif 的 rav1e codec choice 实现，而是通过 Rust `zenravif` 静态库集成。明确不拉取、不启用普通 rav1e 后端。如果 SVT 或 zenravif 在当前 Windows static 环境下不可用，则必须查明原因并给出明确构建或运行诊断，而不是静默切换到 rav1e。

## 目标

1. `auto` encoder 只选择当前构建中真实可用的 AVIF encoder。
2. `aom`、`svt` 通过 libavif 抽象接入；`zenrav1e` 通过 Rust `zenravif` 静态库接入；`rav1e` 不再作为可选 encoder。
3. AOM 支持 8/10/12-bit AVIF 输出，chroma 支持 420/444；SVT 保持 420 约束。
4. 新增 native AVIF decode，先支持静态图第一帧。
5. 修复 Debug JXL runtime/library mismatch，使 Debug 配置能可靠验证 JXL 关键路径。
6. 整体完成后统一审查、测试和本地提交；不提前 push。

## 非目标

- 不实现 AVIF 动画序列完整解码或逐帧导出。
- 不把 libavif/SVT/zenravif 直接链接进主 CLI/Studio 进程。
- 不为 zenrav1e 伪造普通 rav1e 能力；zenrav1e 必须来自 Rust `zenravif` 静态库。
- 不拉取或启用普通 rav1e 后端，也不把 rav1e 当作 zenrav1e 的替代品。
- 不改变 ImageMagick 后端的既有行为。

## 依赖与构建

`vcpkg.json` 从 `libavif[aom]` 扩展为尽可能启用 AOM、SVT 后端；不得添加普通 `rav1e` feature 或端口。`zenrav1e` 由 Rust `zenravif` 静态库提供，构建系统需要把 zenravif 编译为可由 C++ helper 链接的静态产物。若 SVT 或 zenravif 在当前 Windows static 环境下不可用，构建阶段要保留明确诊断，并在 registry 中把对应 encoder 标为 unavailable；不得改用 rav1e 代替。

CMake 继续保持 helper 隔离：主 CLI/Studio 链接现有核心库，不直接链接 libavif、SVT 或 zenravif 静态库。libavif 与 zenravif 相关 target 只进入 AVIF helper/codec 目标，避免再次触发 ImageMagick delegate 与 AV1 静态库的进程初始化冲突。

## 架构

现有 `AVIF-WebP-Native-Avif-Aom.exe` 扩展为通用 native AVIF helper，或新增等价 helper。helper 参数包含：

- `--mode encode|decode`
- `--encoder auto|aom|svt|zenrav1e`
- `--input`
- `--output`
- `--quality`
- `--speed`
- `--bit-depth`
- `--chroma`
- `--threads`

主进程负责资源规划、registry 选择、CLI/UI 参数校验和 diagnostics。helper 负责 libavif encode/decode，并用退出码与 stderr 返回失败原因。native backend 调用 helper 后验证输出文件存在且非空。

AVIF codec 层从单一 AOM encoder 演进为多实现 codec 模块：AOM/SVT 共享 libavif RGB/YUV 转换、bit-depth 映射、codecChoice 映射和 decode 到 `ImageBuffer` 的逻辑；zenrav1e 走 zenravif 静态库的 C ABI 或等价桥接层，不复制主进程 pipeline。

## Encoder 选择策略

`auto` 从理论能力选择改为可用能力选择。registry 维护每个 encoder 的状态：available、unavailable、experimental、license、支持 chroma、支持 bit-depth、默认 speed 映射。

自动选择优先级：

1. 若请求 444 或 10/12-bit，优先 AOM。
2. 若请求 420 且 SVT 可用，可优先 SVT，尤其是大图场景。
3. zenrav1e 只有在 zenravif 静态库构建可用并通过 smoke test 后才进入 auto 候选；普通 rav1e 永不进入候选。
4. 若候选不可用，auto 跳过；显式选择不可用 encoder 时返回清晰错误。

SVT 继续强制 420：UI 中 chroma 置灰并固定 420，CLI 中非 420 直接报错。

## 10/12-bit 输出

AOM 支持 8/10/12-bit 输出。若输入解码器只提供 8-bit RGBA，libavif 输入侧使用 16-bit RGB container 表达 10/12-bit 目标深度，并按目标 bit-depth 做尺度扩展。若未来 decoder 原生提供高 bit-depth，直接保留高位深路径。

422 暂不作为强制目标；如 libavif 后端可用且转换路径稳定，可保留 registry 能力，否则显式报错。420/444 是本阶段必须验证的 chroma。

## AVIF decode

新增 libavif decode 路径，使用 `avifDecoder` 解析输入，并用 `avifImageYUVToRGB` 输出到内部 `ImageBuffer`。本阶段只读取静态图第一帧。decode 输出至少支持 RGBA 8-bit；若源图为 10/12-bit，可输出对应高 bit-depth buffer，或在 pipeline 暂不支持时给出明确错误。

AVIF decode 加入 native decoder registry，使 native pipeline 能处理 `.avif` 输入并转出 WebP/JXL/AVIF。

## Debug JXL

Debug JXL 问题按构建一致性处理。优先让 Debug preset 使用匹配的 runtime/library 组合；如果 vcpkg/第三方 JXL 只可靠提供 Release static，则 CMake 必须显式阻止不可靠 Debug 链接，或调整依赖来源，使 Debug JXL 关键测试可运行。

验收目标是 Debug 配置可 configure/build，并至少运行 JXL codec 关键测试；Release 全量测试仍是最终交付门槛。

## 测试计划

- registry tests：覆盖 auto 只选可用 encoder、显式不可用 encoder 报错、SVT 420 约束、AOM 10/12-bit 能力、rav1e 被拒绝。
- codec tests：覆盖 AOM 8/10/12-bit encode、420/444 encode、AVIF decode 第一帧。
- helper tests：覆盖 CLI 调 helper 的 encode/decode 成功路径和失败诊断。
- native backend tests：覆盖 AVIF 输入和 AVIF 输出端到端。
- UI/config tests：覆盖 encoder 选择、SVT 色度锁定、CLI 非法组合报错。
- Debug JXL validation：运行 Debug 配置下 JXL 关键测试。
- Release validation：运行 Release 全量 CTest。

## 风险与处理

- vcpkg/libavif feature 名称、SVT 后端或 zenravif 静态库可用性可能与预期不同：先探测 feature 和 Rust 构建入口，再改 manifest/CMake；zenravif 不可用时记录为构建诊断并在运行时清晰报错，不改拉 rav1e。
- AV1 静态库与 ImageMagick delegate 可能再次冲突：坚持 helper 隔离，主进程不直接链接 libavif/SVT/zenravif。
- 高 bit-depth 输入管线可能需要扩大 `ImageBuffer` 表达能力：优先局部扩展像素格式和 stride 校验，避免重写全 pipeline。
- Debug JXL 可能受第三方预编译库限制：不绕过问题；要么修复 runtime 匹配，要么用 CMake 明确隔离不可用配置并给出说明。

## 完成标准

- `--backend native --format avif --avif-encoder auto` 能选择真实可用 encoder 并成功输出。
- 显式 `aom` 支持 8/10/12-bit 的验证样例。
- 显式 `svt`、`zenrav1e` 要么真实可用并通过测试，要么因当前 libavif/vcpkg/zenravif 构建限制给出明确诊断；`rav1e` 明确不可用。
- native AVIF decode 可解静态第一帧。
- Debug JXL 关键验证完成。
- Release 全量 CTest 通过。
- 最终统一审查并创建本地 git commit，不执行远端 push。
