# 更新日志

## 0.10.1 - 2026-07-11

- 修复 AVIF grid 的非整数倍尺寸：右列和底行使用实际剩余尺寸，输出保持原始宽高；奇数尺寸自动回退 4:4:4，显式不兼容的 4:2:0/4:2:2 会明确报错。
- 动图和多图容器统一只转换合成后的第一帧，包括 WebP、GIF、APNG、JXL、TIFF、AVIF sequence、WIC 多帧和 JPEG MPF；不能可靠提取时直接报错，不保留其余帧。
- 右键菜单移除视觉质量，仅保留固定编码质量预设；精简右键转换窗口，文件名居中、目录以较小字号显示在下方，并合并冗余状态列。
- Windows Explorer 对多选图片/文件夹分别发起的右键进程会在启动期合并到一个窗口和同一转换队列，不再为每个选中项保留独立 AWJ 进程。
- 右键转换窗口新增“强制终止”；本体与右键窗口点击右上角关闭时会先立即终止全部活动任务，再关闭界面。
- Studio 新增已安装字体下拉选择，首项可恢复系统默认；Windows 将 `ui_font_family` 写入 `AWJ.jsonc`。
- Linux GCC Release 启用静态 `zenrav1e`，Nautilus/Thunar 对齐五种格式及其质量、位深、速度、尺寸限制和格式专属菜单参数；WIC/JXR 等 Windows 专属功能仍不提供。

## 0.10.0 - 2026-07-10

0.10.0 是一次把 Linux/Vulkan 首版并入主线、统一 Windows/Linux 共用 core，并落地超大图自动处理链路的版本。

- 合并 Linux GCC/Vulkan 构建到主线：`linux-gcc-x64-debug` / `linux-gcc-x64-release` 预设可用；Linux 只生成单个 ELF `AWJ`，不生成 `AWJ.com`。
- Linux Release 默认使用 GCC 16 side-by-side、`-O3`、IPO/LTO、`-march=x86-64-v3`，并静态链接 `libstdc++` / `libgcc`，降低跨发行版运行时依赖。
- Linux visual_quality GPU 指标路径改为 Vulkan compute；失败、小图或资源不足时自动回退 CPU，日志记录 `vulkan-session` / `cpu-fallback`。
- Linux 不启用、不展示 WIC 兜底；JXR/WIC/`AWJ.com`/Windows 注册表 shell 仍仅限 Windows。Linux 提供用户级 Nautilus Scripts 与 Thunar UCA 右键入口。
- 超过 AOM 单图上限后自动走大图链路：默认 `zenrav1e` 优先并回退 `grid`，参数页可改为 `grid` 优先；两条路径都失败或触达内存/输入上限时明确报错。
- Studio 大图页保留状态展示，并可对单项强制指定 `zenrav1e` / `grid`；手动强制路径不会静默改路。
- 新增会话内“解除 20 GiB 输入/运行时上限”（UI 设置页 / `--unlock-max-input-file-bytes`），默认关闭、红字警告 OOM 风险，且不写入 `AWJ.jsonc`。
- 参数设置页编码参数保持“本次运行内有效、不写入 jsonc”的策略；主题/模板/菜单参数等既有持久化项不变。
- 补强 grid 失败诊断：不可整除且需要 padding 时，明确提示当前版本未启用安全裁切，并建议改用可整除尺寸、experimental clamped padding 或 `zenrav1e`（边长 <= 65536）。
- Windows 与 Linux 共用 core/pipeline/codecs；平台差异仅保留 Win32 shell、WIC/JXR、D3D11、`AWJ.com` 与 Linux Vulkan/POSIX 入口。
- 同步中英文 README / 迁移文档，并提交 Linux Release `AWJ` 产物。

## 0.9.1 - 2026-07-06

0.9.1 是一次以 Windows 右键菜单转换体验、菜单参数持久化、图片边长限制和 HDR 色彩修正为核心的小版本更新。

- 右键菜单转换改为由 `AWJ.exe` 启动轻量 Slint 队列窗口，多选图片会进入同一个队列，不再为每个文件打开独立窗口或命令行窗口。
- 右键转换窗口适配深色模式和高 DPI，队列条目使用圆角卡片，进度条从左到右显示，长文件名自动省略，完成后可按菜单参数自动关闭。
- 右键菜单参数迁移到左侧“菜单参数”页，支持安装右键菜单、移除右键菜单、保存参数，并在注册表菜单与当前版本/参数不一致时提示移除旧菜单。
- 右键菜单预设独立于 Studio 参数页持久化保存，AVIF、WebP、JXL、JPGLI、PNG 均可分别配置菜单参数；PNG 仍只影响右键菜单/CLI 输出，不重新加入 Studio 输出格式下拉。
- 新增图片边长限制：支持自动、无限制和手动模式；手动模式可同时设置最大宽、高、长边和短边，并按最严格限制缩放。
- 新增 `suffix-number` 输出重名策略，按 `name(1)`、`name(2)` 递增，避免重复编码时出现 `name(1)(1)` 一类文件名。
- 修复右键单文件 WebP 输入识别/解码路径，优先使用 native WebP 解码，允许 WIC 作为兜底解码器。
- 改进 JXR/WIC scRGB HDR 解码到 BT.2020 + PQ 16-bit 的色彩路径，但 WebP、PNG、JPGLI 的 HDR 输出仍为异常/不可靠场景。
- 重要提醒：如果源图是 HDR，请不要选择 WebP、PNG 或 JPGLI 作为目标格式；HDR 源图建议优先使用 AVIF 或 JXL。
- 修复窗口底部下拉框向下展开空间不足时显示不全的问题，仅在空间不足时改为向上展开。
- AVIF 大图模式阈值改为真实单图编码上限：AOM/libaom 允许 65536 边和 `2^30` 像素内继续普通编码，`svt-av1-hdr` 标记为 16384×8704 上限；1000 万像素以上但未超限的图片只延后到普通队列尾部。
- Studio 主页队列改用新版 Slint `DragArea`/`DropArea`，未开始项目可直接拖动排序；同时修复“下移”排序插回原位的问题。
- 保留普通 AVIF 的单任务多线程编码路径，大图阈值调整不会把 1000 万像素级图片强制切到 grid/大图 worker。

## 0.9.0 - 2026-07-04

0.9.0 是一次以编码器 preset/speed 语义修正、Studio 输出格式行为调整和 JXL JPEG 无损转封装为核心的版本更新。

- 修正默认质量策略：AVIF 默认 q70，JXL 默认 q85，WebP 默认 speed4，AVIF/JXL 默认 speed6。
- 调整 JXL speed 到 effort 的映射为 `speed0 => effort10`、`speed6 => effort4`、`speed10 => effort1`，覆盖 libjxl 当前 effort 1..10 范围。
- 明确 AOM/AVIF 默认路径：不设置不存在的 `--good`，保留 libavif/AOM 自身 still image / all-intra / speed 逻辑；AVIF auto 文案同步为默认 AOM。
- 显式选择 SVT-AV1-HDR 编码 AVIF 时，对 lossless/q100、需要保留 alpha、显式 444/422 和 >10-bit 等不支持场景直接报错，避免隐式降级。
- JXL 对 JPEG 输入默认优先使用 JPEG bitstream 无损转封装；遇到 strip metadata 或色彩/HDR 元数据冲突时回退普通 JXL 有损编码。
- Studio 输出格式下拉移除 PNG，PNG 保留 CLI 与右键菜单支持；Studio 的 speed 输入仅对 AVIF/WebP/JXL 显示和传入。
- JPEGli 关闭伪 speed 支持：Studio 不展示 speed，CLI 显式传入 `--speed` 时直接报错。
- 修复 Studio 切换输出格式时质量值跨格式污染的问题：各格式在本次运行内独立记忆质量，重启后恢复内置默认值。

## 0.8.5 - 2026-06-18

0.8.5 是一次以 AVIF/JXR 位深兼容、默认质量策略调整和深层编码性能优化为核心的版本更新。

- 修复 JXR 等 16-bit 源图输出 AVIF 时直接失败的问题；继承源图位深超过当前 AVIF 编码器上限时，现在限制到编码器支持的最高输出位深，用户显式请求不支持位深仍会明确报错。
- 调整默认质量：AVIF 默认 q70，JXL 默认 q85，WebP 与 JPGLI 默认值保持不变。
- 优化 AVIF 10/12-bit 编码准备路径，取消 AWJ 侧 8-bit 到高位深 RGB 临时扩展，交由 libavif 官方 RGB/YUV depth rescale 处理。
- 并行化 AVIF Grid tile 准备，并通过可重复 CMake patch 为保守 still color grid 场景启用 libavif tile 编码并行。
- 改进超大图队列调度，在内存预算、线程预算和输出路径冲突约束下允许不同输出路径并行处理。
- 细化编码线程资源规划，按 AOM/zenrav1e、SVT-AV1-HDR、JXL 和其他编码器分别设置默认线程上限。
- 更新 FetchContent 依赖到当前可用上游版本，并在 Release BUILD_INFO 中记录对应 pin。

## 0.8.2 - 2026-06-09

0.8.2 是一次以 Studio 队列行为修复、窗口状态记忆和默认常量统一管理为核心的小版本更新。

- 修复 Studio 队列中已编码图片不能删除的问题；清空队列现在会清理全部普通队列和大图队列条目。
- 增强未清理队列时的再次编码行为：已完成、已跳过或正在锁定输出的条目不会被重新编码，只有待编码、失败和已取消的条目会进入下一次运行。
- Studio `AWJ.jsonc` 新增 `window_width` / `window_height`，关闭窗口时会刷新当前窗口大小，下次启动按上次尺寸恢复。
- 将编码默认值、metadata 上限、JXL/WebP 缓冲区、输出临时文件重试和 visual_quality 评分常量集中迁移到 `src/core/encoding_defaults.ixx`；Studio UI 运行时常量独立放入 `src/ui/studio_defaults.ixx`。
- 参数页仅居中“大图处理”下拉框文本，其他下拉框保持原有对齐。
- Studio 字体仍优先使用系统已安装的鸿蒙黑体 / HarmonyOS Sans SC；建议用户安装鸿蒙黑体以获得最佳中文界面显示。
- 更新版本到 0.8.2，并保持 Release 输出为 `AWJ.exe`、`AWJ.com` 及对应 `.sha256` 校验文件。

## 0.8.1 - 2026-06-09

0.8.1 是一次以多路径性能优化、Studio 运行时配置持久化和系统字体回退为核心的小版本更新。

- 优化 native 后端文件复制缓冲、WebP/JXL 编码输出缓冲预估，以及 visual_quality 搜索候选解码路径，减少重复 metadata 拷贝和候选指标开销。
- Studio 新增同目录 `AWJ.jsonc` 运行时配置；启动时先使用程序内默认值，再由 `AWJ.jsonc` 覆盖，用户修改过的设置会自动写入对应配置项。
- Studio 不再嵌入字体文件，默认优先使用系统已安装的鸿蒙黑体 / HarmonyOS Sans SC，未安装时回退微软雅黑等系统字体；建议用户安装鸿蒙黑体以获得最佳界面显示。
- 更新版本到 0.8.1，并保持 Release 输出为 `AWJ.exe`、`AWJ.com` 及对应 `.sha256` 校验文件。

## 0.8.0 - 2026-06-09

0.8.0 是一次以 JPGLI 高级编码选项、BMP/JXR native codec、Visual Quality GPU 指标优化和四文件发布清理为核心的版本更新。

- JPGLI 新增 progressive level、Huffman 优化和 XYB 相关配置；CLI、Studio、config 校验、summary 和回归测试同步支持。
- 新增 BMP 与 JXR native codec 注册、WIC 解码路径和对应 codec 测试，扩展输入格式覆盖范围。
- 优化 Visual Quality GPU 指标链路，改进 downsample、luma、GMSD 与 MS-SSIM shader 的计算稳定性和诊断输出。
- 改进 Studio 交互和任务状态展示，补充格式、质量、视觉质量、取消和内存限制相关提示。
- Release 构建清理规则收敛为 `AWJ.exe`、`AWJ.com` 及两个 `.sha256` 文件；测试可执行文件改输出到独立 tests 目录，避免污染发布目录。
- 清理旧设计文档残留，并补充本轮严格审查记录。

## 0.7.3 - 2026-06-07

0.7.3 新增 JPGLI/Jpegli native codec 路径，并同步 Studio/CLI 入口、诊断和文档。

- 新增 `AWJ_ENABLE_JPEGLI` CMake 选项，默认拉取并静态链接 google/jpegli 的 `jpegli-static`。
- CLI 新增 `--format jpgli` / `--format jpegli`，Studio 格式下拉新增 `JPGLI`；输出扩展名默认保持 `.jpg`，但 UI、命令、summary 和日志均显示 `JPGLI` / `jpegli`。
- 新增 `JpegliImageEncoder` 与 `JpegliImageDecoder`，支持 encode/decode、`decode_memory`、质量与速度映射，以及 ICC/EXIF/XMP metadata 保留/剥离。
- JPEG 兼容输入优先尝试 Jpegli decoder，失败后回退 libjpeg-turbo/WIC。
- `summary.csv` 新增显式 `encoder_id` 列，并保留旧 `encoder_selected` 列；JPGLI 输出记录 `format=JPGLI`、`encoder_id=jpegli`。
- 改进 Studio JPGLI 相关默认质量、固定 8-bit 位深、拖拽队列提示和任务列表格式显示。
- 补充 JPGLI codec、config、decoder registry、native pipeline、资源规划和 summary 安全回归测试。

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
