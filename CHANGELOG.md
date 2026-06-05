# 更新日志

## 0.7.2 - 2026-06-05

0.7.2 修复部分终端中 `AWJ.com --help`、`AWJ --help` 无输出的问题。

- `AWJ.com` 不再只把当前标准输出句柄继承给 GUI subsystem 的 `AWJ.exe`，改为通过 stdout/stderr pipe 捕获子进程输出后写回当前终端。
- 保留 `AWJ.com` 的等待和退出码透传语义，`--help` 返回 0，未知参数返回非 0。
- `AWJ.exe` 仍保持 Windows GUI subsystem，双击启动 Studio 不弹出命令行窗口。

## 0.7.1 - 2026-06-05

0.7.1 修复双击启动 `AWJ.exe` 时短暂弹出命令行窗口的问题，并保留命令行入口的等待、输出和退出码语义。

- 将 `AWJ.exe` 改为 Windows GUI subsystem，双击无参数启动 Studio 时不再创建命令行窗口。
- 新增 `AWJ.com` console shim；命令行输入 `AWJ ...` 时由 shim 转发到同目录 `AWJ.exe`，等待结束并透传 stdout/stderr 与退出码。
- Studio 内部 worker 仍自举 `AWJ.exe`，不依赖 `AWJ.com`，取消、强制终止和 Job Object 隔离语义保持不变。
- 更新构建脚本、CLI smoke 脚本、README、迁移文档和帮助文本，说明 GUI 主程序与 console shim 的分工。

## 0.7.0 - 2026-06-04

0.7.0 是一次以单一 `AWJ.exe` 发布物、Studio 自举 worker 和 CLI/Studio 合体体验为核心的版本更新。重点是取消 Studio 对独立命令行可执行文件的依赖，让 UI、命令行与内部编码 worker 都收敛到同一个可执行文件。

- 合并 CLI 与 Studio 发布目标，Release/Debug 日常构建只生成 `AWJ.exe`，不再生成旧 CLI/Studio 双可执行发布物。
- 新增统一入口：无参数启动 Studio UI，带 CLI 参数时进入 headless 转换路径，`AWJ.exe --help` 显示命令行帮助并保持可靠 stdout/stderr 与退出码。
- Studio 转换改为自举同目录 `AWJ.exe` worker 子进程，继续使用 Job Object 管理生命周期；“取消”通过事件请求协作式停止，“强制终止”和运行中关闭窗口只终止编码 worker，不结束 Studio 本体。
- 保留现有 native codec pipeline、visual_quality GPU 指标路径、summary/log 诊断、AVIF 大图模式和实验编码器能力；UI 与 CLI 共用同一套参数解析与批处理服务。
- 更新脚本、帮助文本、README、迁移文档、manifest/resource 和发布清理规则，面向用户的构建目标、命令示例、输出文件与校验文件统一为 `AWJ`。
- 日志文件名从 `log\awj-cli.log` 调整为 `log\awj.log`，与统一发布物命名保持一致。

## 0.6.2 - 2026-06-04

0.6.2 是一次以 visual_quality GPU 指标链路、Studio 任务隔离和交互修复为核心的小版本更新。

- 将 visual_quality Direct3D 11 shader 改为构建期预编译并内嵌 bytecode，Release 运行时不再依赖 `d3dcompiler` 或 `.cso` sidecar。
- 扩展 Visual Quality Search 的 GPU 诊断，summary 与日志可区分 GPU requested/used/path/fallback reason/fallback count。
- 优化 visual_quality 搜索为预测窗口内 lower-bound 二分，lossy q 限定在 1..99，减少 1 次早停和 8/9 次探测之间的跳变。
- Studio 转换改为启动同目录统一 AWJ worker 子进程，使用 Job Object 管理生命周期；“取消”通过事件请求协作式停止，“强制终止”和运行中关闭窗口只终止编码 worker，不结束 Studio 本体。
- 修复 Studio 窗口拖动：除 Slint 标题区域回调外，增加 Win32 `WM_NCHITTEST` fallback，将左侧标题区域识别为 `HTCAPTION`。
- 修复 Studio 先选择输出路径、再选择输入路径时自动覆盖已有输出路径的问题；已有输出路径会保持用户手动指定的内容。
- 清理旧 AVIF/AOM helper 源码和 Studio 同进程 worker 残留代码，转换路径继续收敛为 native codec 与统一 AWJ 发布目标。

## 0.6.1 - 2026-06-03

0.6.1 是一次以 Visual Quality 默认体验、Studio 任务控制和批处理资源规划为核心的小版本更新。

- 重构默认预设策略，Studio 默认从固定编码质量切换为 visual-quality 观感目标，平衡、快速、急速等预设更清楚地区分质量、体积与耗时取舍。
- 完善 Visual Quality Search，补齐 GPU/CPU 切换、未达标候选兜底、候选复用、JXL RGB 输入缓存和更细的编码/解码/指标耗时诊断。
- 强化取消与强制终止路径，CLI、Studio worker、批处理线程和候选搜索增加更细粒度 stop 检查，Studio 运行中支持强制终止并安全回收后台 worker。
- 优化批处理资源规划，根据图像尺寸、输出格式、visual-quality 状态和内存预算区分普通任务、延后任务、内存超限任务与大图模式任务。
- 改进 AVIF 大图提示，按当前构建能力识别 AOM grid 与实验 zenrav1e，队列、日志和 summary 中输出更明确的尺寸、原因与可用处理方式。
- 扩展 summary.csv 与日志诊断字段，记录 visual-quality 分数、GMSD/MS-SSIM、候选次数、GPU/内存回退、速度参数、编码线程、内存预算和阶段耗时。
- 调整 Studio 参数页与设置页说明，补充质量、视觉质量、内存、速度、WIC 兜底、GPU 加速和实验编码器等选项文案，并改用 HarmonyOS Sans SC 字体资源。
- 更新构建版本到 0.6.1，同步 Debug/Release 脚本构建参数，继续保持 native codec 主线和统一 AWJ 发布目标。

## 0.4.0-rc1 - 2026-05-25 (Pre-release)

- 全面转向 Native 编码核心，彻底清理旧版依赖残留，重组为轻量高效的原生 codec 转换架构。
- 重构色彩空间与元数据透传管道，支持 Primaries、Transfer、Matrix、Range、ICC 及 HDR 元数据全链路透传检测。
- 引入 AlphaModePolicy 策略选项，提供针对 Alpha 通道的安全处理机制（保留/丢弃/智能检测）。
- 完善 Studio 级大图转换队列、AVIF grid/zenrav1e 调度入口，防止 AVIF 无损路径静默降级。
- 优化 Studio UI 表现，根据实际构建能力动态置灰未实现后端，限制事件积压，降低常驻内存。
- 强化 helper 进程、写入安全、Raw 校验以及质量搜索路径的安全边界，极大优化取消处理表现，并补齐回归测试。

## 0.3.1 - 2026-05-21

- 重写 Studio 为 WinUI 3 风格左侧导航布局，拆分编码队列、参数设置、大图模式和设置页。
- 编码队列改为默认入口，输入输出和开始/取消/清空等操作集中到队列页。
- 调整导航、下拉框和页面卡片样式，移除多余说明文字与默认下拉黑色焦点边框。
- 大图模式保持轻量占位，不加载图片、不缓存预览资源，页面切换继续使用条件实例化降低常驻内存。
- 记录项目构建约束：日常构建只构建 AWJ，不构建测试可执行文件。

## 0.3 - 2026-05-21

- 移除内置 ImageMagick/MagickWand 后端、构建脚本与运行时复制逻辑，转换路径收敛为 native codec。
- 保留内置 libavif/AOM、实验 zenrav1e、WebP、JXL；Magick 与 ffmpeg 仅作为未来外部集成方向。
- 将 zenrav1e 隔离到内部 AVIF helper，CLI/Studio 主进程不再直接链接 Rust bridge。
- 新增 `--experimental-encoders` 与 UI 实验编码开关，默认 auto 不选择实验编码器。
- 清理 Release 输出，只保留用户可启动的 CLI/Studio，并将 AVIF helper 放入内部目录。

## 0.2.1 - 2026-05-17

- 新增 JXL 输出格式，CLI 支持 `--format jxl`，UI 格式下拉支持 JXL。
- JXL 默认质量为 q95，输出扩展名为 `.jxl`，任务列表和日志显示为 `JXL`。
- JXL 使用当时的 MagickWand 编码、批处理和质量搜索流程。
- JXL `--speed 0..10` 映射为 ImageMagick `jxl:effort`；用户可通过 `--define jxl:effort=...` 或其他 `jxl:*` define 覆盖。
- JXL 下禁用手动 chroma sampling；位深留空保持原图语义，显式设置时透过 `MagickSetImageDepth`。
- 启动编码前检测 ImageMagick runtime 是否支持 JXL，不支持时提示重新构建带 JPEG XL delegate 的 runtime。
- 更新 ImageMagick 构建脚本、CLI smoke 脚本和文档，说明最小 runtime 构建包含 JXL coder。
- 修复静态 Release CLI 在部分环境首次输出前访问冲突的问题。

## 0.2 - 2026-05-16

- 增强 Release 构建复现性，默认使用自编译 ImageMagick runtime 与静态依赖组合。
- 改进 Studio UI 渲染稳定性、字体与 DPI 表现。
- 改进转换诊断、日志、summary 和错误提示。
- 增强输入路径选择、拖放处理和批处理稳定性。

## 0.1 - 2026-05-16

- 初始 C++23 / Slint / MagickWand 迁移版本。
- 提供 AVIF/WebP CLI 批处理和 Studio 桌面 UI。
