# AWJimage

Chinese: [README.md](README.md)

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

`linux-gcc-x64-release` uses `-O3`, IPO/LTO, `-march=x86-64-v3`, section GC/strip, and statically links `libstdc++` / `libgcc`. Only `AWJ` is produced; there is no `AWJ.com`.

Windows release script:

```powershell
.\release.ps1
```

Versioning is controlled by the root `VERSION` file. Release flow:

```powershell
Set-Content VERSION "0.10.2"
.\scripts\Update-VcpkgVersion.ps1
git add VERSION vcpkg.json CHANGELOG.md
git commit -m "release: 0.10.2"
git tag 0.10.2
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
pwsh .\scripts\benchmark.ps1 -PowerSchemeGuid 381b4222-f694-41f0-9685-ff5bb260df2e
```

The fixed AVIF/AOM profile is quality 80, speed 6, 4:2:0, and 8-bit. Each case has one warmup and five measured runs. Mixed inputs cover 1, 4, 12, 13, and 613 files; opaque and transparent inputs cover 1 and 13 files; CLI, the Studio manifest worker, and the shell conversion policy are measured separately. Results and the content-addressed input manifest are written under `build/benchmarks/`.

`Process CPU` is Windows process CPU time. `Item seconds sum` is the sum of per-image `summary.csv` durations retained only for comparison with historical numbers; it is not batch wall time or process CPU time. Canonical runs require clean source files (tracked Release artifacts rebuilt by `release.ps1` may differ), a matching Release `BUILD_INFO.txt`, and an explicit power scheme. `-Smoke` validates the runner only.

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
