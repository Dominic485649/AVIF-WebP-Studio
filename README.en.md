# AWJimage

Chinese: [README.md](README.md)

1.0.0 release notes (Chinese): [RELEASE_NOTES_1.0.0.md](RELEASE_NOTES_1.0.0.md)

AWJimage is a C++23 / Slint batch image converter. Windows and Linux now share the same mainline. The conversion path is native-only:

- AVIF: libavif/AOM, experimental `zenrav1e`, and `svt-av1-hdr`; Windows and Linux GCC Release builds link them statically.
- WebP: libwebp
- JXL: libjxl
- JPGLI: google/jpegli; produces JPEG-compatible bitstreams with the default `.jpg` extension.

The built-in ImageMagick/MagickWand backend has been removed. Magick and ffmpeg can only return later as explicit external integrations.

Linux keeps one ELF `AWJ` for both Slint UI and CLI. Visual-quality GPU metrics use Vulkan and fall back to CPU on failure, tiny images, or resource limits. WIC, JXR, `AWJ.com`, and Windows registry shell integration remain Windows-only. Linux hides WIC fallback UI and provides user-level Nautilus Scripts plus Thunar UCA actions without sudo.

## 1.0.0 GitHub release archives

Download the platform-matched archive from [GitHub Release 1.0.0](https://github.com/Dominic485649/AWJimage/releases/tag/1.0.0). The archives deliberately do not mix platform files:

| Archive | Exact contents | SHA-256 |
|---|---|---|
| [AWJ_Linux.7z](https://github.com/Dominic485649/AWJimage/releases/download/1.0.0/AWJ_Linux.7z) | Linux ELF: `AWJ` | Release body |
| [AWJ_Win.7z](https://github.com/Dominic485649/AWJimage/releases/download/1.0.0/AWJ_Win.7z) | Windows: `AWJ.exe`, `AWJ.com` | Release body |

Both use 7-Zip LZMA2 at maximum compression with one compression thread (`-t7z -m0=lzma2 -mx=9 -mmt=1 -mf=off`) and were checked with `7z t` before upload. `-mf=off` prevents the automatic BCJ2 filter for `.exe` files so the method stays LZMA2.

## Build

Windows (MSVC):

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
```

Linux (WSL local tree, preferred `/home/dominic/Code/Cpp/AWJimage`; avoid building on `/mnt/d`):

```bash
cd /home/dominic/Code/Cpp/AWJimage
cmake --preset linux-gcc-x64-release
cmake --build --preset linux-gcc-x64-release --target AWJ
```

`linux-gcc-x64-release` uses `-O3`, IPO/LTO, `-march=x86-64-v3`, section GC/strip, and statically links `libstdc++` / `libgcc`. Only `AWJ` is produced; there is no `AWJ.com`.

Windows release script:

```powershell
.\release.ps1
```

Versioning is controlled by the root `VERSION` file. Release flow:

```powershell
Set-Content VERSION "1.0.0"
.\scripts\Update-VcpkgVersion.ps1
git add VERSION vcpkg.json CHANGELOG.md
git commit -m "release: 1.0.0"
git tag 1.0.0
.\release.ps1

# Run from bin\x64\Release. Each archive must contain only these files.
7z a -t7z ..\..\..\build\release\AWJ_Linux.7z AWJ -m0=lzma2 -mx=9 -mmt=1 -mf=off
7z a -t7z ..\..\..\build\release\AWJ_Win.7z AWJ.exe AWJ.com -m0=lzma2 -mx=9 -mmt=1 -mf=off
7z t ..\..\..\build\release\AWJ_Linux.7z
7z t ..\..\..\build\release\AWJ_Win.7z
```

## visual_quality GPU metrics

`--visual-quality` 1..99 repeatedly encodes candidates, decodes them from memory, and scores visual metrics. Default `--visual-quality-gpu` accelerates the metrics path only:

- Windows: Direct3D 11 compute
- Linux: Vulkan compute

Candidate encode/decode and final encoded-bytes selection remain native CPU pipelines. This is not end-to-end GPU transcoding. Failures fall back to CPU and are recorded as `cpu-fallback`.

## Large-image mode

AVIF inputs over the single-image limits (AOM 65536 edge / `2^30` pixels; SVT 16384×8704) enter the automatic large-image chain:

1. default priority is `zenrav1e`, then fall back to `grid`
2. Studio keeps that default order; CLI `--large-image-priority grid` can prefer `grid` first
3. if both paths are unavailable/fail, or the input/runtime memory cap is hit, the job fails clearly

Studio no longer has a separate large-image page; automatic large-image status stays in the main queue. Inputs above 10 MP but still under single-image limits stay in the ordinary queue tail so one large memory estimate cannot throttle every small-file worker. They still use the ordinary encoder, and batches above 12 files keep one encoder thread per file in the ordinary, deferred, and large-image stages. Grid supports smaller right/bottom edge cells for non-divisible dimensions while preserving the original output size.

`--alpha auto` retains non-opaque alpha automatically. AVIF color and alpha both use the requested quality or visual-quality result; `--chroma auto` preserves YUV 4:2:0/4:2:2/4:4:4 sources, maps RGB/RGBA to YUV 4:4:4, and uses 4:2:0 for grayscale or unknown sources while always producing YUV. q100 permits byte-stream passthrough only for a YUV 4:2:0 AVIF with no requested color, alpha, bit-depth, or metadata rewrite; all other inputs use lossless AOM quantization and those auto rules. Source CICP range is retained by default (PC/full or TV/limited); unknown range uses full. `--alpha off` removes alpha.

CLI session unlock (not written to `AWJ.jsonc`):
- `--large-image-priority zenrav1e|grid`
- `--unlock-max-input-file-bytes` / `--unlock-20gib-limit` removes the default 20 GiB input/runtime cap for the current process only; huge images may OOM.

Studio auto threading reserves 4 logical threads at >=12, 2 at 5-11, and 1 at 2-4; a single-thread system still uses 1. Automatic memory uses the smaller of 80% of total memory and 50% of currently available memory, falling back to the available source when only one is readable.

## Reproducible benchmark

The current Windows benchmark is CLI-only and uses the 613-file `D:\图片\benchmark\test` directory. It measures AWJ's default AVIF behavior and a strict ffmpeg comparison:

```powershell
pwsh -NoProfile -File .\scripts\benchmark.ps1 `
  -InputRoot 'D:\图片\benchmark\test' `
  -AwjExecutable .\bin\x64\Release\AWJ.exe `
  -FfmpegExecutable 'D:\DevTools\Cli\FFmpeg\ffmpeg.exe' `
  -PowerSchemeGuid 381b4222-f694-41f0-9685-ff5bb260df2e `
  -Mode All
```

The current protocol passes no quality, speed, chroma, bit-depth, or memory option to AWJ, so it verifies the true defaults: AOM, quality 70, speed 6, source-aware automatic chroma, automatic at-least-10-bit output, and automatic retention of non-opaque alpha at q70. The strict comparison fixes ffmpeg to AOM QP 23, 10-bit 4:2:0, all-intra, row-mt, and pixel/byte/path ordering. The runner validates actual `summary.csv` settings before recording Job Object process-tree CPU and peak memory.

The default `All` mode runs both regression and strict comparison; `Regression` runs AWJ only and `Strict` runs the 210 opaque images without ICC/EXIF/XMP against ffmpeg. Non-smoke runs use one warmup and five measured runs per scenario, nearest-rank P95, and a 30-second cooldown. Results go under `build/benchmarks/`; a fastest single run is not a version conclusion.

### 0.10.3 strict-comparison archive

The following values were recorded on 2026-07-16 with the older q80/8-bit protocol. They are historical data, not output reproducible by the current runner. The tested builds were AWJ 0.10.3 MSVC Release, commit `2798db2`, AOM 3.13.3, and ffmpeg `git-2026-07-14-312c830916`, AOM 3.14.1.

### Large-image capability

| Tool | Result | Wall | Process CPU | Sampled peak | Output |
| --- | --- | ---: | ---: | ---: | ---: |
| AWJ | AOM grid encode succeeded | 167.554 s | 892.016 s | 23,707.0 MiB | 164.39 MiB |
| ffmpeg | MJPEG decoder rejected the input before libaom | 0.246 s (time to failure) | n/a | 309.5 MiB | none |

The validated ffmpeg signature is `MJPEG packet ... too big`. Its 0.246 seconds is cached-input failure latency, not encoding throughput, so no large-image speed ratio is reported.

### 613-file results

| Tool | Schedule | Success / failure | Wall median / P95 | Process CPU median / P95 | Sampled peak median / P95 | Throughput | Median output |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| AWJ | 12 files x 1 encoder thread | 612 / 1 | 110.878 / 111.891 s | 940.047 / 956.859 s | 2810.0 / 2820.5 MiB | 5.529 img/s, 14.668 MP/s | 128.98 MiB |
| ffmpeg | 12 processes x 1 encoder thread | 612 / 1 | 107.671 / 109.772 s | 855.688 / 865.672 s | 3242.1 / 3275.8 MiB | 5.693 img/s, 15.104 MP/s | 131.48 MiB |

The five wall-time samples are 111.364, 109.919, 110.883, 111.891, and 107.727 seconds for AWJ, versus 109.772, 108.981, 106.784, 107.671, and 106.767 seconds for ffmpeg. The ffmpeg/AWJ median wall ratio is 0.97x: ffmpeg is about 2.9% faster under this machine and protocol, while AWJ's sampled median peak memory is about 13.3% lower. This is not evidence of a large AWJ speedup.

AWJ's summed `decode / prepare / encode / write` stage median/P95 values are 37.529/38.040, 0.278/0.291, 1077.766/1090.467, and 8.916/9.609 seconds. Stage values sum concurrent per-image durations and can exceed batch wall time; the multi-process ffmpeg run does not expose a comparable stage split.

This is an end-to-end CLI comparison, not a same-libaom-build microbenchmark, and it does not establish equal visual quality. The tools use different AOM versions, build options, and decode/color-conversion front ends. Fixed AWJ-then-ffmpeg order can give AWJ a lower initial temperature and ffmpeg a filesystem-cache advantage. These results describe only this machine and protocol; they do not attribute the difference to AWJ glue code or any AOM parameter.

## Studio and verification

Custom combos, buttons, navigation, and queue menus expose keyboard focus and accessibility metadata. The queue shows pending/running/success/failure counts, failed-only filtering, retry, and complete item details including paths, error text, encoder threads, and stage timings. The font list remains limited to ten visible rows with wheel and scrollbar support and no search/manual entry.

`scripts/cli-worker-smoke.ps1` launches no UI. It validates the real CLI manifest ITEM/DETAIL stream, a failed-item retry manifest, named-event cancellation, and Job Object force termination. A small headless Slint component test covers navigation, keyboard input, font scrolling, light/dark state, 820x560, and 100%/150%/200% scale. 0.10.4 Windows MSVC Release passes 31/31 tests; the 55,614,152-byte Linux GCC 16.1 Release passes 16/16 and has no dynamic `libstdc++`/`libgcc_s` dependency.

## Platform notes

| Feature | Windows | Linux |
| --- | --- | --- |
| UI + CLI single binary | `AWJ.exe` (+ optional `AWJ.com`) | `AWJ` ELF |
| visual_quality GPU | D3D11 | Vulkan |
| WIC / JXR | yes | no |
| Shell integration | registry context menu | Nautilus Scripts + Thunar UCA |
| Preferred compiler | MSVC | GCC 16 side-by-side |

Animated or multi-image WebP/GIF/APNG/JXL/TIFF/AVIF, Windows WIC inputs, and JPEG MPF inputs are flattened to their composed first frame. Extraction failures are reported instead of silently retaining extra frames.

Windows Explorer multi-selection launches are coalesced into one shell window and one queue. The shell window offers graceful cancellation and force termination; closing either UI terminates active work first.

## Docs

- Chinese migration notes: [docs/cpp-port.md](docs/cpp-port.md)
- English migration notes: [docs/cpp-port.en.md](docs/cpp-port.en.md)
- Changelog (Chinese): [CHANGELOG.md](CHANGELOG.md)
