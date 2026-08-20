# AWJ 1.0.4 / 1.0.5 release

Keep every build, staging directory, archive, and old-version sample under repository `build/` or `bin/`; never create release files at the `D:/` root. Keep the Windows 1.0.3 sample only in `bin/old-versions/1.0.3/` and retain no later old version unless explicitly requested.

## 1.0.4 prerelease

Run Windows Release CTest plus Studio/CLI smoke, then ordinary WSL Linux Release build, CTest, CLI, ELF, and archive checks. Do not run updater end-to-end tests in WSL or a VM. Merge directly to `master`, tag a clean 1.0.4 commit, and build both platforms from that tag's locked dependencies.

Run `scripts/package-release.ps1` with the three platform binary paths and `-SkipManifests`. It writes only `build/release/1.0.4/`, runs `7z t`, extracts into a new directory, verifies every file SHA-256, and runs Windows CLI `--help`. Archives contain the binary, checksum file, `LICENSE`, `THIRD_PARTY_NOTICES.txt`, `THIRD_PARTY_LICENSES/`, and `BUILD_INFO.txt`; run Linux `AWJ --help` and `readelf -d` in the normal WSL validation. Publish a prerelease with exactly `AWJ_Win.7z` and `AWJ_Linux.7z`, verify their public downloads, then rerun the script with `-ArchiveManifestSequence`, UTC time, and the external Ed25519 seed/public key. Commit and push only the resulting signed `update-manifest-v2.json` and `.sig` after the archive assets are public.

The v2 manifest has its own sequence and only archive assets. Each required member binds to the archive URL, size, and SHA-256; clients do not fall back to raw assets.

## 1.0.5 bridge prerelease

Create 1.0.5 from the locked 1.0.4 source, changing only version, changelog, and release notes. Validate the same build and archives, then publish exactly `AWJ_Win.7z`, `AWJ_Linux.7z`, `AWJ.exe`, and `AWJ.com`. Its notes must state that functionality is identical to 1.0.4, it exists only for the 1.0.3 updater bridge test, and normal users should download 1.0.4.

After those assets are public, run the packager with `-BridgeRelease` and a strictly higher `-LegacyManifestSequence`. This writes only the legacy v1 1.0.5 entry, not a v2 candidate. On Windows, use an isolated copy of `bin/old-versions/1.0.3/` to validate the local 1.0.3→1.0.5 download, verification, install, health check, and rollback. Stop after it passes; do not create or publish 1.0.6 without explicit new authorization.
