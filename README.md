# AWJimage

AWJimage 是一个 Windows C++23 / Slint 批量图片转换工具。当前内置转换路径只保留 native codec：

- AVIF：libavif/AOM，实验 `zenrav1e` 通过私有 helper 进程隔离
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

AVIF helper 会放在内部目录：

- `bin\x64\internal\Release\AWJ-native-avif-helper.exe`

它不是用户直接启动的文件。

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
