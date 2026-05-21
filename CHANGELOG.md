# 更新日志

## 0.3.1 - 2026-05-21

- 重写 Studio 为 WinUI 3 风格左侧导航布局，拆分编码队列、参数设置、大图模式和设置页。
- 编码队列改为默认入口，输入输出和开始/取消/清空等操作集中到队列页。
- 调整导航、下拉框和页面卡片样式，移除多余说明文字与默认下拉黑色焦点边框。
- 大图模式保持轻量占位，不加载图片、不缓存预览资源，页面切换继续使用条件实例化降低常驻内存。
- 记录项目构建约束：日常构建只构建 CLI 和 Studio，不构建测试可执行文件。

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
