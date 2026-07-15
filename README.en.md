# AWJimage

Chinese: [README.md](README.md)

0.10.3 release notes (Chinese): [RELEASE_NOTES_0.10.3.md](RELEASE_NOTES_0.10.3.md)

AWJimage is a C++23 / Slint batch image converter. Windows and Linux now share the same mainline. The conversion path is native-only:

- AVIF: libavif/AOM, experimental `zenrav1e`, and `svt-av1-hdr`; Windows and Linux GCC Release builds link them statically.
- WebP: libwebp
- JXL: libjxl
- JPGLI: google/jpegli; produces JPEG-compatible bitstreams with the default `.jpg` extension.

The built-in ImageMagick/MagickWand backend has been removed. Magick and ffmpeg can only return later as explicit external integrations.

Linux keeps one ELF `AWJ` for both Slint UI and CLI. Visual-quality GPU metrics use Vulkan and fall back to CPU on failure, tiny images, or resource limits. WIC, JXR, `AWJ.com`, and Windows registry shell integration remain Windows-only. Linux hides WIC fallback UI and provides user-level Nautilus Scripts plus Thunar UCA actions without sudo.

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

`linux-gcc-x64-release` uses `-O3`, IPO/LTO, `-march=x86-64-v3`, section GC/strip, and statically links `libstdc++` / `libgcc`. Only `AWJ` is produced; there is no `AWJ.com`. The 0.10.3 GCC 16.1 Release ELF is 53.1 MiB.

Windows release script:

```powershell
.\release.ps1
```

Versioning is controlled by the root `VERSION` file. Release flow:

```powershell
Set-Content VERSION "0.10.3"
.\scripts\Update-VcpkgVersion.ps1
git add VERSION vcpkg.json CHANGELOG.md
git commit -m "release: 0.10.3"
git tag 0.10.3
.\release.ps1
```

## visual_quality GPU metrics

`--visual-quality` 1..99 repeatedly encodes candidates, decodes them from memory, and scores visual metrics. Default `--visual-quality-gpu` accelerates the metrics path only:

- Windows: Direct3D 11 compute
- Linux: Vulkan compute

Candidate encode/decode and final encoded-bytes selection remain native CPU pipelines. This is not end-to-end GPU transcoding. Failures fall back to CPU and are recorded as `cpu-fallback`.

## Large-image mode

AVIF inputs over the single-image limits (AOM 65536 edge / `2^30` pixels; SVT 16384×8704) enter the automatic large-image chain:

1. default priority is `zenrav1e`, then fall back to `grid`
2. parameter page “large-image priority” can prefer `grid` first
3. if both paths are unavailable/fail, or the input/runtime memory cap is hit, the job fails clearly

Studio no longer has a separate large-image page; automatic large-image status stays in the main queue. Inputs above 10 MP but still under single-image limits stay in the ordinary queue tail so one large memory estimate cannot throttle every small-file worker. They still use the ordinary encoder, and batches above 12 files keep one encoder thread per file in the ordinary, deferred, and large-image stages. Grid supports smaller right/bottom edge cells for non-divisible dimensions while preserving the original output size.

When AVIF alpha must be preserved, both color and alpha automatically use full AOM q100/4:4:4 lossless encoding while the requested `speed` remains unchanged. `--alpha off` continues to use the requested quality and speed.

CLI session unlock (not written to `AWJ.jsonc`):
- `--large-image-priority zenrav1e|grid`
- `--unlock-max-input-file-bytes` / `--unlock-20gib-limit` removes the default 20 GiB input/runtime cap for the current process only; huge images may OOM.

## Reproducible benchmark

Run the canonical Windows Release benchmark with the expected power scheme:

```powershell
& .\scripts\benchmark.ps1 -PowerSchemeGuid 381b4222-f694-41f0-9685-ff5bb260df2e -Surface cli
```

The fixed AVIF/AOM profile is quality 80, speed 6, 4:2:0, and 8-bit. Each case has one warmup and five measured runs. Mixed inputs cover 1, 4, 12, 13, and 613 files; opaque and transparent inputs cover 1 and 13 files. The 0.10.3 validation uses CLI only: Studio manifest and shell modes are policies of the same CLI worker and do not require window automation. Results and the content-addressed input manifest are written under `build/benchmarks/`.

`Process CPU` is Windows process CPU time. `Item seconds sum` is the sum of per-image `summary.csv` durations retained only for comparison with historical numbers; it is not batch wall time or process CPU time. Canonical runs require clean source files (tracked Release artifacts rebuilt by `release.ps1` may differ), a matching Release `BUILD_INFO.txt`, and an explicit power scheme. `-Smoke` validates the runner only.

The fixed 613-file CLI case used the same input fingerprint, AOM 3.13.3, build profile, power scheme, and parameters in both versions:

The earlier 1367.171 and 1660.888 core-second records used different chroma/protocol settings, so they are neither directly comparable nor the baseline for this table.

| Metric | 0.10.2 `ec61551` | 0.10.3 `2798db2` |
| --- | ---: | ---: |
| Wall median / P95 | 214.361 / 226.493 s | 162.660 / 166.395 s |
| Process CPU median / P95 | 1613.906 / 1624.469 s | 1381.844 / 1399.391 s |
| Peak memory median / P95 | invalid (stale cached sample) | 2807.7 / 2822.4 MiB |
| Median throughput | 2.860 img/s | 3.769 img/s |
| decode / prepare / encode / write median | 51.107 / 0.346 / 2189.732 / 15.396 s | 40.749 / 0.314 / 1668.552 / 10.198 s |

The five 0.10.3 wall runs are 158.326, 166.395, 162.366, 162.660, and 163.967 seconds. The old 0.10.2 peak-memory value came from the first cached `Process` sample and is not comparable; 0.10.3 refreshes the process data before each sample. Wall, CPU, and stage timings were unaffected. No CLI encoder code, AOM tuning, quality, speed, chroma, bit-depth, or thread rule changed in 0.10.3, so the faster median is an observation, not a claimed UI optimization. The strict regression did not reproduce; ETW/perf was not run.

| 0.10.3 CLI case | Wall median / P95 (s) | CPU median / P95 (s) | Peak median / P95 (MiB) | Median throughput (img/s) |
| --- | ---: | ---: | ---: | ---: |
| mixed-1 | 1.577 / 3.648 | 2.656 / 3.703 | 104.7 / 105.0 | 0.634 |
| mixed-4 | 1.804 / 2.447 | 8.094 / 8.625 | 317.2 / 321.5 | 2.217 |
| mixed-12 | 6.295 / 6.600 | 25.406 / 26.031 | 826.1 / 860.8 | 1.906 |
| mixed-13 | 11.004 / 12.692 | 30.312 / 33.734 | 786.6 / 801.8 | 1.181 |
| opaque-1 | 0.605 / 0.635 | 1.875 / 2.000 | 119.9 / 120.3 | 1.653 |
| transparent-1 | 1.150 / 1.249 | 1.969 / 2.141 | 121.7 / 121.8 | 0.870 |
| opaque-13 | 5.887 / 6.433 | 26.500 / 27.531 | 897.0 / 918.3 | 2.208 |
| transparent-13 | 6.733 / 7.040 | 27.734 / 28.250 | 882.9 / 902.0 | 1.931 |

## Studio and verification

0.10.3 adds keyboard focus and accessibility metadata to custom combos, buttons, navigation, help, and queue menus. The queue shows pending/running/success/failure counts, failed-only filtering, retry, and complete item details including paths, error text, encoder threads, and stage timings. The font list remains limited to ten visible rows with wheel and scrollbar support and no search/manual entry.

`scripts/cli-worker-smoke.ps1` launches no UI. It validates the real CLI manifest ITEM/DETAIL stream, a failed-item retry manifest, named-event cancellation, and Job Object force termination. A small headless Slint component test covers navigation, keyboard input, font scrolling, light/dark state, 820x560, and 100%/150%/200% scale. Windows MSVC Release passes 31/31 tests; Linux GCC 16.1 Release passes 16/16 and has no dynamic `libstdc++`/`libgcc_s` dependency.

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
