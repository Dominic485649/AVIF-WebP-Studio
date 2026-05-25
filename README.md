# AWJimage

AWJimage 是一个 Windows C++23 / Slint 批量图片转换工具。当前内置转换路径只保留 native codec：

- AVIF：libavif/AOM；实验 `zenrav1e` 静态链接进主程序但仍需显式启用；`svt-av1-hdr` 通过 libavif SVT backend 静态链接进主程序
- WebP：libwebp
- JXL：libjxl

内置 ImageMagick/MagickWand 后端已经移除，Release 输出不再携带 ImageMagick XML、许可文件或模块目录。Magick 与 ffmpeg 以后只能作为外部集成重新引入；当前版本不处理该环节。

## 构建

推荐使用预设：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ-cli AWJ-studio
```

或使用脚本：

```powershell
.\release.ps1
```

脚本会配置 native 依赖并清理 Release 输出目录，只保留面向用户的：

- `bin\x64\Release\AWJ-cli.exe`
- `bin\x64\Release\AWJ-studio.exe`

`svt-av1-hdr` 已作为源码依赖构建并静态链接进主程序，Release 不需要 `SvtAv1EncApp.exe`、DLL 或其他 SVT sidecar。当前 SVT 路径仍限制为 420 色度采样和 8/10-bit AVIF 输出。

## CLI 示例

```powershell
AWJ-cli.exe -i "D:\图片" -o Avifoutput --format avif -q q90
AWJ-cli.exe -i input.png --format webp --template "{name}-{date}"
AWJ-cli.exe -i input.png -o output.jxl --format jxl -q 90
AWJ-cli.exe -i input.png --format avif --avif-encoder aom --chroma 444 --bit-depth 10
AWJ-cli.exe -i input.png --format avif --avif-encoder zenrav1e --experimental-encoders
```

`zenrav1e` 是实验编码器：必须显式选择 `--avif-encoder zenrav1e` 并加 `--experimental-encoders`。默认 `auto` 不会选择实验编码器。

## 测试

```powershell
ctest --test-dir build/x64/Release -C Release --output-on-failure
```
