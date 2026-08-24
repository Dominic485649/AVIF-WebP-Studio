#!/usr/bin/env bash
set -euo pipefail

die() { printf '%s\n' "$*" >&2; exit 1; }

binary=""
output_dir=""
while (($#)); do
  case "$1" in
    --binary) binary=${2-}; shift 2 ;;
    --output-dir) output_dir=${2-}; shift 2 ;;
    -h|--help)
      printf 'usage: %s --binary /native/path/AWJ [--output-dir /native/path/build/release-linux/VERSION]\n' "$0"
      exit 0
      ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ -n "$binary" ]] || die '--binary is required'
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(CDPATH= cd -- "$script_dir/.." && pwd)
[[ "$binary" = /* ]] || binary="$PWD/$binary"
binary=$(realpath -e -- "$binary")
[[ -x "$binary" && $(basename -- "$binary") == AWJ ]] || die '--binary must name an executable AWJ'

version=$(tr -d '\r\n' < "$repo/VERSION")
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die 'VERSION is invalid'
[[ -z $(git -C "$repo" status --porcelain --untracked-files=no) ]] || die 'tracked source changes are not allowed'
[[ $(git -C "$repo" describe --exact-match --tags HEAD 2>/dev/null || true) == "$version" ]] || die 'HEAD must be the matching release tag'
command -v 7z >/dev/null || die '7z is required'

if [[ -z "$output_dir" ]]; then output_dir="$repo/build/release-linux/$version"; fi
[[ "$output_dir" = /* ]] || output_dir="$repo/$output_dir"
output_dir=$(realpath -m -- "$output_dir")
case "$output_dir" in "$repo"/build/*) ;; *) die '--output-dir must stay below repo/build' ;; esac
[[ ! -e "$output_dir" ]] || die "refusing to overwrite existing output: $output_dir"

cmake_value() {
  sed -nE "s/^set\\($1 \"([^\"]+)\".*/\\1/p" "$repo/CMakeLists.txt" | head -n 1
}

baseline=$(sed -nE 's/.*"baseline"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' "$repo/vcpkg-configuration.json" | head -n 1)
svt_commit=$(cmake_value AWJ_SVTAV1HDR_GIT_TAG)
libavif_commit=$(cmake_value AWJ_LIBAVIF_GIT_TAG)
jpegli_commit=$(cmake_value AWJ_JPEGLI_GIT_TAG)
slint_commit=$(cmake_value AWJ_SLINT_GIT_TAG)
[[ -n "$baseline$svt_commit$libavif_commit$jpegli_commit$slint_commit" ]] || die 'could not read build pins'

package="$output_dir/package/AWJ_Linux"
archive="$output_dir/AWJ_Linux.7z"
mkdir -p "$package/THIRD_PARTY_LICENSES"
install -m 0755 "$binary" "$package/AWJ"
printf '%s  AWJ\n' "$(sha256sum "$package/AWJ" | awk '{print $1}')" > "$package/AWJ.sha256"
cp -- "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.txt" "$package/"
cp -- "$repo/licenses/libplacebo-LGPL-2.1-or-later.txt" "$package/THIRD_PARTY_LICENSES/"
printf '%s\n' \
  "AWJimage $version" \
  'Build Type: Release' \
  "Git Commit: $(git -C "$repo" rev-parse HEAD)" \
  'Architecture: x64' \
  'Platform: Linux' \
  "Vcpkg baseline: $baseline" \
  "SVT-AV1-HDR commit: $svt_commit" \
  "libavif commit: $libavif_commit" \
  "Jpegli commit: $jpegli_commit" \
  "Slint commit: $slint_commit" \
  'libplacebo: v7.360.1' \
  'libarchive: v3.8.9' \
  'Source: https://github.com/Dominic485649/AWJimage' > "$package/BUILD_INFO.txt"

pushd "$package" >/dev/null
7z a -t7z "$archive" ./* -m0=lzma2 -mx=9 -mmt=1 -mf=off -mtc=off -mta=off -mtm=off >/dev/null
popd >/dev/null
7z t "$archive" >/dev/null

verify_dir=$(mktemp -d "$output_dir/verify.XXXXXX")
trap 'rm -rf -- "$verify_dir"' EXIT
7z x "$archive" "-o$verify_dir" -y >/dev/null
[[ -x "$verify_dir/AWJ" ]] || die 'archive did not retain AWJ executable mode'
while IFS= read -r -d '' source; do
  relative=${source#"$package/"}
  target="$verify_dir/$relative"
  [[ -f "$target" ]] || die "archive omitted $relative"
  [[ $(sha256sum "$source" | awk '{print $1}') == $(sha256sum "$target" | awk '{print $1}') ]] || die "archive hash mismatch: $relative"
done < <(find "$package" -type f -print0)
[[ $(find "$package" -type f | wc -l) == $(find "$verify_dir" -type f | wc -l) ]] || die 'archive contains an unexpected file'
"$verify_dir/AWJ" --help >/dev/null

printf 'Linux package: %s\nLinux archive: %s\n' "$package" "$archive"
