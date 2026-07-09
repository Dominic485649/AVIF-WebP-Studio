# C++23 Port Notes

Chinese: [cpp-port.md](cpp-port.md)

## Goal

AWJimage is a C++23 / native-codec / Slint batch converter. Windows and Linux share the same mainline. Platform-specific code is isolated with `WIN32` / POSIX guards while config, pipeline, codecs, and most Studio logic stay shared.

## Backends

Native codecs only:

- AVIF: libavif/AOM; experimental `zenrav1e` on Windows; SVT-AV1-HDR available on Windows and Linux when enabled
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

## Linux first-class support

- One ELF `AWJ` for UI and CLI
- GCC 16 side-by-side preferred; Release: `-O3` + LTO + `x86-64-v3` + static `libstdc++`/`libgcc`
- No `AWJ.com`, no WIC UI, no JXR
- File picker via zenity/yad/kdialog when available
- User-level Nautilus Scripts and Thunar UCA context actions

## Large-image mode (0.10.0)

- Oversized AVIF inputs use an automatic chain: preferred path then one fallback (`zenrav1e` default priority, or `grid` first).
- Studio large-image page still shows status and can force one path for a selected item.
- Session-only unlock removes the default 20 GiB input/runtime cap; it is never written to `AWJ.jsonc`.
- Grid planning that requires unsupported padding fails with an actionable error.
