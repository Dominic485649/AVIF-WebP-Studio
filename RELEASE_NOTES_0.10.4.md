# AWJimage 0.10.4 - 静态运行时、AVIF 位深默认与发布清理

0.10.4 聚焦 Windows/Linux 发行物可移植性、AVIF 默认行为与可审计诊断。它不增加新的编码器入口，也不修改用户显式传入的 quality、speed、chroma 或 bit-depth。

## 运行时与依赖

- Windows Release 强制静态链接 Slint，不再依赖 Slint DLL；Release 继续以单个 `AWJ.exe` 提供 Studio 和 CLI，并保留 `AWJ.com` 命令行入口。
- vcpkg baseline 更新，AOM 升级至 3.14.1，dav1d 升级至 1.5.4，并通过项目内 overlay 固定 AOM、dav1d 和 libyuv 的构建输入。
- Release `BUILD_INFO.txt` 记录 vcpkg baseline、AOM、dav1d、libyuv 与 FetchContent 的实际 commit，便于追溯二进制来源。

## AVIF 默认与诊断

- 未显式指定 bit-depth 的 AVIF 默认至少使用 10-bit；10/12-bit 源图保持对应精度，显式 `--bit-depth 8|10|12` 仍优先于自动选择。
- `--chroma auto` 固定输出 YUV 4:2:0；只有显式 `422` 或 `444` 才改变采样，不再自动继承 422/444 或切换 RGB。奇数 grid 与默认 420 不兼容时明确报错，而不静默切换 444。
- `--alpha auto` 自动保留非不透明 alpha。颜色与 alpha 都随请求 quality 或 visual-quality 结果编码，不再为透明图强制整图 q100/4:4:4；`--alpha off` 仍会移除 alpha。
- 源图 CICP color range 默认保持：PC/full 保持 full，TV/limited 保持 limited；未知 range 使用 full。AVIF q100 仅对未请求改写色彩、alpha、位深或元数据的既有 YUV420 AVIF 原码流直通；其他输入使用 AOM 无损量化并按默认 420 重编码，仍可能发生 RGB/YUV 或色度转换损耗。
- `summary.csv` 新增 AVIF RGB->YUV、AddImage、Finish 和输出复制耗时，便于定位编码阶段，而不是把全部时间归为单一 encode 字段。
- 基准脚本改为验证当前默认 AVIF 参数，并把历史 q80/8-bit 数据与当前协议明确分开；不以单轮性能数据宣称优化。

## Studio 与交付

- 参数页移除右侧问号提示，左侧导航不再给已选项绘制额外边框；Tab、Enter/Space、方向键、Home/End 与可访问角色继续保留。
- `bin/` 仅作为本地构建和打包输出，不再进入版本控制；正式二进制由 GitHub Release 归档提供。

## 验证与归档

- Windows MSVC Release：`ctest --test-dir build/x64/Release -C Release --output-on-failure` 为 31/31 通过；`scripts/benchmark.ps1 -SelfTest` 与 `scripts/cli-worker-smoke.ps1` 通过。后者覆盖真实 CLI manifest 的 ITEM/DETAIL、失败项重试、命名事件取消和 Job Object 强制终止。
- Linux GCC 16.1 Release：`ctest --test-dir build/linux-gcc-x64-release --output-on-failure` 为 16/16 通过。`AWJ` 为 55,614,152 bytes，`readelf -d` 仅列出 Vulkan、fontconfig、libm、libc 与动态加载器，不含 `libstdc++.so.6` 或 `libgcc_s.so.1`。
- Studio 实机检查确认参数页右侧没有帮助问号，导航没有额外外侧焦点框；导航仍以可访问选项卡暴露，并保留键盘导航与选中态。
- GitHub Release 归档继续严格拆分 Linux `AWJ` 和 Windows `AWJ.exe` / `AWJ.com`。归档清单、完整性检查和最终 SHA-256 记录在 Release 正文，而非源码树。
