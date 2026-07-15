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

Studio uses the same CLI pipeline in a child process. Its versioned ITEM/DETAIL stream reports item state, encoder, thread count, and decode/prepare/encode/write timing; retry runs use a compact run index that maps back to the original queue row.

## Linux first-class support

- One ELF `AWJ` for UI and CLI
- GCC 16 side-by-side preferred; Release: `-O3` + LTO + `x86-64-v3` + static `libstdc++`/`libgcc`
- No `AWJ.com`, no WIC UI, no JXR
- File picker via zenity/yad/kdialog when available
- User-level Nautilus Scripts and Thunar UCA context actions

## Studio accessibility and queue diagnostics (0.10.3)

- Custom combos, buttons, navigation, help controls, and queue menus expose focus, keyboard behavior, accessible roles, names, values, and states.
- The font popup keeps at most ten visible rows with wheel and scrollbar support, without search or manual input.
- Parameters are grouped into common, resource, and advanced-format sections. Dangerous warnings remain visible; long routine help moves to tooltips.
- Queue counts, failed-only filtering, retry, and item details expose complete errors, paths, encoder threads, and stage timings outside the elided table.
- A headless Slint component smoke covers 820x560, 100%/150%/200% scale, long text, navigation, keyboard, scrolling, and theme state. Windows worker cancellation and Job Object force termination are tested directly through the CLI, without UI Automation.

Windows MSVC Release passes 31/31 tests. Linux GCC 16.1 Release passes 16/16; its 53.1 MiB ELF retains `-O3`, LTO, `x86-64-v3`, static `libstdc++`/`libgcc`, and no dynamic `libstdc++.so.6` or `libgcc_s.so.1` dependency.

## GitHub release archives (0.10.3)

Create the archives from `bin/x64/Release` with the platform files kept separate: `AWJ_Linux.7z` contains only the ELF `AWJ`, while `AWJ_Win.7z` contains only `AWJ.exe` and `AWJ.com`. Use 7-Zip with `-t7z -m0=lzma2 -mx=9 -mmt=1 -mf=off`; `-mf=off` prevents the automatic BCJ2 filter for `.exe` files, keeping every data block LZMA2. Before upload, validate the exact listing and method with `7z l -slt`, then test integrity with `7z t`.

0.10.3 is published at the [GitHub Release](https://github.com/Dominic485649/AWJimage/releases/tag/0.10.3). `AWJ_Linux.7z` SHA-256: `d7efc2f4ece5fdf3876cad480fa74b7848d00deeda4398bc26f11cdc7b69377c`; `AWJ_Win.7z` SHA-256: `108883cf75185255b68b390b7c2c5f9567811b8e180b66a12b394cd7d5243fae`.

## Large-image handling (0.10.1)

- Oversized AVIF inputs use an automatic chain: preferred path then one fallback (`zenrav1e` default priority, or `grid` first).
- Studio has no separate large-image page; automatic handling remains visible in the main queue.
- Session-only unlock removes the default 20 GiB input/runtime cap; it is never written to `AWJ.jsonc`.
- Non-divisible grids use smaller right/bottom edge cells and preserve the original dimensions; incompatible explicit 420/422 requests fail clearly.
