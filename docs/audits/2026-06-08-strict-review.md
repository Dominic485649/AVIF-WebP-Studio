# AWJimage strict review - 2026-06-08

## Baseline
- Debug `AWJ` build: passed.
- Release `AWJ` build: passed.
- Initial Debug `ctest`: 23/26 passed. Failures were `avif_registry_core`, `memory_cancel_core`, and `cli_rejects_svt_444`.
- Targeted retest after fixes: `avif_registry_core`, `memory_cancel_core`, `cli_rejects_svt_444`, `native_visual_search_core`, and `visual_metrics_core` passed.
- Final Debug `ctest`: 26/26 passed.
- Release `AWJ.exe --help`: passed.

## Fixed in this pass
- Explicit `svt-av1-hdr` with non-420 chroma is now rejected in config/backend validation instead of silently forcing 420.
- Localized diagnostic tests now accept the current Chinese messages for unavailable encoders, experimental gating, over-limit checks, and malformed raw files.
- `visual_quality` now records an explanatory search trace with range, predicted quality, candidate pass/fail probes, selected candidate, and fallback state.
- The trace is propagated to native logs and appended to `summary.csv` without changing existing CSV columns.
- Direct3D visual metric shaders now support z-dimension candidate batches while preserving the single-candidate z=1 path.
- `AcceleratedVisualMetricSession` now exposes `calculate_candidate_metrics_batch(...)` with packed candidate luma, batched GMSD/MS-SSIM dispatch, batched readback, and normal error fallback semantics for callers.

## Review findings
- Performance: visual_quality remains dominated by encode/decode on small samples; the safest default speedup is adaptive probing plus fewer wasted candidates. GPU batch API is available but should be enabled in search scheduling only after a representative benchmark proves end-to-end speedup.
- Performance: Direct3D metric sessions still share a process-wide immediate-context mutex, so cross-worker GPU work is serialized. This is safer for D3D11 state reuse, but it limits scaling when many workers request GPU metrics.
- Security: output writes use temp files and atomic `MoveFileExW`, and summary CSV cells are escaped against spreadsheet formula execution. Keep new CSV fields behind the same escape path.
- Security: Explorer drag/drop uses `ChangeWindowMessageFilterEx` and `DragAcceptFiles`; this is needed for elevated windows but should stay scoped to `WM_DROPFILES`, `WM_COPYDATA`, and `WM_COPYGLOBALDATA`.
- Security: recursive scans in UI/core skip inaccessible paths via `error_code`, but junction/symlink recursion policy should be kept explicit in tests if future imports start following directory symlinks.
- Stability: cancellation is checked across decode, encode, visual search, output write, large-mode handoff, and UI worker termination. Forced termination still depends on Job Object assignment success and then falls back to `TerminateProcess`.
- Build hygiene: existing MSVC warnings remain around libpng/libjpeg `setjmp` interaction and aligned JPEG error-manager padding. These are known C-library integration warnings, not new failures.
- Build hygiene: Release still links with `/FORCE:MULTIPLE` for the static Rust runtimes from Slint and zenravif. The exe starts successfully, but `LNK4088` should remain tracked as packaging risk.

## Next performance gates
- Measure end-to-end visual_quality timings on small, medium, large, and very large inputs for WebP, JXL, JPGLI, and AVIF.
- Enable GPU batch scheduling by default only if total item time improves, not merely metric time.
- If encode/decode dominates, prefer search-window reductions and RGB/RGBA conversion caching over speculative extra candidate encoding.
