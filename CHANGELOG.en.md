# Changelog

## 1.0.3 - 2026-08-19

- Prerelease: fixed the lifetime of the current user's Windows proxy configuration during update requests, keeping manual proxy and PAC/auto-detected results valid for the whole request and safely falling back to direct routing when discovery is unavailable.
- Prerelease: fixed update-channel focus after switching, keeping one blue focus ring the same width as the combo box instead of leaving a black border behind.

## 1.0.2 - 2026-08-10

- Prerelease: when automatic proxy discovery (WPAD/PAC) is unavailable, fall back to WinHTTP's default route instead of rejecting a direct connection; an explicitly configured manual proxy still takes precedence.
- Prerelease: Windows update requests now honor the current user's manual proxy, PAC, and auto-detect settings; when the user profile is unavailable, WinHTTP's default proxy remains in effect and proxy addresses are never logged.
- Prerelease: the update-channel combo keeps focus while automatic checking disables interaction, so focus no longer jumps to the “Show changelog” checkbox or paints a black focus border.
- Prerelease: fixed Windows updates when LocalAppData staging and the installed application are on different volumes; the helper now copies to a same-volume temporary and atomically replaces the target while preserving rollback.
- Prerelease: fixed release-script parsing when continuing an existing manifest so sequence/version validation does not treat the field label as a version component.
- Prerelease: isolated all test executables under `bin/x64/tests/<configuration>` so cross-platform builds and release cleanup cannot overwrite test artifacts.
- Prerelease: changed the changelog page to show the complete version-sorted release history from the signed manifest; removed manual checking so update checks remain automatic.
- Prerelease: embedded the bilingual major-version history in the changelog page and merged it with verified manifest entries; removed explanatory and empty-state text.
- Prerelease: atomically persist the latest verified manifest bytes and signature with the existing config, then re-verify them at startup so the complete history remains available offline between automatic checks.

## 1.0.1 - 2026-08-09

- Added the signed update foundation. The client accepts only a static `update-manifest.json` whose original deterministic UTF-8 bytes verify against the embedded Ed25519 public key. A monotonic sequence rejects replay, versions use strict integer MAJOR.MINOR.PATCH comparison, and stable/prerelease filtering is explicit.
- Added a clickable version label, persistent update dot, and a first-class changelog page beside Queue, Parameters, and Settings. Settings now expose the update channel, changelog visibility, automatic-check status, and the last successful check time. Hovering, opening the page, navigating, restarting, or a failed check never clears the dot.
- Windows checks and downloads run on a cancellable worker with WinHTTP timeouts, response limits, and an exact host allowlist. Installation re-fetches and re-verifies the manifest, then verifies each asset's declared size and SHA-256. Updates are blocked while encoding; unwritable installs open the Release page without requesting elevation.
- The Windows helper derives the install directory from its parent process handle and can replace only sibling `AWJ.exe` and `AWJ.com`. It backs up both files, journals the transaction, replaces the pair, launches the new build, and waits for a health signal. Missing health, early exit, or any replacement failure restores the full old pair; the next launch can also recover an interrupted transaction.
- Unified update-state and normal Studio config commits on the UI thread. Windows config writes now use a same-directory temporary file, `FlushFileBuffers`, and atomic replacement. Integer config storage is 64-bit, avoiding the Unix timestamp overflow in 2038.
- Corrected AVIF q100/visual-quality 100, CICP, and clamped-grid help and documentation to match source-aware automatic chroma, HDR metadata precedence, and smaller edge cells. Benchmark result paths, titles, and profiles now derive their version from `VERSION`.
- Corrected public Windows/Linux build and packaging instructions. Archives include `LICENSE`, `THIRD_PARTY_NOTICES.txt`, and `BUILD_INFO.txt` and are followed by an archive-list check. Portable Linux presets are separated from maintainer-only compiler-wrapper presets.
- Corrected the pinned `zenravif 0.1.3` AGPL-3.0-only/commercial dual-license notice and documented the updater's libsodium and nlohmann JSON dependencies. The release script now requires bilingual changelog entries, a deterministic manifest, an explicit sequence, a signing seed, and a matching embedded public key.

## 1.0.0 - 2026-07-30

- Added instant Chinese/English Studio switching with translations bundled into the executable and persisted on Windows.
- Reworked parameter-help layout and narrow-window wrapping, fixed C++ module dependency declarations, and hardened GUI crash diagnostics and worker exception boundaries.
- Updated the dependency baseline and pinned codec/UI sources, fixed the upstream `svt-av1-hdr` chroma scaling assignment during configure, and aligned benchmark assumptions with source-aware AVIF chroma.
- Corrected automatic memory budgeting, removed the obsolete context-menu preset selector, and expanded regression coverage for defaults and UI behavior.
