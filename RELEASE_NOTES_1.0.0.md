# AWJimage 1.0.0

AWJimage 1.0.0 是首个正式版。这个版本把 Windows 与 Linux 的 native 转换路径收敛到同一条主线，同时完成 Studio 窄窗口可用性、异常留痕和发行构建的最后整理。

## 这次发布带来什么

- Studio 参数页的标签、输入框和下拉框按行垂直居中；右侧彩色结论与详细说明在窄窗口自动换行，彩色文字只保留适度间距，设置页长说明不再强迫横向滚动。
- Studio 支持中文与 English 即时切换，翻译随 Slint 资源编入可执行文件，不需要 gettext、`.mo` 或额外运行时文件。
- AVIF、WebP、JXL、JPGLI、PNG 均使用内置 native codec。AVIF `--chroma auto` 按源图 420/422/444 规则处理，RGB/RGBA、灰度和未知源保持明确的输出策略；大图自动链默认使用 zenrav1e，失败后回退 grid。
- Windows 与 Linux Release 均启用现代编译优化；Linux 发行 ELF 静态包含 libstdc++ 与 libgcc，GPU visual metrics 使用 Vulkan，不能使用时自动回退 CPU。
- Windows 异常和线程边界增加可诊断的崩溃日志；监控异常会终止对应子进程，无效 Win32 句柄、异常对象生命周期和若干模块依赖声明已完成修正。

## 依赖与构建

发行构建使用 vcpkg baseline `c1d80d9cb071c3f4a98c67c1196b137cc5b72918`。直接依赖包括 AOM 3.14.1、dav1d 1.5.4、libavif 1.4.2、Slint 1.17.1、libjpeg-turbo 3.2.0、libjxl 0.11.2、libyuv 1951、libwebp 1.6.0、libpng 1.6.58、LibRaw 0.22.1、TIFF 4.7.2 与 zenravif 0.1.3；Jpegli 与 svt-av1-hdr 使用当前上游固定提交，并修正后者胶片颗粒色度缩放点的上游重复赋值。依赖版本以构建时实际解析结果为准，没有追逐未发布的开发版本。

## 发行归档

归档严格按平台拆分：Linux 包只有 `AWJ`，Windows 包只有 `AWJ.exe` 与 `AWJ.com`。两份归档均使用 7-Zip LZMA2 最高压缩级别、单线程和关闭 BCJ2（`-t7z -m0=lzma2 -mx=9 -mmt=1 -mf=off`），并在交付前通过 `7z t`。

| 归档 | 内容 | 大小 | SHA-256 |
|---|---|---:|---|
| `AWJ_Linux.7z` | Linux x86-64 ELF：`AWJ` | 16,329,034 bytes | `150e61ae0e992059f4e1363ea052aa799ef909d30782611fd5503171410d8971` |
| `AWJ_Win.7z` | Windows x64：`AWJ.exe`、`AWJ.com` | 11,878,512 bytes | `00dbac85ebd14858aa3d8ea1586abac82dc985a6b16ada8f` |

## 二进制 SHA-256

```text
a82da56767c2c7b4c340cc719fe4084ffaad84c32636bf1560f770f21e916411  AWJ
515be84b41a651e83f8815a090953d9473176b76b07a4735b95a3ac94205c062  AWJ.exe
90eaf27f19fc774f900bbb829aa1b060651459e4025f7a7b12dacc411e845d0e  AWJ.com
```

## 已知边界

Linux 运行时仍需要系统 Vulkan loader 和 fontconfig；Windows 的 WIC、JXR、`AWJ.com` 及资源管理器右键集成不属于 Linux 包。GPU 指标不可用或收益不足时会透明回退 CPU。
