# AWJimage 0.10.2 - 0.10 系列累计更新：Linux、超大图与转码稳定性

0.10.2 是 AWJimage 0.10 系列的当前稳定版本。本说明汇总从 0.9.1 之后进入 0.10.0、0.10.1 和 0.10.2 的全部主要更新，包括 Linux/Vulkan 主线、AVIF 超大图处理、多帧首帧转换、Windows 右键队列，以及本次转码卡顿、线程调度和透明 AVIF 无损修复。

0.10.2 没有修改 quality、speed 的默认值、映射或用户传值。速度恢复来自进程管理、worker 调度和并发规划修复。

### 0.10.2：转码卡顿、线程调度与可靠性

- **修复 Windows 转码卡顿与线程规划**
  - 修复 Windows 进程 ID 获取的无限递归，解决 Studio 和右键转码在编码后看似卡住、速度异常缓慢的问题。
  - Windows 编码 worker 恢复普通调度优先级，避免系统存在其他负载时被长期饿住。
  - 自动线程预算保持原有规则：硬件线程数 >=12 时预留 4 线程，5..11 时预留 2 线程，2..4 时预留 1 线程，单线程仍使用 1 线程。
  - 编码器线程数与 CPU 文件并发上限之积严格等于总预算，内存限制继续独立约束实际并发。
  - 总批次超过 12 张时，普通、延后和大图阶段均按每张图片 1 个编码线程执行。
  - 1000 万像素以上且仍在 AOM 普通单图范围内的图片放到队列尾部，避免单张大图降低整个小图队列的并发。

- **修正透明 AVIF 与 AV1 解码链路**
  - 需要保留透明通道时固定使用 AOM，颜色与 alpha 均按 q100、4:4:4 整图无损编码。
  - 用户设置的 `speed` 保持不变；`alpha=off` 继续使用请求的 quality 与 speed 进行普通有损编码。
  - `zenrav1e` 不再接收无法保证透明无损的输入，避免静默产生不符合预期的结果。
  - AVIF/AV1 解码优先使用 dav1d；dav1d 不可用或解码失败时自动回退 AOM。

- **恢复可靠的取消与强制终止**
  - Studio 主队列恢复使用独立 worker 进程执行编码。
  - “强制终止”会立即结束活动进程树，普通取消仍使用合作式取消。
  - 修复 Windows `TerminateProcess` 兜底已经成功却仍报告失败的问题。

- **增强右键队列、日志与结果记录**
  - 右键多选请求在 Slint 初始化前完成合并，使用初始收集窗口与消息静默窗口，并在关闭通道前锁定发送端、最终排空队列，避免末尾图片漏转。
  - `summary.csv` 新增 `awj_version`，明确记录生成结果所使用的 AWJ 版本。
  - 合法旧日志继续追加；发现无效 UTF-8、格式错误或截断内容时清空重建。
  - 日志写入增加跨进程锁，避免多个右键转码进程互相覆盖。
  - 队列 manifest 改为版本化二进制格式，并补充路径、文件大小、记录数量和输出覆盖校验。
  - 单个损坏或无法探测尺寸的输入只记录为该项失败，不再中止其余 AVIF 批处理任务。

- **改进 Studio 与右键窗口**
  - 主界面与右键窗口统一使用固定顶部表头和连续列表。
  - 项目开始后立即显示“正在转码”，完成、失败和运行中的行不再因不可拖动而整体变灰。
  - 右键窗口在高 DPI 下会连同标题栏和边框约束到当前工作区，避免底部操作按钮移出屏幕。

- **更新依赖版本**
  - 更新 vcpkg baseline、TIFF 4.7.2、Slint 1.17.1 与 Rust 传递依赖。
  - 固定 libavif 1.4.2、AOM 3.13.3、dav1d 1.5.3 及当前编码器构建 pin。

### 0.10.1：AVIF grid、多帧首帧与右键队列

- **修复 AVIF grid 非整数倍尺寸**
  - 右列和底行使用图片实际剩余尺寸，输出保持原始宽高，不再因 grid 对齐改变尺寸。
  - 奇数尺寸自动使用 4:4:4；显式指定不兼容的 4:2:0 或 4:2:2 时会明确报错。

- **统一多帧图片转换行为**
  - WebP、GIF、APNG、JXL、TIFF、AVIF sequence、Windows WIC 多帧和 JPEG MPF 输入统一转换合成后的第一帧。
  - 不保留其余帧；无法可靠提取第一帧时直接报错，避免生成不完整或语义错误的输出。

- **改进 Windows 右键转换队列**
  - 右键菜单移除视觉质量选项，菜单转换只保留固定编码质量预设。
  - 精简右键转换窗口，文件名居中显示，所在目录以较小字号显示在下方，并移除冗余状态列。
  - Windows Explorer 对多选图片或文件夹分别发起的启动请求会合并到同一个窗口和转换队列，不再保留多个独立 AWJ 进程。
  - 右键转换窗口新增“强制终止”；关闭 Studio 或右键窗口时会先终止活动任务，再关闭界面。

- **增强 Studio 字体设置**
  - 新增系统已安装字体下拉选择，首项可以恢复系统默认字体。
  - Windows 将所选字体写入 `AWJ.jsonc`，下次启动自动恢复。

- **继续对齐 Linux 能力**
  - Linux GCC Release 启用静态 `zenrav1e`。
  - Nautilus 与 Thunar 右键菜单对齐 AVIF、WebP、JXL、JPGLI、PNG 五种格式及其质量、位深、速度、尺寸限制和格式专属参数。
  - WIC、JXR 等 Windows 专属功能继续保持平台隔离。

### 0.10.0：Linux/Vulkan 主线与超大图自动链路

- **合并 Linux GCC/Vulkan 到主线**
  - 新增 `linux-gcc-x64-debug` 与 `linux-gcc-x64-release` 构建预设。
  - Linux 使用单个 ELF `AWJ` 同时提供 Slint UI 与 CLI，不生成 Windows `AWJ.com` shim。
  - Linux Release 默认使用 GCC 16 side-by-side、`-O3`、IPO/LTO 和 `-march=x86-64-v3`，并静态链接 `libstdc++` 与 `libgcc`，降低跨发行版运行时依赖。

- **新增 Linux Vulkan visual_quality 指标路径**
  - Linux 的 visual-quality GPU 指标使用 Vulkan compute；Windows 继续使用 Direct3D 11 compute。
  - Vulkan 初始化失败、图片过小或资源不足时自动回退 CPU，日志记录 `vulkan-session` 或 `cpu-fallback`。
  - GPU 只加速 luma、GMSD、MS-SSIM 和 downsample 指标分析，候选编码、解码与最终结果选择仍使用 native CPU pipeline。

- **明确 Windows 与 Linux 平台边界**
  - Windows 与 Linux 共用 core、pipeline 和 codecs，平台差异只保留 Win32 shell、WIC/JXR、D3D11、`AWJ.com` 与 Linux Vulkan/POSIX 入口。
  - Linux 不启用也不展示 WIC 兜底；JXR、Windows 注册表 shell 和 `AWJ.com` 仍仅限 Windows。
  - Linux 提供用户级 Nautilus Scripts 与 Thunar UCA 右键入口，不要求 sudo。

- **落地 AVIF 超大图自动处理链路**
  - 普通单图尽量保留在原编码器路径：AOM 上限为单边 65536 且总像素不超过 `2^30`，SVT-AV1-HDR 上限为 16384×8704。
  - 超过 AOM 单图上限后默认优先使用实验 `zenrav1e`，失败或不支持时回退 `grid`；参数页可以改为 `grid` 优先。
  - 两条路径都不可用、都失败，或触达输入与运行时内存上限时明确报错；手动强制路径不会静默改路。
  - 0.10.0 当时在 Studio 大图页展示状态，并允许对单项强制指定 `zenrav1e` 或 `grid`；截至 0.10.2，自动大图状态已并入主队列，不再保留独立大图页。

- **增加大图资源控制与诊断**
  - 新增仅当前会话有效的“解除 20 GiB 输入/运行时上限”，UI 与 CLI `--unlock-max-input-file-bytes` / `--unlock-20gib-limit` 均可使用，默认关闭且不写入 `AWJ.jsonc`。
  - grid 遇到不可整除且需要 padding 的输入时提供明确诊断，并提示使用可整除尺寸、experimental clamped padding 或边长不超过 65536 的 `zenrav1e`。
  - 参数页编码设置继续只在本次运行内有效，不写入 `AWJ.jsonc`；主题、模板和菜单参数等既有持久化项保持不变。

- **同步文档与 Linux 发行物**
  - 同步中英文 README 与迁移说明，记录 Windows/Linux 共用核心、平台差异和大图处理策略。
  - 发行目录新增 Linux Release ELF `AWJ`。

### SHA256

```text
59ca8df6f953012b4ffd434a5f1b97938c6a2d1c267da7b3bc9ac237744a0e67  AWJ.exe
0d6e5782e2e51f6cdd5cf60d9d3c768569c9b3021ea8cd3dbb975a60a4d4d6af  AWJ.com
63c900da441af16f3fd846e433d27f5a02e0cccac7851249f4b08a02f85209c8  AWJ
```
