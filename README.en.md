# AWJimage

Chinese: [README.md](README.md)

AWJimage is a C++23 / Slint batch image converter. Windows and Linux now share the same mainline. The conversion path is native-only:

- AVIF: libavif/AOM; Windows builds can statically link experimental `zenrav1e` and `svt-av1-hdr`. Linux GCC builds enable AOM and `svt-av1-hdr`; `zenrav1e` remains Windows experimental for now.
- WebP: libwebp
- JXL: libjxl
- JPGLI: google/jpegli; produces JPEG-compatible bitstreams with the default `.jpg` extension.

The built-in ImageMagick/MagickWand backend has been removed. Magick and ffmpeg can only return later as explicit external integrations.

Linux keeps one ELF `AWJ` for both Slint UI and CLI. Visual-quality GPU metrics use Vulkan and fall back to CPU on failure, tiny images, or resource limits. WIC, JXR, `AWJ.com`, and Windows registry shell integration remain Windows-only. Linux hides WIC fallback UI and provides user-level Nautilus Scripts plus Thunar UCA actions without sudo.

## Build

Windows (MSVC):

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ
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
Set-Content VERSION "0.10.0"
.\scripts\Update-VcpkgVersion.ps1
git add VERSION vcpkg.json CHANGELOG.md
git commit -m "release: 0.10.0"
git tag 0.10.0
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

Studio large-image page can still force a single path for one item; forced manual selection never silently switches. Inputs above 10 MP but still under single-image limits stay in the ordinary queue tail.

CLI session unlock (not written to `AWJ.jsonc`):
- `--large-image-priority zenrav1e|grid`
- `--unlock-max-input-file-bytes` / `--unlock-20gib-limit` removes the default 20 GiB input/runtime cap for the current process only; huge images may OOM.

## Platform notes

| Feature | Windows | Linux |
| --- | --- | --- |
| UI + CLI single binary | `AWJ.exe` (+ optional `AWJ.com`) | `AWJ` ELF |
| visual_quality GPU | D3D11 | Vulkan |
| WIC / JXR | yes | no |
| Shell integration | registry context menu | Nautilus Scripts + Thunar UCA |
| Preferred compiler | MSVC | GCC 16 side-by-side |

## Docs

- Chinese migration notes: [docs/cpp-port.md](docs/cpp-port.md)
- English migration notes: [docs/cpp-port.en.md](docs/cpp-port.en.md)
- Changelog (Chinese): [CHANGELOG.md](CHANGELOG.md)
