# AWJimage 项目深度改名设计

## 背景

当前项目已经从旧目录迁移到 `D:\Code\Cpp\FFimage`，远端仓库为 `Dominic485649/AVIF-WebP-Studio`。用户希望把远端仓库名称改为 `AWJimage`，本地文件夹名称也改为 `AWJimage`，并把用户入口程序改名为 `AWJ-cli` 和 `AWJ-studio`。

本设计覆盖仓库、目录、CMake target、可执行文件、UI 标题、CLI help、脚本、测试和文档的命名变更。目标是完成品牌层深度改名，同时避免把 AVIF 图像格式概念错误替换为 AWJ。

## 已确认决策

- 兼容策略：彻底改名，不保留旧 `AVIF-WebP-*` 可执行文件或 target 作为兼容别名。
- 改名深度：深度改名。产品/项目层 CMake target、源码通用前缀、UI/help/脚本/测试/文档都迁移到 AWJ 命名。
- 语义边界：AVIF 作为图像格式名保留。`--avif-encoder`、AVIF registry、AVIF codec、AVIF decode/encode 诊断和“AVIF 编码器”等格式语义不机械改成 AWJ。
- 远端仓库：由 Claude 执行 GitHub 仓库重命名。
- 本地目录：先把 `D:\Code\Cpp\FFimage` 改为 `D:\Code\Cpp\AWJimage`，再继续代码改名。

## 推荐方案

采用分层深度改名：

1. 产品层统一为 `AWJimage` / `AWJ-cli` / `AWJ-studio`。
2. CMake 根工程、产品 target、helper target、manifest、UI 标题、CLI help、脚本、测试和当前说明文档同步改名。
3. 通用项目层 target 和源码符号从 `avif_*` 迁移到 `awj_*`，例如 base、pipeline、core、backend 等。
4. 格式层保留 `avif`，包括 AVIF codec、AVIF registry、AVIF 参数和 AVIF 用户文案。

不采用全量机械替换，因为这会混淆 AWJ 品牌和 AVIF 格式语义，增加构建与测试风险。

## 本地目录与远端仓库流程

实施顺序如下：

1. 确认当前仓库干净，`main` 与 `origin/main` 对齐，且没有未提交变更。
2. 将本地目录从 `D:\Code\Cpp\FFimage` 改为 `D:\Code\Cpp\AWJimage`。
3. 切换当前工作上下文到新目录后继续实施。
4. 将 GitHub 仓库从 `Dominic485649/AVIF-WebP-Studio` 重命名为 `Dominic485649/AWJimage`。
5. 将本地 `origin` 从 `git@github.com:Dominic485649/AVIF-WebP-Studio.git` 更新为 `git@github.com:Dominic485649/AWJimage.git`。
6. 通过 `git remote -v`、`git fetch origin` 和 `git status --short --branch` 验证远端和本地状态。

本地目录名和 GitHub 仓库名不是 Git 内容，不单独产生代码提交。

## CMake 与构建产物

根工程从 `AVIFWebPStudio` 改为 `AWJimage`。工程描述和构建日志中的产品名从 `AVIF-WebP-Studio` 改为 `AWJimage`。

用户入口 target 和可执行文件改名：

- `AVIF-WebP-Cli` → `AWJ-cli`
- `AVIF-WebP-Studio` → `AWJ-studio`

最终输出：

- `AWJ-cli.exe`
- `AWJ-studio.exe`

native AVIF helper 从旧 MVP 名称改为通用 helper 名称：

- `AVIF-WebP-Native-Avif-Aom` → `AWJ-native-avif-helper`
- `AVIF-WebP-Native-Avif-Aom.exe` → `AWJ-native-avif-helper.exe`

所有依赖、runtime 复制、CTest 命令、脚本路径和源码中 helper 查找路径同步更新。

Studio manifest 文件改名：

- `src/ui/AVIF-WebP-Studio.manifest` → `src/ui/AWJ-studio.manifest`

manifest 内部 assembly identity 改为 `AWJ-studio`。

## 源码命名边界

通用项目层命名迁移到 AWJ：

- `avif_base` → `awj_base`
- `avif_pipeline` → `awj_pipeline`
- `avif_core` → `awj_core`
- `avif_magick_backend` → `awj_magick_backend`
- `avif_native_backend` → `awj_native_backend`
- 通用测试 target 可改为 `awj_*`

格式层 AVIF 命名保留：

- `src/codecs/avif_aom_codec.ixx`
- `src/codecs/avif_registry.ixx`
- `avif_avif_aom_codec_tests` 或可改为更清晰的 `awj_avif_aom_codec_tests`
- `avif_avif_registry_tests` 或可改为更清晰的 `awj_avif_registry_tests`
- `--avif-encoder`
- “AVIF encoder” / “AVIF 编码器”
- AVIF decode/encode 诊断中的格式名

环境变量 `AVIF_MAGICK` 属于既有用户配置接口。新接口改为 `AWJ_MAGICK`，运行时优先读取 `AWJ_MAGICK`；若不存在则兼容读取 `AVIF_MAGICK`。文档标记 `AVIF_MAGICK` 为 legacy，避免立即破坏已有本机配置。

## UI 与 CLI 文案

UI 窗口 title 和主标题从 `AVIF-WebP Studio` 改为 `AWJ Studio`。格式下拉中的 `AVIF`、`WebP`、`JXL` 保持不变，因为它们是输出格式名。“AVIF 编码器”保持不变，因为它是 AVIF 格式配置。

CLI help banner 从：

```text
AVIF-WebP-Studio C++23
AVIF-WebP-Cli.exe [选项]
```

改为：

```text
AWJimage C++23
AWJ-cli.exe [选项]
```

示例命令同步改为 `AWJ-cli.exe`。格式参数、AVIF/JXL/WebP 说明和 AVIF encoder 参数保持原语义。

## 脚本、测试与文档

需要更新的脚本和测试包括：

- `debug.ps1`
- `release.ps1`
- `scripts/test-cli-pixpin.ps1`
- CTest 中引用 CLI target、helper target、Studio target 或 help banner 的地方
- `tests/cli_native_avif_aom_tests.cpp` 中查找 CLI exe 的名字
- `src/backends/native_backend.ixx` 中查找 helper exe 的名字

README、CHANGELOG、`docs/cpp-port.md`、`docs/magick-runtime.md` 中当前产品名和 exe 名同步改为 AWJ。历史 spec 文档不做全文机械修改，只在新设计文档中说明旧文档里的 `AVIF-WebP-Studio` 是历史名称，避免篡改历史决策记录。

## 错误处理

若 GitHub 仓库重命名失败，停止远端改名和 remote URL 更新，保留本地代码改名计划，并输出明确错误与手动处理方式。

若本地目录改名失败，先尝试切换到父目录再重命名；如果仍失败，停止实施，不在错误路径上继续代码改名。

若静态扫描发现旧产品名仍残留在 CMake、src、scripts、tests、README 或当前文档中，应在提交前修正。历史 spec 中的旧产品名允许保留。

## 验证计划

实施后进行三层验证。

### Git 与远端

- `git status --short --branch`
- `git remote -v`
- `git fetch origin`

### 静态扫描

扫描旧产品名残留：

- `AVIF-WebP-Studio`
- `AVIF-WebP-Cli`
- `AVIF-WebP-Native-Avif-Aom`
- `AVIFWebPStudio`
- `AVIF-WebP`
- `FFimage`

历史 spec 中的旧名称允许保留；其他位置应清理。

确认新名称出现并绑定正确：

- `AWJimage`
- `AWJ-cli`
- `AWJ-studio`
- `AWJ-native-avif-helper`

同时确认 AVIF 作为图像格式名仍存在。

### 构建与测试

至少构建受影响 target：

- `AWJ-cli`
- `AWJ-studio`
- `AWJ-native-avif-helper`

至少运行关键验证：

- CLI help
- native AVIF helper 路径相关测试
- CTest 可用时运行全量测试

如果 Windows 权限策略拦截带 `-ExecutionPolicy Bypass` 的 CTest，则沿用此前方式，直接运行测试可执行文件和 CLI 断言进行等价验证。

## 提交与推送策略

设计文档单独提交，提交信息为：

```text
设计 AWJimage 深度改名
```

实际代码改名完成并验证后创建一个本地提交，建议提交信息为：

```text
重命名项目为 AWJimage
```

代码改名提交完成后先不自动 push，除非用户再次明确要求推送。远端仓库名称变更会在实施流程中发生，因为这是用户明确要求的一部分。

## 成功标准

- 本地目录为 `D:\Code\Cpp\AWJimage`。
- GitHub 远端仓库为 `Dominic485649/AWJimage`。
- 本地 `origin` 指向 `git@github.com:Dominic485649/AWJimage.git`。
- 构建产物为 `AWJ-cli.exe`、`AWJ-studio.exe` 和 `AWJ-native-avif-helper.exe`。
- UI 和 CLI 用户可见品牌为 AWJ。
- AVIF/WebP/JXL 作为格式名保持正确。
- 构建和关键测试通过。
- 旧产品名只允许出现在历史 spec 或明确历史说明中。
