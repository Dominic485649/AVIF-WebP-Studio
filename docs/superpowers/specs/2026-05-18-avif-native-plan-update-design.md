# AVIF native 剩余计划更新设计

本文档是 AVIF-WebP-Studio 后续 AVIF/native 编码器、visual_quality、大图压缩、grid 分块、默认参数、CLI/UI 与验收标准的新依据。旧文档 `2026-05-18-native-codec-visual-quality-design.md` 仅保留为历史记录，不再作为后续执行依据。

当前阶段只更新计划、设计说明和验收标准；不修改源码、不构建、不测试、不安装依赖、不删除 Magick。

## 1. 总体目标

AVIF-WebP-Studio 后续目标是从当前 WebP/JXL native 基线扩展为完整 native 编解码体系，重点补齐 AVIF native encoder registry、visual_quality 固定速度搜索、大图压缩、grid 分块规划、默认参数集中管理，以及 CLI/UI/diagnostics/CSV 的完整可观测性。

### 已确认基线

- WebP/JXL native codec 基础实现已完成。
- native backend 初版已完成。
- visual_quality 核心评分、GMSD/MS-SSIM、native visual search 已完成。
- 资源规划模块已完成。
- CLI 已有 `--backend native`、`--memory-limit`、visual quality 参数。
- UI 已有后端选择、内存限制输入。
- 当前完整 CTest 为 `19/19 passed`。

### 待验证事项

- JXL full pipeline 输出路径此前出现过 access violation，需要单独定位。
- 自动内存预算、大图队列、grid 规划在真实大图场景下的行为需要后续验证。
- AVIF native 的实际依赖路径需要确定，尤其是 `svt-av1-hdr`、libheif、AOM、zenrav1e 的集成方式。

### 待实现事项

- AVIF native encoder registry。
- SVT/SVT-AV1-HDR、AOM、zenrav1e/rav1e 支持。
- 更多专业输入解码器。
- 大图压缩队列。
- grid/tile 分块压缩规划。
- 默认参数集中模块。
- diagnostics/CSV 字段扩展。
- native 完整稳定后再清理 Magick。

## 2. 本次新增/修正规则

### 2.1 visual_quality 固定速度规则

已确认：`visual_quality` 只搜索质量参数，不调整速度参数。

固定项包括：

- JXL：`effort` 固定，只搜索 `distance/quality` 映射。
- WebP：`method/speed` 固定，只搜索 `quality`。
- SVT/SVT-AV1-HDR：`preset` 固定，只搜索 `crf/qp/quality` 映射。
- AOM：`cpu-used` 固定，只搜索 `cq-level/crf/qp`。
- zenrav1e/rav1e：`preset/speed` 固定，只搜索 `quality/quantizer`。

`visual_quality=100` 保持无损或最接近无损语义，不允许为了速度或体积降低质量语义。

候选选择规则保持：

- 达标候选中选最小体积。
- 体积相同选更低 encoder quality。
- 无达标且 fallback 关闭时报错。
- fallback 开启时选最接近目标候选，并记录 fallback reason。

### 2.2 默认参数集中规则

已确认：新增单独默认参数模块，建议命名为 `src/core/encoding_defaults.ixx`。

要求：

- 默认参数集中管理。
- 使用 `inline constexpr` / `constexpr`。
- CLI 默认值、UI 默认值、encoder registry 默认值都从该模块读取。
- 不允许在 CLI、UI、encoder registry、backend 中重复写死默认值。
- 后续修改默认 preset、speed、quality、bit-depth、chroma、大图阈值时，优先只改默认参数模块。

### 2.3 AVIF encoder registry 规则

已确认：AVIF native 不绑定单一 SVT 路径，而是新增 encoder registry。

registry 按能力选择 encoder，至少考虑：

- requested/applied chroma。
- requested/applied bit-depth。
- pixel_count。
- 是否超过 SVT 安全像素上限。
- 是否需要 422/444。
- 是否需要 12-bit。
- 是否 experimental。
- feature flag 是否启用。
- license 是否允许默认发行。

### 2.4 大图规则

已确认：

- `pixel_count = width * height`。
- `> 20,000,000 px` 进入 large-image queue，并从普通队列剔除。
- `> 35,000,000 px` 禁止使用 SVT/SVT-AV1-HDR。
- 超大图必须选择安全路径：AOM、zenrav1e/rav1e、JXL、grid 分块压缩，或其他明确记录 fallback reason 的策略。

## 3. 默认参数常量区设计

### 3.1 模块设计

新增轻量模块：`src/core/encoding_defaults.ixx`。

建议 namespace：

```cpp
namespace avif::encoding_defaults {
  // constants only
}
```

模块不依赖大型第三方库，只提供默认值和轻量枚举/常量。

### 3.2 常量内容

建议包含：

```cpp
namespace avif::encoding_defaults {

inline constexpr int default_jxl_effort = 7;
inline constexpr bool default_jxl_effort_is_explicit = false;

inline constexpr int default_svt_preset = /* fastest */;
inline constexpr int default_aom_cpu_used = 6;
inline constexpr int default_zenrav1e_preset = 6;

inline constexpr int default_quality = 80;
inline constexpr int default_visual_quality = 90;

inline constexpr int large_image_threshold_pixels = 20'000'000;
inline constexpr int svt_safe_max_pixels = 35'000'000;

inline constexpr int default_grid_overlap_pixels = 0;

inline constexpr bool default_avif_tune_iq = true;
inline constexpr bool default_zenrav1e_enable_qm = true;
inline constexpr bool default_zenrav1e_enable_trellis = false;
inline constexpr bool default_zenrav1e_enable_vae_or_vaq = false;

}
```

### 3.3 JXL 默认 effort 规则

已确认：

- JXL 默认 effort 为 `7`，对齐 `cjxl` 默认值。
- 默认情况下不向 libjxl 显式传递 effort 参数。
- 只有用户显式指定 `--effort`，或统一 `--speed` 映射到 JXL effort 时，才传递该值。
- visual_quality 搜索期间固定 effort：未指定时固定为 codec 默认 effort=7，已指定时固定为用户指定 effort。

设计含义：

- `default_jxl_effort = 7` 表示项目认定的 JXL 默认 effort。
- `default_jxl_effort_is_explicit = false` 表示默认状态下不主动传递 effort。
- CLI help / UI 可以展示“默认 7（codec/cjxl 默认，不显式传递）”。
- diagnostics 中区分 `requested_effort=auto`、`applied_effort=7`、`effort_explicit=false`。

如果用户显式指定 effort，则 diagnostics 记录 `requested_effort=<用户值>`、`applied_effort=<用户值>`、`effort_explicit=true`，编码器实际传递 effort 参数。

### 3.4 适用范围

已确认：以下来源必须改为引用默认参数模块：

- `AppConfig` 默认值。
- CLI help 默认值。
- UI 初始值。
- encoder registry 默认值。
- speed/effort/preset/cpu-used 默认映射。
- large-image 阈值。
- grid overlap 默认值。
- tune/QM/trellis/VAQ 默认值。

### 3.5 不放入常量区的内容

已确认：默认参数常量区不负责解释用户质量档位。

不写入：

- `quality=30` 是什么含义。
- `quality=50` 是什么含义。
- `quality=65/80/95/100` 的语义解释。

`quality` 具体含义由用户选择和 encoder 映射决定。

## 4. 编码器选择矩阵

### 4.1 AVIF encoder 候选

#### 4.1.1 svt-av1-hdr

状态：待实现，主力优先路径。

适合：

- 420。
- 8-bit。
- 10-bit。
- HDR。
- still image。
- tune=iq。

不适合：

- 422。
- 444。
- 超过 SVT 安全像素上限的超大图。

硬性限制：

- `pixel_count > 35,000,000` 时不得使用。
- requested chroma 为 422/444 时不得使用。
- 不满足 profile/bit-depth 时不得强行使用。

still image 强制参数：

- `avif=1`。
- `tune=iq` 或 `tune=3`。
- `keyint=1`。

如果走 ffmpeg wrapper，至少保证 `-g 1` 或 `-svtav1-params keyint=1`。

如果走 `SvtAv1EncApp`，必须传 `--keyint 1`。

如果走 dedicated library API：

- 必须映射到底层等价字段。
- 代码注释说明其等价于 `keyint=1 / all-intra / still image`。
- diagnostics 记录 `applied_keyint=1`。

#### 4.1.2 AOM

状态：待实现，补充路径。

主要用途：

- 弥补 SVT/SVT-AV1-HDR 不支持 422/444 的问题。
- 支持更完整 chroma/profile/bit-depth 组合。
- 12-bit 或专业 profile 场景。
- SVT 超过安全像素上限时的候选路径。

默认策略：

- `cpu-used=6`。
- `tune=iq`。
- 默认值来自 `encoding_defaults.ixx`。

visual_quality 策略：

- 固定 `cpu-used`。
- 只搜索 `cq-level/crf/qp/quality` 映射。
- 不得在搜索期间自动改变速度。

诊断记录：

- `requested_tune=iq`。
- `applied_tune=iq`。
- `requested_cpu_used`。
- `applied_cpu_used`。

#### 4.1.3 zenrav1e

状态：实验性，默认不启用。

来源：`https://github.com/imazen/zenrav1e`

用途：

- still AVIF。
- animated AVIF。
- 420/422/444。
- 多位深场景下作为 AOM 外的 experimental 候选。

默认策略：

- 默认 quality 来自 `encoding_defaults.ixx`。
- 默认 `preset/speed=6`。
- 默认 `enable_qm=true`。
- 默认不启用 trellis quantization。
- 默认不启用 VAQ/VAE。
- still image 必须启用 `still_picture=true` 或等价 still image mode。
- 使用 `Tune::StillImage` 或等价 photographic/still-image tuning。

启用要求：

- 默认不参与发行构建。
- 必须通过 CMake option / feature flag / cargo feature 控制。
- UI 默认不显示，开启 experimental 后才显示。
- 必须标注 experimental。
- 必须标注 license warning。

许可证规则：

- zenrav1e 为 AGPL-3.0 / commercial 双许可。
- 不能无脑静态集成进默认发行包。
- 如许可证不适合发行，只支持用户本地自行启用。

#### 4.1.4 rav1e / 其他 AV1 encoder

状态：预留扩展点。

要求：

- registry 支持后续扩展。
- 不把 encoder 选择逻辑散落在 backend if/else 中。

### 4.2 chroma / bit-depth 选择规则

新增 chroma：`auto`、`420`、`422`、`444`。

新增 bit-depth：`auto`、`8`、`10`、`12`。

#### 4.2.1 bit-depth auto

已确认：

- `bit-depth=auto` 优先尝试 10-bit。
- 如果当前 encoder + chroma + profile 不支持 10-bit 或更高，则自动锁定为 8-bit。
- 不允许静默失败。
- diagnostics/CSV 必须记录 `requested_bit_depth`、`applied_bit_depth`、`bit_depth_reason`。

#### 4.2.2 encoder 选择建议

| 场景 | 优先 encoder | 状态 |
|---|---|---|
| 420 + 8-bit | svt-av1-hdr | 待实现 |
| 420 + 10-bit | svt-av1-hdr | 待实现 |
| 422 | aom | 待实现 |
| 444 | aom | 待实现 |
| 12-bit | aom 或 zenrav1e/rav1e | 待实现/实验性 |
| > 35MP | 非 SVT 路径 | 待实现 |
| zenrav1e feature disabled | 不参与选择 | 已确认规则 |
| zenrav1e feature enabled | experimental 候选 | 实验性 |

## 5. visual_quality 策略

### 5.1 统一规则

已确认：visual_quality 是面向用户的视觉目标，但搜索空间只包含质量参数。

不允许搜索或自动改变：

- JXL `effort`。
- WebP `method/speed`。
- SVT `preset`。
- AOM `cpu-used`。
- zenrav1e/rav1e `preset/speed`。
- tune 策略。
- chroma。
- bit-depth。
- encoder 选择，除非当前 encoder 无法满足目标格式/能力约束。

### 5.2 各 encoder 搜索变量

| Encoder | 固定项 | visual_quality 搜索变量 |
|---|---|---|
| WebP | method/speed | quality |
| JXL | effort，默认 7 且默认不显式传递 | distance/quality 映射 |
| SVT/SVT-AV1-HDR | preset | crf/qp/quality 映射 |
| AOM | cpu-used | cq-level/crf/qp |
| zenrav1e/rav1e | preset/speed | quality/quantizer |

### 5.3 `visual_quality=100`

已确认：

- 表示无损或最接近无损语义。
- 不允许为了速度或体积降低质量语义。
- 若 encoder 无法提供真正 lossless，应明确记录 `lossless=false` 和 fallback reason，或拒绝该组合。
- 不允许静默把 100 当作普通高质量有损编码。

### 5.4 fallback 规则

无达标候选时：

- fallback 关闭：报错。
- fallback 开启：选择最接近目标候选。

诊断记录：

- `visual_quality_fallback_used`。
- `visual_quality_fallback_reason`。
- `requested_visual_quality`。
- `visual_score`。
- `raw_gmsd`。
- `raw_ms_ssim`。
- `gmsd_quality_score`。
- `msssim_quality_score`。
- `final_encoder_quality`。
- `search_attempt_count`。

## 6. 大图与 grid 策略

### 6.1 large-image planner

新增 large-image compression planner。

#### 6.1.1 阈值

默认值来自 `encoding_defaults.ixx`：

| 项目 | 默认值 |
|---|---:|
| 大图阈值 | `20,000,000 px` |
| SVT 安全上限 | `35,000,000 px` |

#### 6.1.2 处理规则

已确认：

- 正常批量压缩前扫描图片尺寸。
- `pixel_count = width * height`。
- `pixel_count > 20,000,000` 时从普通队列剔除并进入 large-image queue。
- `pixel_count > 35,000,000` 时禁止 SVT/SVT-AV1-HDR。

可选策略：

- AOM。
- zenrav1e/rav1e。
- JXL。
- grid 分块压缩。

#### 6.1.3 diagnostics/CSV

必须记录：

- `width`。
- `height`。
- `pixel_count`。
- `large_image_mode`。
- `excluded_from_normal_queue`。
- `selected_large_image_strategy`。
- `svt_allowed`。
- `svt_block_reason`。

### 6.2 grid/tile 分块压缩

#### 6.2.1 目标

为超大图提供 grid 分块压缩路径，避免单张超大图压垮 SVT 或阻塞普通队列。

#### 6.2.2 基本规则

- 按网格切分为多个 tile。
- 每个 tile 单独压缩。
- 分块边界考虑 overlap/padding，避免视觉边界断裂。
- 默认 overlap 来自 `encoding_defaults.ixx`，初始为 `0`。
- 首版可只实现 planner + manifest 设计。

#### 6.2.3 输出策略

至少设计两种：

1. 内部工程格式：manifest + tiles，用于后续重组和内部处理。
2. 标准格式 fallback：如果目标格式不能表达 grid tiles，则改走 JXL 或其他更适合大图的单图格式。

#### 6.2.4 manifest 字段

manifest 至少记录：

- `original_width`。
- `original_height`。
- `tile_width`。
- `tile_height`。
- `overlap`。
- `rows`。
- `cols`。
- `encoder`。
- `quality/crf/qp`。
- `speed/effort/preset/cpu-used`。
- `bit_depth`。
- `chroma`。
- `tile_file_list`。
- `reconstruction_checksum` 或尺寸校验。

#### 6.2.5 CLI/UI 预留

CLI 预留：

- `--large-image-mode auto|off|queue|grid`。
- `--large-image-threshold-pixels <int>`。
- `--svt-safe-max-pixels <int>`。
- `--grid-tile-size <WxH>`。
- `--grid-overlap <pixels>`。

UI 后续显示：

- large-image mode。
- selected strategy。
- tile size。
- overlap。
- manifest 输出路径。
- grid fallback reason。

## 7. CLI/UI 改动

### 7.1 CLI 参数

#### AVIF encoder

```text
--avif-encoder auto|svt-av1-hdr|aom|zenrav1e
```

规则：

- 默认 `auto`。
- `zenrav1e` 仅在 experimental feature enabled 时可用。
- 未启用时选择 `zenrav1e` 必须报错并提示 license/feature 状态。

#### chroma

```text
--chroma auto|420|422|444
```

规则：

- 保留现有 chroma 参数语义。
- 422/444 不得选择 SVT。

#### bit depth

```text
--bit-depth auto|8|10|12
```

规则：

- `auto` 优先 10-bit。
- 不支持时锁定 8-bit，并记录 reason。

#### large image

```text
--large-image-mode auto|off|queue|grid
--large-image-threshold-pixels <int>
--svt-safe-max-pixels <int>
```

规则：

- 默认值来自 `encoding_defaults.ixx`。
- 超过 SVT 安全上限时不能强行使用 SVT。

#### grid

```text
--grid-tile-size <WxH>
--grid-overlap <pixels>
```

规则：

- 首版可只生成 planner/manifest。
- overlap 默认来自 `encoding_defaults.ixx`。

#### tune

```text
--no-avif-tune-iq
```

规则：

- 只允许 CLI 关闭。
- UI 不暴露关闭入口。
- SVT 默认 tune=iq。
- AOM 默认 tune=iq。

#### speed/effort/preset/cpu-used

可保留统一抽象或增加 encoder-specific alias：

- `--speed`。
- `--effort`。
- `--preset`。
- `--cpu-used`。

规则：

- JXL 默认 effort=7，默认不显式传递。
- SVT 默认最快 preset。
- AOM 默认 `cpu-used=6`。
- zenrav1e 默认 `preset/speed=6`。
- 用户显式指定后必须固定。
- visual_quality 搜索不得修改速度参数。

#### help 默认值

已确认：CLI help 中展示的默认值必须来自默认参数模块，不允许 CLI 内部再写死一份。

JXL help 文案建议：

```text
JXL effort 默认 7（codec/cjxl 默认；未显式指定时不传递 effort）
```

### 7.2 UI 改动

#### visual_quality UI

已确认：

- visual_quality 开启时禁用 quality 输入。
- 不禁用 speed/effort/preset/cpu-used。
- 明确提示：visual_quality 只搜索质量，不改变速度。

#### encoder UI

计划显示 AVIF encoder：

- auto。
- svt-av1-hdr。
- aom。
- zenrav1e，只有 experimental feature enabled 后显示。

zenrav1e 必须标注 experimental 和 license warning。tune=iq 不提供 UI 关闭入口。

#### JXL UI

- JXL speed/effort 默认显示为 `默认 7（不显式传递）`。
- 如果用户填入值，则显示实际传递 effort。
- visual_quality 开启时提示：`视觉质量只搜索质量，JXL effort 保持默认 7 或用户指定值。`

#### 输出状态 UI

应显示：

- 实际 encoder。
- requested/applied chroma。
- requested/applied bit-depth。
- fallback reason。
- large-image mode。
- grid strategy。
- visual score。
- GMSD。
- MS-SSIM。
- Qg/Qm。
- final quality。
- attempts。
- lossless flag。
- 固定速度参数。

#### 默认值

已确认：UI 初始值必须来自默认参数模块，不允许 UI 单独写死默认 preset/speed/quality/threshold。

## 8. diagnostics/CSV 字段

### 8.1 编码器与参数字段

新增或扩展：

- `backend`。
- `encoder_selected`。
- `encoder_requested`。
- `encoder_experimental`。
- `requested_quality`。
- `final_encoder_quality`。
- `requested_visual_quality`。
- `visual_score`。
- `raw_gmsd`。
- `raw_ms_ssim`。
- `gmsd_quality_score`。
- `msssim_quality_score`。
- `search_attempt_count`。
- `lossless`。

### 8.2 speed/effort/preset/cpu-used 字段

新增或扩展：

- `speed_parameter_kind`。
- `requested_speed`。
- `applied_speed`。
- `requested_effort`。
- `applied_effort`。
- `effort_explicit`。
- `requested_preset`。
- `applied_preset`。
- `requested_cpu_used`。
- `applied_cpu_used`。
- `visual_quality_speed_locked`。

### 8.3 tune/still-image 字段

新增：

- `requested_tune`。
- `applied_tune`。
- `avif_tune_iq_enabled`。
- `applied_keyint`。
- `applied_avif_mode`。
- `applied_still_picture`。

SVT/SVT-AV1-HDR still image 路径必须记录：

- `applied_keyint=1`。
- `applied_avif_mode=true`。

AOM 默认必须记录：

- `requested_tune=iq`。
- `applied_tune=iq`。

### 8.4 chroma / bit-depth 字段

新增：

- `requested_chroma`。
- `applied_chroma`。
- `requested_bit_depth`。
- `applied_bit_depth`。
- `bit_depth_reason`。
- `fallback_reason`。

### 8.5 large-image / grid 字段

新增：

- `width`。
- `height`。
- `pixel_count`。
- `large_image_mode`。
- `excluded_from_normal_queue`。
- `selected_large_image_strategy`。
- `svt_allowed`。
- `svt_block_reason`。
- `grid_enabled`。
- `grid_tile_width`。
- `grid_tile_height`。
- `grid_overlap`。
- `grid_rows`。
- `grid_cols`。
- `grid_manifest_path`。

### 8.6 fallback 字段

新增：

- `decoder_fallback_used`。
- `encoder_fallback_used`。
- `visual_quality_fallback_used`。
- `fallback_reason`。
- `fallback_chain`。

## 9. 测试验收标准

### 9.1 默认参数常量区

必须测试或静态检查：

- 存在单独 `encoding_defaults.ixx` 或等价默认参数模块。
- CLI 默认值来自该模块。
- UI 默认值来自该模块。
- encoder registry 默认值来自该模块。
- 不重复写死 AOM `cpu-used=6`、zenrav1e `preset/speed=6`、JXL `effort=7`、大图阈值、SVT 安全上限、tune/QM/trellis/VAQ 默认值。

### 9.2 visual_quality

必须测试：

- visual_quality 搜索期间 speed/effort/preset/cpu-used 不变化。
- visual_quality 只调整质量参数。
- visual_quality=100 保持 lossless 或近似无损语义。
- 达标候选选最小体积。
- 体积相同选更低 encoder quality。
- fallback 关闭时报错。
- fallback 开启时选择最接近目标候选并记录 reason。

### 9.3 默认速度

必须测试：

- JXL 未指定 effort 时不向 libjxl 显式设置 effort。
- JXL 未指定 effort 时 diagnostics 记录 `requested_effort=auto`、`applied_effort=7`、`effort_explicit=false`。
- JXL 显式指定 effort 时向 libjxl 传递用户指定值，并记录 `effort_explicit=true`。
- JXL visual_quality 搜索期间未指定 effort 时固定为默认 7，已指定 effort 时固定用户值。
- SVT 默认最快 preset。
- AOM 默认 `cpu-used=6`。
- zenrav1e 默认 `preset/speed=6`。
- 用户显式指定速度后不得被 visual_quality 覆盖。

### 9.4 SVT/SVT-AV1-HDR

必须测试：

- AVIF still image 编码参数包含 `keyint=1`。
- AVIF still image 编码参数包含 `avif=1`。
- AVIF still image 编码参数包含 `tune=iq` 或 `tune=3`。
- diagnostics 记录 `applied_keyint=1`。
- `> 35MP` 图片不得选择 SVT。
- 422/444 不得选择 SVT。

### 9.5 AOM

必须测试：

- 默认 `cpu-used=6`。
- 默认 `tune=iq`。
- 422/444 输入不会错误选择 SVT。
- visual_quality 搜索期间 `cpu-used` 固定。
- diagnostics 记录 `requested_tune=iq` 和 `applied_tune=iq`。

### 9.6 zenrav1e

必须测试：

- 默认未启用。
- feature flag 开启后才参与 encoder registry。
- 默认 quality 来自默认参数模块。
- 默认 `preset/speed=6`。
- still_picture=true 或等价字段启用。
- enable_qm=true。
- trellis 默认关闭。
- VAQ/VAE 默认关闭。
- `quality=100` 时 lossless 语义正确，或明确拒绝不支持的 lossless 组合。
- 文档包含 AGPL/commercial license warning。
- 文档不硬编码解释 `quality 30/50/65/80/95/100` 的语义。

### 9.7 chroma / bit-depth

必须测试：

- `bit-depth=auto` 优先尝试 10-bit。
- 不支持 10-bit+ 时自动锁定为 8-bit。
- requested/applied bit-depth 写入 CSV/diagnostics。
- requested/applied chroma 写入 CSV/diagnostics。
- 422/444 不走 SVT。
- 12-bit 优先 AOM 或 experimental zenrav1e/rav1e，不强行走 SVT。

### 9.8 大图

必须测试：

- `> 20MP` 图片进入 large-image queue。
- `> 20MP` 图片从普通队列剔除。
- `> 35MP` 图片禁止 SVT。
- diagnostics 记录 large_image_mode。
- diagnostics 记录 selected_large_image_strategy。
- 阈值来自默认参数模块。

### 9.9 grid

必须测试：

- grid planner 能生成 manifest。
- manifest 包含原图尺寸、tile 尺寸、overlap、rows/cols、encoder、quality/crf/qp、speed/effort/preset/cpu-used、bit-depth、chroma、tile list、reconstruction checksum 或尺寸校验。
- 超大图可选择 grid 或安全 fallback。
- 目标格式不能表达 grid tiles 时，走明确 fallback。

### 9.10 CLI

必须测试：

- 新参数 help 文案存在。
- 非法参数不崩溃。
- 未指定 `--backend` 时保持旧默认行为，除非后续计划明确切换默认 backend。
- 未指定 `--visual-quality` 时仍使用固定 quality。
- 指定 `--visual-quality` 时 quality 输入被搜索结果覆盖，但 speed 固定。
- help 中展示默认值与默认参数模块一致。
- `--no-avif-tune-iq` 只影响 CLI。
- 未启用 zenrav1e feature 时指定 `--avif-encoder zenrav1e` 报错。

### 9.11 UI

必须验证：

- visual_quality 开启后 quality 禁用。
- speed/effort/preset/cpu-used 不禁用。
- 显示实际 encoder/chroma/bit-depth/fallback。
- experimental zenrav1e 有警告。
- tune=iq 没有 UI 关闭入口。
- UI 默认值与默认参数模块一致。
- native+不支持组合必须明确提示，不崩溃。

## 10. 风险与未确认项

### 10.1 SVT/SVT-AV1-HDR 集成方式

待验证：

- vcpkg 官方路径是否能满足 `svt-av1-hdr` 要求。
- 是否需要 overlay port。
- libheif 是否能直接接入自定义 SVT encoder。
- 如果 libheif 无法启用 SVT，是否需要 patch vcpkg libheif port、直接调用 SVT AV1 bitstream 后封装 AVIF，或保留其他 container 写入路径。

### 10.2 AOM 依赖与 Magick 符号冲突

待验证：

- 当前 Magick 静态 delegate 可能已有 heif/aom 相关依赖。
- AOM native target 与 Magick 链接在同一最终可执行文件中是否有符号冲突。
- 可能需要进一步拆分 target 或延后 Magick 链接。

### 10.3 zenrav1e 许可证

已确认风险：

- AGPL-3.0 / commercial 双许可。
- 默认发行包不得无脑静态集成。
- 必须 experimental feature flag。
- 文档必须明确风险。

### 10.4 JXL full pipeline 崩溃

待验证：

- `jxl_codec_core` 通过不代表 full pipeline 稳定。
- 需要单独复现 WebP/JXL input 到 JXL output 的 full pipeline。
- 需要区分 JXL encoder 生命周期问题、pipeline 对象析构顺序问题、Magick/JXL 静态链接析构冲突、测试进程退出阶段问题。

### 10.5 grid 格式设计

待验证：

- 内部 manifest + tiles 是否作为项目专有格式。
- 是否需要未来支持标准 tile-capable 容器。
- 重组校验、颜色管理、metadata 是否保留。
- tile overlap 默认 0 是否足够。

### 10.6 bit-depth auto 体积/兼容性

待验证：

- 8-bit 输入提升 10-bit 对体积和兼容性的影响。
- zenrav1e 或 AOM 在自动 10-bit 下是否适合所有输入。
- diagnostics 必须记录 applied bit-depth 和 reason。

## 11. 建议执行顺序

### 阶段 1：设计与计划落地

1. 新增本更新稿为独立设计文档。
2. 标注旧 native codec 设计文档不再作为后续执行依据。
3. 自审设计文档：无占位、无前后矛盾、区分已确认/待验证/实验性、验收标准可测试。

### 阶段 2：默认参数与 registry 基础

1. 新增 `encoding_defaults.ixx`。
2. 改 CLI/UI/backend/registry 默认值来源。
3. 建立 AVIF encoder registry 类型和 capability 模型。
4. 增加默认参数一致性测试。

### 阶段 3：JXL full pipeline 稳定性

1. 新增 WebP 到 JXL full pipeline 测试。
2. 拆分定位 JXL 崩溃来源。
3. 修复 JXL encoder 生命周期或确认是第三方静态析构问题。
4. 保持 native WebP/JXL 现有测试继续通过。

### 阶段 4：AVIF encoder registry 第一版

1. 接入 SVT/SVT-AV1-HDR 路径。
2. 强制 still image 参数：`avif=1`、`tune=iq`、`keyint=1`。
3. 接入 AOM 路径。
4. 实现 chroma/bit-depth capability selection。
5. 实现 diagnostics/CSV 基础字段。

### 阶段 5：visual_quality 与 AVIF 联通

1. AVIF 接入 native visual quality search。
2. 确认搜索只调整质量参数。
3. 增加 SVT/AOM visual_quality 测试。
4. 验证 `visual_quality=100` lossless/近似无损语义。

### 阶段 6：大图 planner

1. 扫描 width/height/pixel_count。
2. 实现 large-image queue。
3. 实现 SVT 安全上限判断。
4. 增加 diagnostics/CSV 字段。
5. 增加大图边界测试。

### 阶段 7：grid planner

1. 实现 grid/tile planner。
2. 生成 manifest。
3. 预留 CLI 参数。
4. 明确标准格式 fallback。
5. 增加 manifest 测试。

### 阶段 8：zenrav1e experimental

1. 增加 feature flag。
2. 文档写明 AGPL/commercial 风险。
3. registry 中默认禁用。
4. feature enabled 后参与选择。
5. 验证默认 preset/speed/QM/trellis/VAQ/still_picture。

### 阶段 9：专业 decoder 扩展

按顺序接入：PNG、JPEG、TIFF、GIF、JPEG 2000、HEIF/AVIF、RAW、BMP。

同时建立 decoder registry，避免 `decoder_for_path` 继续硬编码膨胀。

### 阶段 10：UI/CLI 完整化

1. CLI 增加 AVIF encoder、large-image、grid、tune 参数。
2. UI 显示实际 encoder/chroma/bit-depth/fallback。
3. UI 不暴露 tune=iq 关闭入口。
4. visual_quality 开启时只禁用 quality，不禁用 speed。
5. experimental zenrav1e 有 license warning。

### 阶段 11：Magick 清理

前置条件：native WebP/JXL/AVIF 编码稳定，常见输入格式 decoder 覆盖足够，visual_quality native 路径稳定，large-image/grid 策略可用，CLI/UI smoke tests 通过。

执行：

1. 保留 backend 接口。
2. 默认切换 native 或提供明确迁移策略。
3. 删除不再需要的 Magick runtime 拷贝。
4. 移除 Magick 头文件/库依赖。
5. 清理 CMake、scripts、docs。
6. 降低硬盘占用。

## 最终关键参数表

| 项目 | 默认值 |
|---|---:|
| JXL `effort` | `7`，默认不显式传递 |
| AOM `cpu-used` | `6` |
| AOM `tune` | `iq` |
| zenrav1e `preset/speed` | `6` |
| SVT still image `keyint` | `1` |
| 大图阈值 | `20,000,000 px` |
| SVT 安全上限 | `35,000,000 px` |
