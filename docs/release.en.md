# AWJ 1.0.4 / 1.0.5 release

Keep every build, staging directory, archive, and old-version sample under repository `build/` or `bin/`; never create release files at the `D:/` root. Keep the Windows 1.0.3 sample only in `bin/old-versions/1.0.3/` and retain no later old version unless explicitly requested.

Frozen 1.0.4/1.0.5 publication must check out the `1.0.4` tag and use the scripts frozen with that tag; do not backport the 1.0.6 keyring/expiry policy into either bridge release.

## 1.0.4 prerelease

Run Windows Release CTest plus Studio/CLI smoke, then ordinary WSL Linux Release build, CTest, CLI, ELF, and archive checks. Do not run updater end-to-end tests in WSL or a VM. Merge directly to `master`, tag a clean 1.0.4 commit, and build both platforms from that tag's locked dependencies.

Run `scripts/package-release.ps1` exactly once with the three platform binary paths, `-ArchiveManifestSequence`, UTC time, and the external Ed25519 seed/public key. It writes only `build/release/1.0.4/`, runs `7z t`, extracts into a new directory, verifies every file SHA-256, runs Windows CLI `--help`, and stages the signed v2 manifest. Archives contain the binary, checksum file, `LICENSE`, `THIRD_PARTY_NOTICES.txt`, `THIRD_PARTY_LICENSES/`, and `BUILD_INFO.txt`; run Linux `AWJ --help` and `readelf -d` in the normal WSL validation. Publish exactly the resulting `AWJ_Win.7z` and `AWJ_Linux.7z`, verify their public downloads, then commit and push only the already-generated signed `update-manifest-v2.json` and `.sig`. Do not rerun packaging solely to sign the manifest: archive member timestamps could otherwise change its hash.

The v2 manifest has its own sequence and only archive assets. Each required member binds to the archive URL, size, and SHA-256; clients do not fall back to raw assets.

## 1.0.5 bridge prerelease

Create 1.0.5 from the locked 1.0.4 source, changing only version, changelog, and release notes. Validate the same build and archives, then publish exactly `AWJ_Win.7z`, `AWJ_Linux.7z`, `AWJ.exe`, and `AWJ.com`. Its notes must state that functionality is identical to 1.0.4, it exists only for the 1.0.3 updater bridge test, and normal users should download 1.0.4.

Before creating the immutable 1.0.5 Release, run the packager once with `-BridgeRelease` and a strictly higher `-LegacyManifestSequence`, then upload the four assets it produced. After public-download validation, commit only the already-generated legacy v1 1.0.5 entry; it is not a v2 candidate. On Windows, use an isolated copy of `bin/old-versions/1.0.3/` to validate the local 1.0.3→1.0.5 download, verification, install, health check, and rollback. Stop after it passes. 1.0.6 has separate authorization and follows the next section; updater E2E remains prohibited in a VM or WSL.

## 1.0.6 security prerelease

1. Finish frozen 1.0.4/1.0.5 assets, public-download validation, and the local bridge test first; 1.0.6 must not rewrite either tag or asset.
2. Keep the seed outside the repository at `C:\Users\ROG\Documents\AWJimage-secrets\update-ed25519-seed.hex`. Do not read, print, commit, or copy it; legacy, recovery-root, and release seeds all need independent encrypted offline backups.
3. Have two roots sign `update-keyring-v1.json`, confirm its sequence increases strictly and its `expires_at` is at most 180 days out, and commit the `.sig` before publishing an update manifest. See [update signing and key rotation](update-security.en.md).
4. From a clean 1.0.6 tag, run `scripts/package-release.ps1` once to build and archive `AWJ_Win.7z` and `AWJ_Linux.7z`, with explicit `-ManifestKeyId`, `-ManifestExpiresAtUtc`, matching release public key/seed, and the preceding manifest public key when it differs. It stages the v1/v2 manifests before the release. Upload the resulting assets; after public-download validation, commit only those already-generated manifests rather than rerunning packaging. The script rejects a signer outside the non-revoked keyring entry.
5. GitHub Immutable Releases is enabled. Verify tag, prerelease state, asset names, sizes, and hashes before creation; publishing first and correcting later is not an option.
