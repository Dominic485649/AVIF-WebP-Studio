# visual_quality GPU-first 性能管线设计

## 背景

AWJimage 当前已经实现 Direct3D 11 compute shader 视觉质量指标路径：`awj.visual_metrics_gpu` 负责 luma、GMSD、MS-SSIM 和 downsample 等 GPU 计算，`awj.native_visual_search` 在 `visual_quality` 搜索中创建 `AcceleratedVisualMetricSession` 并复用 reference 图状态。默认配置启用 `visual_quality_gpu`，CLI 和 Studio 都提供开关。

源码审计显示，当前实现更准确地说是 **visual_quality 指标 GPU 加速**，而不是端到端 GPU 转码。候选编码、候选解码、质量搜索决策和最终 encoded bytes 返回仍在 CPU memory pipeline 中完成。后续工作应把这条链路整理为 GPU-first、诊断透明、可继续批量优化的性能管线，而不是误称为完整转码全链路 GPU。

## 目标

1. 确认当前 GPU 覆盖范围、fallback 条件和实际诊断输出。
2. 保留 Direct3D 11 compute shader 路线，并把 visual_quality 指标路径升级为 GPU-first。
3. 构建期预编译 shader 并内嵌 bytecode，避免最终用户运行时编译 shader。
4. 优化整体链路性能，包括候选编码/解码的内存传递、当前仍存在的外部 codec/辅助进程边界、线程池调度、内存预申请与复用、CPU↔GPU 传输和 GPU readback。
5. 让 GPU 请求、实际使用、fallback 次数、fallback 原因和各阶段耗时在 summary/log/文档中可见。
6. 删除无效分支、无效代码和误导性文案，并补充必要中文注释。
7. 更新 README、迁移说明和相关构建/验证说明。

## 非目标

- 不把第三方 native codec 的 encode/decode 强行改成 GPU codec。
- 不恢复 Magick/ffmpeg 内部后端；Magick 仍只能作为未来外部 exe/runtime 集成方向。
- 不把所有图片都强制走 GPU；小图、D3D 不可用、资源超限、shader/device 失败时允许 CPU fallback，但必须可见、可解释。
- 不做与 visual_quality、GPU、性能链路、分支清理和文档无关的大规模重构。
- 日常构建不构建测试可执行文件，只构建 `AWJ` 目标。

## 当前链路判断

当前 visual_quality 搜索链路为：

```text
输入 ImageBuffer（CPU memory）
  ↓
CPU encoder 编码候选图
  ↓
CPU decoder 从 encoded bytes 解码候选图
  ↓
候选 ImageBuffer 上传到 GPU
  ↓
GPU 生成 luma、计算 GMSD 和 MS-SSIM partials
  ↓
partial/result 回读 CPU
  ↓
CPU 合成 VisualMetricResult 并选择最终候选
  ↓
encoded bytes 与 diagnostics 返回内存
```

现有 GPU 路径已经覆盖 luma、GMSD、MS-SSIM 和 MS-SSIM downsample。`AcceleratedVisualMetricSession` 可复用 reference luma，并具备缓存 reference MS-SSIM levels 的结构基础。当前小图阈值策略是合理的性能保护：session 低于约 100 万像素时走 CPU，one-shot 低于约 200 万像素时走 CPU。

当前不足主要在：运行时 shader 编译仍可能发生；候选评估仍逐个执行；candidate 上传与 partial readback 仍有优化空间；GPU fallback 原因需要更透明；文档和 UI/CLI 文案需要避免“全链路 GPU”的误解。

## 架构设计

### `awj.visual_metrics_gpu`

继续作为 GPU 指标核心模块，职责包括：

- Direct3D 11 device/context 初始化。
- shader bytecode 加载与 compute shader 创建。
- reusable structured/readback/constant buffer 管理。
- reference/candidate luma 生成。
- GMSD 计算。
- MS-SSIM downsample levels 与 partial 计算。
- GPU session 生命周期与错误上报。

改进方向：

- 使用构建期预编译并内嵌的 shader bytecode。
- Release 不保留 runtime compile 兜底；构建环境缺少 `fxc.exe` 时在 CMake 配置阶段明确失败，避免最终用户运行时承担 HLSL 编译成本。
- 明确小图 CPU 阈值的原因。
- 返回 GPU fallback 原因，而不是只依赖计数。
- 复用 candidate buffer、partial buffer 和 readback buffer。
- 评估是否可增加 batch candidate 或更深 GPU reduction 接口；若复杂度过高，作为后续可选优化保留。

### `awj.native_visual_search`

继续作为 visual_quality 搜索调度模块，职责包括：

- quality range 搜索。
- 候选编码、解码和评估。
- GPU session 或 CPU fallback 调用。
- timing/fallback 诊断累计。
- 最小达标候选或最接近候选选择。

改进方向：

- 把 GPU 诊断结构化为 requested/used/fallback_count/fallback_reason/path_summary。
- 将 GPU fallback 与“质量未达标时输出最接近候选”的 visual_quality fallback 区分开。
- 为候选记录和 evaluated quality 预留容量，减少扩容。
- 检查 `q_min`/`q_max`、neighbor candidate 等可并行或可批处理的候选，但不破坏二分搜索的顺序依赖。
- 审计当前 AVIF candidate 路径：libavif/AOM 为 in-process，zenrav1e 通过静态 Rust bridge 调用；已删除未参与构建的旧 `AWJ-native-avif-helper` 源码和主进程中的过时 helper 调度代码。SVT 相关 helper/path 诊断属于独立集成路径，不能与旧 AVIF helper 混同。

### `awj.visual_metrics`

继续作为 CPU fallback 和结果一致性基线。

- 不删除 CPU 路径。
- 不改变 metric 公式。
- 只补充必要注释，说明 CPU 路径用于小图、兼容性和 GPU fallback。

### diagnostics、CLI、Studio 和文档

诊断输出应能回答：

- 用户是否请求 GPU。
- 实际是否使用 GPU。
- fallback 次数和原因。
- luma、GMSD、MS-SSIM、encode、decode、candidate IO、GPU fallback 和当前仍存在的外部 codec/辅助进程边界耗时。
- 哪些环节仍是 CPU pipeline。

CLI help、Studio 文案、README 和迁移说明要统一：GPU 加速的是 visual_quality 的指标分析路径，不代表 codec encode/decode 全部 GPU。

## Shader 预编译策略

采用构建期预编译、内嵌 bytecode、运行时直接创建 compute shader 的策略。

```text
HLSL source
  → CMake 调用 Windows SDK fxc.exe 编译为 Direct3D 11 cs_5_0 bytecode
  → 生成 C++ header 内嵌数组
  → 链入 AWJ
  → 运行时 CreateComputeShader(embedded bytecode)
```

设计原则：

- 提交 `.hlsl` 源文件。
- 不提交 `.cso` 二进制，避免源码和 bytecode 不一致。
- 不向 Release 输出添加 `.cso` sidecar。
- 最终用户不需要 Windows SDK 或 `fxc.exe`。
- 当前实现不保留 `D3DCompile + cache` 运行时兜底；如果 shader compiler 缺失，Release 配置直接给出 CMake 诊断。

体积预期：4 个短 compute shader 的 bytecode 与嵌入包装通常只增加几十 KB 到一两百 KB，远低于可感知发布体积成本。预编译主要优化首次 GPU visual_quality 初始化延迟和运行时稳定性，不直接替代候选 dispatch、上传或 readback 优化。

## 整体链路性能设计

### 管道传输

- 对当前 active path 优先使用 encoded bytes in memory 和 `decode_memory`；旧 AVIF helper 管道已移除，避免为每个 visual_quality candidate 引入不必要磁盘 IO 或子进程传输。
- 对仍保留外部边界的 codec/辅助进程路径使用大 buffer 批量传输，避免小块频繁读写。
- 已知输出大小时预分配接收 buffer。
- 使用 `std::span<const std::byte>` 传递 encoded bytes，避免复制。
- 复用 candidate decoded image 到 GPU 上传所需的中间 buffer。

### 进程边界

- 当前 AVIF AOM/libavif 路径为 in-process，实验 `zenrav1e` 为静态 Rust bridge；visual_quality candidate 搜索不再启动旧 `AWJ-native-avif-helper`。
- 已删除未参与 CMake 构建的旧 AVIF helper 源文件和主进程中过时的 helper process 调度代码。
- 对可 in-process 的 encoder 保持 in-process 优先。
- SVT 相关 helper/path 诊断属于独立 codec 集成边界；若未来存在必须隔离的 helper，再评估持久 session、批量 candidate 编码或进程复用。
- 不为了性能牺牲实验 encoder 的隔离边界；若协议改动风险过大，先记录测量结论和下一阶段方案。

### 线程池与调度

- 批处理层继续多图并行。
- 单图 visual_quality 内部尊重二分搜索顺序依赖。
- 可并行或可提前准备的候选包括初始 `q_min`/`q_max` 和 neighbor candidate。
- 避免多个大图同时争用同一个 D3D context mutex 导致总体变慢。
- 评估有限 GPU metric queue 或每 worker GPU session。
- 让 CPU encode/decode 与 GPU metric 尽量形成 pipeline：CPU 准备下一个候选，GPU 处理当前候选。
- 线程数以吞吐和 UI 响应为目标，不以最大并发为目标。

### 内存预申请与复用

重点复用或预申请：

- encoded bytes buffer。
- decode_memory 输出 buffer。
- luma float buffer。
- GPU input words。
- structured buffers。
- readback buffers。
- MS-SSIM level buffers。
- partial buffers。
- candidate records 和 evaluated quality vectors。

现有 reusable buffer 结构是基础，应确保 session 按最大候选尺寸复用。候选数量可按搜索上限估算并 `reserve`。decoded candidate image 在指标计算完成后尽早释放，避免大图搜索时峰值内存过高。

### 降低 GPU 传输开销

- 每个 candidate pixels 只上传一次。
- candidate luma、GMSD 和 MS-SSIM 复用同一 GPU luma buffer。
- 不把 candidate luma 回读 CPU，除非 GPU metric 失败需要 CPU fallback。
- reference luma 和 reference MS-SSIM levels 长驻 session。
- 评估将 GMSD/MS-SSIM partial readback 合并，或进一步在 GPU 上 reduction 到少量 scalar。
- batch candidate 只在候选已知且内存压力可控时引入，不强行破坏二分搜索逻辑。

## Fallback 与错误处理

GPU fallback 条件包括：

- 用户关闭 GPU。
- 小图低于 GPU 阈值。
- Direct3D device/context 创建失败。
- shader bytecode 创建失败。
- buffer/view 创建失败。
- resource size 超限。
- `Map`/readback 失败。
- 输入格式、位深、stride 不符合 GPU path 要求。
- GPU path 中途执行失败。

行为规则：

- GPU 未请求时直接 CPU，不记为 GPU failure。
- GPU 请求但小图走 CPU，要记录为阈值 fallback 或非错误 CPU path。
- GPU 初始化失败时，整张图后续候选直接 CPU，并记录原因。
- 单候选资源失败时，可只让当前候选 CPU fallback，也可按失败类型禁用本图 GPU。
- `visual_quality_fallback=false` 只表示未达质量目标时失败，不影响 GPU→CPU fallback。
- 继续使用 `std::expected<T, std::string>` 表达错误。
- `std::stop_token` 在编码前后、解码后、GPU 上传前后、metric 阶段之间和最终候选选择前都应尽早响应。

## 分支与无效代码清理

实现阶段按安全顺序处理分支：

1. 确认工作区干净。
2. 列出本地/远端分支和 worktree。
3. 对每个分支判断是否同提交、是否为 `master` 祖先、是否有未合并提交、是否绑定 locked worktree。
4. 与 `master` 同提交的普通分支视为已合并。
5. `worktree-agent-*` 若是 `master` 祖先且 worktree 无未提交变更、无活跃会话，可清理 worktree 和分支；若仍 locked 或无法确认安全，不强删，记录原因。
6. 有未合并提交的有效分支合并到 `master`。
7. 不推送远端删除，除非用户另行明确要求。

无效代码清理只聚焦 visual_quality/GPU/性能链路和文档一致性：删除误导文案、重复 fallback 逻辑、不可达旧路径；补充解释 GPU/CPU 边界、fallback、resource 生命周期和性能取舍的中文注释。

## 文档更新

计划更新：

- `README.md`：增加 visual_quality GPU 说明，强调默认启用、允许 CPU fallback、构建只构建 CLI 与 Studio。
- `docs/cpp-port.md`：更新当前流程、模块划分、GPU-first metric path、非 GPU 环节和诊断说明。
- CLI help 与 Studio 文案：避免把 metric GPU 加速写成 codec 全链路 GPU。
- 如 summary/log 列发生变化，同步说明字段含义。

## 验证计划

遵守项目规则：每次构建只构建 AWJ 目标，不构建测试可执行文件。

### 静态验证

- 检查 GPU path 调用是否完整。
- 检查 shader 预编译生成和内嵌 bytecode 优先路径。
- 检查 config/help/UI/README/docs 说法一致。
- 检查 diagnostics 是否从 GPU module 传到 encode result、summary/log。

### 构建验证

仅运行：

```powershell
cmake --build --preset windows-msvc-x64-release --target AWJ
```

不执行全量构建，不构建测试可执行文件。

### 手工 CLI 验证

- 使用已有样例或轻量临时图片运行 visual_quality。
- 对比默认 GPU 与 `--no-visual-quality-gpu` CPU path。
- 检查 summary/log 中 GPU/fallback/timing 是否可见。
- 不使用 brainstorming URL/可视化辅助功能。

## 验收标准

- 有效分支已合并；无效临时分支被安全清理或记录无法清理原因。
- shader 构建期预编译并内嵌，Release 不要求最终用户运行时编译 shader，也不新增 `.cso` sidecar。
- visual_quality GPU 覆盖情况在源码、CLI/Studio 文案和文档中一致。
- GPU metric path 明确覆盖 luma、GMSD、MS-SSIM；codec encode/decode 和最终候选选择不被误称为 GPU。
- GPU fallback 原因、次数和耗时可诊断。
- 明显重复分配、重复 shader 编译、不必要 IO、过时 helper 调度代码等性能问题得到修复，或在风险过高时记录为下一阶段。
- AWJ 目标构建通过。
- 不构建测试可执行文件。
