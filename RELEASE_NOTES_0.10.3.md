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

早期 1367.171 与 1660.888 核秒记录使用不同色度/协议，不能直接比较，也不作为本表基线。

| 613 张 CLI | 0.10.2 `ec61551` | 0.10.3 `2798db2` |
|---|---:|---:|
| 墙钟中位数 / P95 | 214.361 / 226.493 s | 162.660 / 166.395 s |
| 进程 CPU 中位数 / P95 | 1613.906 / 1624.469 s | 1381.844 / 1399.391 s |
| Item seconds sum 中位数 / P95 | 2259.181 / 2406.916 s | 1719.619 / 1769.620 s |
| 峰值内存中位数 / P95 | 无效（旧采样缓存） | 2807.7 / 2822.4 MiB |
| 吞吐量中位数 | 2.860 img/s | 3.769 img/s |
| decode / prepare / encode / write 中位数 | 51.107 / 0.346 / 2189.732 / 15.396 s | 40.749 / 0.314 / 1668.552 / 10.198 s |
| encode 阶段占比 | 97.0% | 97.0% |

两版均为 612 成功、1 个已知空 WebP 失败。0.10.3 五轮墙钟为 158.326、166.395、162.366、162.660、163.967 s。旧 0.10.2 峰值内存值来自未刷新 `Process` 缓存的首次采样，不能用于版本比较；0.10.3 修复后 1/4/12/13 张及透明/不透明矩阵的峰值工作集中位数为 104.7..897.0 MiB。由于 CLI 编码路径未变，这些更快中位数只记录为本机观测，不能证明 UI 改动带来编码加速。严格回归未复现，因此没有运行 ETW/perf，也没有调整 libaom 参数。

## 构建与测试

- Windows MSVC Release：31/31 CTest 通过。
- Linux GCC 16.1 Release：16/16 CTest 通过。
- Linux ELF 53.1 MiB；`readelf -d` 不含 `libstdc++.so.6` 或 `libgcc_s.so.1`，构建包含 `-O3`、LTO、`-march=x86-64-v3`、`-static-libstdc++` 和 `-static-libgcc`。
- CLI worker smoke：ITEM/DETAIL、失败 manifest、取消 130、强制终止 130 与无残留进程全部通过。

## 风险说明

- 613 张本轮墙钟范围为 158.326..166.395 s；平衡电源方案仍不能锁定 CPU 频率或排除后台调度影响，不应把单轮最快值用于版本结论。
- 旧版峰值内存采样无效，不能进行前后内存比较；0.10.3 的 613 张进程峰值工作集中位数为 2807.7 MiB，本版不宣称内存优化。
- UI smoke 是无窗口组件测试，不替代屏幕阅读器兼容性认证；它验证可访问树、键盘路径和最小布局几何。
- `DETAIL` 是同版本 Studio 与其自举 CLI worker 的内部协议；不支持跨版本混用可执行文件。

## GitHub 发行归档

GitHub Release 标签为 [`0.10.3`](https://github.com/Dominic485649/AWJimage/releases/tag/0.10.3)。归档按平台严格拆分，不携带未指定的附属文件：

| 归档 | 精确内容 | 大小 | SHA-256 |
|---|---|---:|---|
| [AWJ_Linux.7z](https://github.com/Dominic485649/AWJimage/releases/download/0.10.3/AWJ_Linux.7z) | `AWJ`（Linux ELF） | 16,338,056 bytes | `d7efc2f4ece5fdf3876cad480fa74b7848d00deeda4398bc26f11cdc7b69377c` |
| [AWJ_Win.7z](https://github.com/Dominic485649/AWJimage/releases/download/0.10.3/AWJ_Win.7z) | `AWJ.exe`、`AWJ.com`（Windows） | 11,717,484 bytes | `108883cf75185255b68b390b7c2c5f9567811b8e180b66a12b394cd7d5243fae` |

两份文件均使用 7-Zip `-t7z -m0=lzma2 -mx=9 -mmt=1 -mf=off` 打包：LZMA2、最高压缩级别、单压缩线程，并关闭 `.exe` 的自动 BCJ2 过滤以保持 LZMA2 方法。上传前 `7z t` 已通过；Linux 包只列出 `AWJ`，Windows 包只列出 `AWJ.exe` 和 `AWJ.com`。

## SHA-256

```text
6218ded9b880c8551b08d4ed2bcae6b8858e773ed32cae5d484fce32b491a393  AWJ.exe
63e67b5db140555e3477acc2bf0d4d7e885d8565658fe2abf3280ef42dd69999  AWJ.com
64a4659bd065fc81cec4331e03490de9ef9925f2f5abe5bb1347c17bfd4026f5  AWJ
```
