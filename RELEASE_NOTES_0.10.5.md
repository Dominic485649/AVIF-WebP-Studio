# AWJimage 0.10.5 - Studio 参数说明、AVIF 自动色度与大图简化

0.10.5 聚焦 Studio 参数可读性与自动策略的一致性：界面说明直接反映实际资源预算，AVIF 的 `auto` 色度在编码器选择前按源格式解析，主页面只保留日常转换需要的控制项。

## Studio 参数

- 主参数页移除“预设”和“大图优先”。质量、视觉质量、速度、资源和格式高级选项仍可独立设置；右键菜单的专用预设保持不变。
- 参数控件右侧恢复彩色说明：蓝色对应格式与色度，橙色对应编码器、位深和 alpha，绿色提示速度，青色说明资源预算，红色常驻提示 visual_quality 的重复编码成本。
- 自动线程严格按逻辑线程数预留桌面余量：>=12 预留 4，5-11 预留 2，2-4 预留 1，单线程系统仍使用 1。
- 自动内存上限为总内存 50% 与当前可用内存 80% 的较小值；系统只能报告其中一项时使用该项，不沿用不准确的固定比例描述。

## AVIF 自动色度

- `--chroma auto` 现在保留 YUV 源的 420/422/444；RGB/RGBA 统一转为 YUV444，灰度或未知格式回退 YUV420，输出仍是 YUV，不会输出 RGB。
- 解析后的 auto chroma 会在 AVIF 编码器选择之前传入。422/444 源图因此不会误选只支持 420 的 SVT 路径；显式选择 SVT 时，420-only 的限制和明确错误保持不变。
- AOM 直接编码路径采用相同的源格式映射。`--alpha auto` 继续保留非不透明 alpha，颜色与 alpha 都使用请求的质量，不再因透明通道强制整图无损。

## 依赖更新

- vcpkg baseline 刷新到当前 registry，所有 manifest 及传递依赖会从该基线重新解析。
- libyuv 更新至 1951，libavif 更新至 v1.4.2，静态 SVT AV1 HDR 源更新至最新上游提交；AOM 3.14.1、dav1d 1.5.4、Jpegli 当前提交与 Slint 1.17.1 已核对为当前上游版本。

## 大图策略

- Studio 的自动大图链固定为 `zenrav1e` 优先、失败或不支持后回退 `grid`，不再暴露会被意外保留的页面偏好。
- CLI 仍保留 `--large-image-priority zenrav1e|grid`，供批处理和手动 worker 明确指定优先路径。

## GitHub 发行归档

GitHub Release 标签为 [`0.10.5`](https://github.com/Dominic485649/AWJimage/releases/tag/0.10.5)，对应提交 `d9704f96018abe19c0e8d238d44506cc7d99e0ff`。归档按平台严格拆分，不携带未指定的附属文件：

| 归档 | 精确内容 | 大小 | SHA-256 |
|---|---|---:|---|
| [AWJ_Linux.7z](https://github.com/Dominic485649/AWJimage/releases/download/0.10.5/AWJ_Linux.7z) | `AWJ`（Linux ELF） | 16,289,284 bytes | `55cbdf0369adaa633ac32b977f187292a570ab90ad6738ee4ac0888bdd3de96e` |
| [AWJ_Win.7z](https://github.com/Dominic485649/AWJimage/releases/download/0.10.5/AWJ_Win.7z) | `AWJ.exe`、`AWJ.com`（Windows） | 11,713,010 bytes | `fca40d6be0b569fd6d9a8813b88ad615822e00ba03113c55e803e988d0980228` |

两份文件均使用 7-Zip `-t7z -m0=lzma2 -mx=9 -mmt=1 -mf=off` 打包。Linux 包只包含 `AWJ`，Windows 包只包含 `AWJ.exe` 和 `AWJ.com`。

## 二进制 SHA-256

```text
dfc8aec58beb0eb679a13da96d036a02cb2baa66f46a58b8672720c2a7299681  AWJ.exe
4ec633f6d908543ca6f98cd2da5a5a05d1afd22d81dab6a156962d8745d36acc  AWJ.com
7b3370ba66d27acdd15c36f44168d3f10e44f2c4f68768abbe9d9b291b0bf99a  AWJ
```
