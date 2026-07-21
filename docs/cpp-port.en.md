# C++23 Port Notes

Chinese: [cpp-port.md](cpp-port.md)

## Goal

AWJimage is a C++23 / native-codec / Slint batch converter. Windows and Linux share the same mainline. Platform-specific code is isolated with `WIN32` / POSIX guards while config, pipeline, codecs, and most Studio logic stay shared.

## Backends

Native codecs only:

- AVIF: libavif/AOM, experimental `zenrav1e`, and SVT-AV1-HDR on Windows and Linux Release builds
- WebP: libwebp
- JXL: libjxl
- JPGLI: google/jpegli (JPEG-compatible `.jpg` output)
- PNG/JPEG/TIFF/GIF/BMP/RAW as decode inputs
- WIC/JXR only on Windows

## Pipeline

1. Decoder registry selects native decoder; Windows may also use WIC/JXR.
2. Decode returns pixels plus ICC/EXIF/XMP/color/HDR/alpha metadata.
3. Optional resize, alpha policy, chroma, bit-depth, and visual-quality search run.
4. Visual-quality metrics prefer GPU: D3D11 on Windows, Vulkan on Linux; CPU fallback otherwise.
5. AVIF auto only picks stable encoders; experimental encoders require explicit selection.
6. Oversized AVIF inputs enter the automatic large-image chain (default `zenrav1e` then `grid`); 10 MP+ under single-image limits are deferred to the ordinary queue tail.
7. Manual large-image force actions remain exclusive for a single item; auto priority may fall back once.
8. Encoders write metadata when supported; unsupported combinations fail clearly.
9. Outputs go through temp files and collision policy before the final path.
10. Logs and `summary.csv` record backend, quality, fallback, and GPU metric path.

## AVIF defaults (0.10.4)

- `auto` always writes YUV 4:2:0; 4:2:2 and 4:4:4 require explicit chroma settings, and AVIF output never switches to RGB.
- Non-opaque alpha is retained by `--alpha auto`; color and alpha both follow the requested quality or visual-quality result rather than forcing q100.
- Source CICP range is preserved as PC/full or TV/limited unless overridden. Unknown range uses full.
- q100 permits byte-stream passthrough only for a YUV 4:2:0 AVIF with no requested color, alpha, bit-depth, or metadata rewrite. All other inputs use lossless AOM quantization and default 4:2:0, which can still change pixels during RGB/YUV or chroma conversion; use explicit 4:4:4 when chroma subsampling is unacceptable.

Studio uses the same CLI pipeline in a child process. Its versioned ITEM/DETAIL stream reports item state, encoder, thread count, and decode/prepare/encode/write timing; retry runs use a compact run index that maps back to the original queue row.

## Linux first-class support

- One ELF `AWJ` for UI and CLI
- GCC 16 side-by-side preferred; Release: `-O3` + LTO + `x86-64-v3` + static `libstdc++`/`libgcc`
- No `AWJ.com`, no WIC UI, no JXR
- File picker via zenity/yad/kdialog when available
- User-level Nautilus Scripts and Thunar UCA context actions

## Studio accessibility and queue diagnostics (0.10.3)

- Custom combos, buttons, navigation, and queue menus expose focus, keyboard behavior, accessible roles, names, values, and states.
- The font popup keeps at most ten visible rows with wheel and scrollbar support, without search or manual input.
- Parameters are grouped into common, resource, and advanced-format sections. Dangerous warnings remain visible.
- Queue counts, failed-only filtering, retry, and item details expose complete errors, paths, encoder threads, and stage timings outside the elided table.
- A headless Slint component smoke covers 820x560, 100%/150%/200% scale, long text, navigation, keyboard, scrolling, and theme state. Windows worker cancellation and Job Object force termination are tested directly through the CLI, without UI Automation.

0.10.4 Windows MSVC Release passes 31/31 tests. The 55,614,152-byte Linux GCC 16.1 Release passes 16/16; its ELF retains `-O3`, LTO, `x86-64-v3`, static `libstdc++`/`libgcc`, and no dynamic `libstdc++.so.6` or `libgcc_s.so.1` dependency.

## GitHub release archives (0.10.4)

Create the archives from `bin/x64/Release` with the platform files kept separate: `AWJ_Linux.7z` contains only the ELF `AWJ`, while `AWJ_Win.7z` contains only `AWJ.exe` and `AWJ.com`. Use 7-Zip with `-t7z -m0=lzma2 -mx=9 -mmt=1 -mf=off`; `-mf=off` prevents the automatic BCJ2 filter for `.exe` files, keeping every data block LZMA2. Before upload, validate the exact listing and method with `7z l -slt`, then test integrity with `7z t`.

0.10.4 is published at the [GitHub Release](https://github.com/Dominic485649/AWJimage/releases/tag/0.10.4). The exact archive SHA-256 values are recorded in the Release body.

## Large-image handling (0.10.1)

- Oversized AVIF inputs use an automatic chain: preferred path then one fallback (`zenrav1e` default priority, or `grid` first).
- Studio has no separate large-image page; automatic handling remains visible in the main queue.
- Session-only unlock removes the default 20 GiB input/runtime cap; it is never written to `AWJ.jsonc`.
- Non-divisible grids use smaller right/bottom edge cells and preserve the original dimensions; an incompatible default 420 grid fails clearly instead of silently changing to 444.
