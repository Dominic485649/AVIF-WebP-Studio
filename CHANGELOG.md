# 更新日志

## 0.2.1 - 2026-05-17

- 新增 JXL 输出格式，CLI 支持 `--format jxl`，UI 格式下拉支持 JXL。
- JXL 默认质量为 q95，输出扩展名为 `.jxl`，任务列表和日志显示为 `JXL`。
- JXL 使用现有 MagickWand 编码、批处理和 `--optimize` 质量搜索流程。
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
