# AWJimage 0.10.3 - 可复现基准、Studio 可访问性与队列诊断

0.10.3 聚焦可测量性、界面交互、稳定性和跨平台工程质量。它没有新增编码器入口，也没有修改 quality、speed、色度、位深、透明 AVIF 或 AV1 解码语义。

## 编码与线程不变量

- 硬件线程数 >=12 时总预算减 4，5..11 时减 2，2..4 时减 1，单线程保持 1。
- 编码器线程数乘文件并发数保持总 CPU 预算；批次超过 12 张时每张固定 1 个编码线程。
- 需要保留透明通道的 AVIF 继续走 AOM q100/4:4:4 整图无损，用户 speed 不变。
- dav1d 继续作为 AVIF/AV1 首选解码器，不可用或失败时自动回退 AOM。
- 本版未修改 AOM 配置、构建 pin 或用户传入/默认参数映射。

## Studio 与 UI/UX

- SoftComboBox、SoftButton、导航、帮助提示和队列右键菜单新增完整 Tab 焦点、可见焦点环、Enter/Space、方向键、Home/End、Esc 与可访问元数据。
- 字体下拉框最多显示 10 行，支持滚轮和滚动条；不提供搜索或手动输入。
- 参数页按常用参数、资源限制、格式高级选项分组。危险警告常驻，其余说明通过帮助提示查看。
- 导航使用克制的青色选中态，主要操作保持蓝色；队列减少重复边框并给列表留出更多空间。
- 队列显示待处理、处理中、成功、失败计数，支持仅看失败、重试失败项和选择详情。
- 详情面板保留完整错误、输入/输出路径、编码器、线程数和 decode/prepare/encode/write 耗时。

## CLI worker 与验证

- Studio manifest worker 新增 `@AWJ-STUDIO/1 DETAIL` 行，按项传回编码器、线程数和四阶段微秒值。
- `scripts/cli-worker-smoke.ps1` 不启动 UI：它直接运行真实 `AWJ.exe` CLI，验证成功项、已知空 WebP 失败项、普通取消和 Job Object 强制终止。
- 普通取消通过命名事件返回 130；强制终止使用 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 并确认记录的进程全部退出。
- 小型 Slint component smoke 使用 testing backend，无窗口覆盖页面切换、字体滚动、键盘、深浅色、820×560 与 100%/150%/200% scale。

## Canonical CLI 基准

固定协议：Windows MSVC Release、平衡电源 `381b4222-f694-41f0-9685-ff5bb260df2e`、输入清单 SHA-256 `4CDFC310837192A7BDAC149C1C212ECD8F720AEE6EECB954D218D66D1E424628`、AVIF/AOM 3.13.3、q80、speed6、4:2:0、8-bit、alpha auto、自动 12 线程预算。每组预热 1 次、测量 5 次，P95 使用 nearest-rank。

| 613 张 CLI | 0.10.2 `ec61551` | 0.10.3 `5732b2b` |
|---|---:|---:|
| 墙钟中位数 / P95 | 214.361 / 226.493 s | 146.235 / 215.123 s |
| 进程 CPU 中位数 / P95 | 1613.906 / 1624.469 s | 1332.266 / 1392.453 s |
| Item seconds sum 中位数 / P95 | 2259.181 / 2406.916 s | 1543.863 / 2298.253 s |
| 峰值内存中位数 / P95 | 13.1 / 13.1 MiB | 12.9 / 12.9 MiB |
| 吞吐量中位数 | 2.860 img/s | 4.192 img/s |
| decode / prepare / encode / write 中位数 | 51.107 / 0.346 / 2189.732 / 15.396 s | 35.994 / 0.242 / 1499.948 / 7.606 s |
| encode 阶段占比 | 97.0% | 97.0% |

两版均为 612 成功、1 个已知空 WebP 失败。0.10.3 五轮墙钟为 139.402、143.287、146.235、169.468、215.123 s；P95 仍接近旧版，小批次峰值内存也出现上升。由于 CLI 编码路径未变，这些更快中位数只记录为本机观测，不能证明 UI 改动带来编码加速。严格回归未复现，因此没有运行 ETW/perf，也没有调整 libaom 参数。

## 构建与测试

- Windows MSVC Release：31/31 CTest 通过。
- Linux GCC 16.1 Release：16/16 CTest 通过。
- Linux ELF 53.1 MiB；`readelf -d` 不含 `libstdc++.so.6` 或 `libgcc_s.so.1`，构建包含 `-O3`、LTO、`-march=x86-64-v3`、`-static-libstdc++` 和 `-static-libgcc`。
- CLI worker smoke：ITEM/DETAIL、失败 manifest、取消 130、强制终止 130 与无残留进程全部通过。

## 风险说明

- 613 张墙钟离散度仍高，平衡电源方案不能锁定 CPU 频率或排除后台调度影响；不应把单轮最快值用于版本结论。
- 小批次峰值内存在部分场景高于 0.10.2；本版不宣称内存优化。
- UI smoke 是无窗口组件测试，不替代屏幕阅读器兼容性认证；它验证可访问树、键盘路径和最小布局几何。
- `DETAIL` 是同版本 Studio 与其自举 CLI worker 的内部协议；不支持跨版本混用可执行文件。

## SHA-256

```text
6f238c8bf03f9a95602cdf4022f81fa3fb1275362b71ac99ea98a1351ab7326f  AWJ.exe
63e67b5db140555e3477acc2bf0d4d7e885d8565658fe2abf3280ef42dd69999  AWJ.com
64a4659bd065fc81cec4331e03490de9ef9925f2f5abe5bb1347c17bfd4026f5  AWJ
```
