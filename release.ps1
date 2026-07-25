param(
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "",
    [switch]$StaticRuntime,
    [switch]$DynamicRuntime,
    [switch]$SharedSlint,
    [switch]$NoVcpkgInstall,
    [switch]$EnableLto,
    [switch]$CleanDependencies,
    [string]$DependencyCacheRoot = "",
    [switch]$DisableZenravif,
    [switch]$DisableSvtAv1Hdr
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSCommandPath
# FetchContent sources are public HTTPS repositories.  Keep a developer's
# global URL rewrite from turning them into an interactive SSH fetch.
$env:GIT_CONFIG_GLOBAL = "NUL"
$env:GIT_CONFIG_NOSYSTEM = "1"
$env:GIT_TERMINAL_PROMPT = "0"
$env:GIT_CONFIG_COUNT = "1"
$env:GIT_CONFIG_KEY_0 = "http.version"
$env:GIT_CONFIG_VALUE_0 = "HTTP/1.1"
$BuildDir = Join-Path $Repo "build\x64\Release"
$OutputDir = Join-Path $Repo "bin\x64\Release"
$OverlayPorts = Join-Path $Repo "vcpkg-overlays"
$ReleaseFiles = @("AWJ.exe", "AWJ.com", "AWJ.exe.sha256", "AWJ.com.sha256", "AWJ", "AWJ.sha256", "LICENSE", "THIRD_PARTY_NOTICES.txt", "BUILD_INFO.txt")
$Version = (Get-Content (Join-Path $Repo "VERSION") -Raw).Trim()
$VcpkgConfiguration = Get-Content (Join-Path $Repo "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$VcpkgBaseline = $VcpkgConfiguration.'default-registry'.baseline

function Remove-RepoDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $ResolvedRepo = [IO.Path]::GetFullPath($Repo).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $ResolvedPath = [IO.Path]::GetFullPath($Path)
    if (-not $ResolvedPath.StartsWith($ResolvedRepo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理仓库外目录: $ResolvedPath"
    }
    Remove-Item -LiteralPath $ResolvedPath -Recurse -Force
}

if (-not $DependencyCacheRoot) {
    $DependencyCacheRoot = Join-Path $Repo "build\dependency-cache\$Version"
}
if ($CleanDependencies) {
    Remove-RepoDirectory $BuildDir
    Remove-RepoDirectory (Join-Path $Repo "vcpkg_installed")
    Remove-RepoDirectory $DependencyCacheRoot
}
$DependencyBinaryCache = Join-Path $DependencyCacheRoot "binary-cache"
$DependencyBuildtrees = Join-Path $DependencyCacheRoot "buildtrees"
$DependencyPackages = Join-Path $DependencyCacheRoot "packages"
New-Item -ItemType Directory -Path $DependencyBinaryCache, $DependencyBuildtrees, $DependencyPackages -Force | Out-Null
$env:VCPKG_DEFAULT_BINARY_CACHE = $DependencyBinaryCache
$GitCommit = (git -C $Repo rev-parse HEAD).Trim()
$GitStatus = @(git -C $Repo status --porcelain=v1 --untracked-files=all)
$GitDirty = @($GitStatus | Where-Object {
    $Path = $_.Substring(3).Trim('"').Replace('\', '/')
    -not ($Path.StartsWith('bin/x64/Release/') -and
          [IO.Path]::GetFileName($Path) -in $ReleaseFiles)
}).Count -gt 0
try {
    $GitTag = (git -C $Repo describe --tags --exact-match HEAD 2>$null).Trim()
} catch {
    $GitTag = ""
}
if ($GitDirty) {
    $GitCommit += "-dirty"
    $GitTag = ""
}
elseif ($GitTag -ne $Version) {
    $GitTag = ""
}

if (-not $VcpkgRoot) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    }
    elseif (Test-Path "D:\Scoop\apps\vcpkg\current") {
        $VcpkgRoot = "D:\Scoop\apps\vcpkg\current"
    }
}

function Ensure-VcpkgManifestPackages([string]$Root, [string]$Triplet, [string[]]$PackageShareNames, [bool]$NoInstall) {
    if (-not $Root) {
        throw "未找到 vcpkg。请设置 VCPKG_ROOT，或传入 -VcpkgRoot。"
    }

    $Missing = @()
    foreach ($PackageShareName in $PackageShareNames) {
        $BuildManifestInstalled = Join-Path $BuildDir "vcpkg_installed\$Triplet\share\$PackageShareName"
        if (Test-Path -LiteralPath $BuildManifestInstalled -PathType Container) {
            continue
        }
        $Missing += $PackageShareName
    }

    if ($Missing.Count -eq 0) {
        return
    }

    if ($NoInstall) {
        throw "未安装 vcpkg manifest 依赖 ($($Missing -join ', '))。请先运行: `"$Root\vcpkg.exe`" install --triplet $Triplet"
    }

    $VcpkgExe = Join-Path $Root "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $VcpkgExe -PathType Leaf)) {
        throw "未找到 vcpkg.exe: $VcpkgExe"
    }

    Write-Host "未发现 vcpkg manifest 依赖 ($($Missing -join ', '))，开始使用 vcpkg 安装..."
    Push-Location $Repo
    try {
        & $VcpkgExe install --triplet $Triplet `
            "--overlay-ports=$OverlayPorts" `
            "--x-install-root=$(Join-Path $BuildDir 'vcpkg_installed')" `
            "--x-buildtrees-root=$DependencyBuildtrees" `
            "--x-packages-root=$DependencyPackages"
        if ($LASTEXITCODE -ne 0) {
            throw "vcpkg install --triplet $Triplet 失败，退出码 $LASTEXITCODE。"
        }
    }
    finally {
        Pop-Location
    }
}

function Get-VcpkgPackageVersion([string]$Root, [string]$Triplet, [string]$Package) {
    $InfoDirs = @((Join-Path $BuildDir "vcpkg_installed\vcpkg\info"))
    foreach ($InfoDir in $InfoDirs) {
        $File = Get-ChildItem -LiteralPath $InfoDir -Filter "${Package}_*_${Triplet}.list" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($File.Name -match "^$([Regex]::Escape($Package))_(.+)_$([Regex]::Escape($Triplet))\.list$") {
            return $Matches[1]
        }
    }
    return "unknown"
}

if ($StaticRuntime -and $DynamicRuntime) {
    throw "不能同时指定 -StaticRuntime 和 -DynamicRuntime。"
}
if ($SharedSlint) {
    throw "Release 产物必须静态链接 Slint；请不要使用 -SharedSlint。"
}

$UseStaticRuntime = -not [bool]$DynamicRuntime
if ($StaticRuntime) {
    $UseStaticRuntime = $true
}

if (-not $VcpkgTriplet) {
    $VcpkgTriplet = if ($UseStaticRuntime) { "x64-windows-static" } else { "x64-windows" }
}

Ensure-VcpkgManifestPackages $VcpkgRoot $VcpkgTriplet @(
    "scnlib",
    "libwebp",
    "libjxl",
    "libpng",
    "libjpeg-turbo",
    "giflib",
    "tiff",
    "libraw",
    "aom",
    "dav1d",
    "libyuv"
) $NoVcpkgInstall

# Release pins must take effect even when an earlier configure populated FetchContent.
# Reuse only clean sources whose resolved commit already matches the release pin.
$PinnedFetchContentCommits = @{
    "svtav1hdr" = "8b4b9f5624cb70c2363a7cebb553110c1447dd4c"
    "libavif" = "bd32fb40a3b2028eb555b67f769c57a32ed8a1b4"
    "jpegli"  = "031a0077f5799a6041004267fc12b956c1f52a20"
    "slint"   = "cf62c975c311e7036d599ed8ed0b7e6a8386a934"
}
$PinnedFetchContentPatchedFiles = @{
    "libavif" = " M src/write.c"
    "slint"   = " M api/cpp/include/private/slint_config.h"
}
$FetchContentSourceOverrides = @()
foreach ($FetchContentName in @("svtav1hdr", "libavif", "jpegli", "slint")) {
    $SourceDir = Join-Path $BuildDir "_deps\$FetchContentName-src"
    $ExpectedCommit = $PinnedFetchContentCommits[$FetchContentName]
    $CommitMarker = Join-Path $SourceDir ".awj-source-commit"
    $KeepSource = if ($FetchContentName -eq "svtav1hdr") {
        (Test-Path -LiteralPath $CommitMarker -PathType Leaf) -and
            ((Get-Content -LiteralPath $CommitMarker -Raw).Trim() -eq $ExpectedCommit)
    }
    else {
        $ExpectedCommit -and
            (Test-Path -LiteralPath (Join-Path $SourceDir ".git") -PathType Container)
    }
    if ($KeepSource -and $FetchContentName -ne "svtav1hdr") {
        $ResolvedCommit = (& git -C $SourceDir rev-parse HEAD).Trim()
        $SourceStatus = @(git -C $SourceDir status --porcelain --untracked-files=all)
        $ExpectedPatchedFile = $PinnedFetchContentPatchedFiles[$FetchContentName]
        $StatusMatches = $SourceStatus.Count -eq 0 -or
            ($ExpectedPatchedFile -and $SourceStatus.Count -eq 1 -and
             $SourceStatus[0] -eq $ExpectedPatchedFile)
        $KeepSource = $LASTEXITCODE -eq 0 -and $ResolvedCommit -eq $ExpectedCommit -and
            $StatusMatches
    }
    if (-not $KeepSource) {
        Remove-RepoDirectory $SourceDir
    }
    else {
        if ($FetchContentName -eq "slint") {
            cmake "-DSOURCE_DIR=$SourceDir" -P (Join-Path $Repo "cmake\patch_slint_static_import.cmake")
            if ($LASTEXITCODE -ne 0) {
                throw "无法修补缓存 Slint 源码，退出码 $LASTEXITCODE。"
            }
        }
        $FetchContentSourceOverrides +=
            "-DFETCHCONTENT_SOURCE_DIR_$($FetchContentName.ToUpperInvariant())=$SourceDir"
    }
    foreach ($FetchContentSuffix in @("build", "tmp")) {
        Remove-RepoDirectory (Join-Path $BuildDir "_deps\$FetchContentName-$FetchContentSuffix")
    }
}
if (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt")) {
    Remove-Item -LiteralPath (Join-Path $BuildDir "CMakeCache.txt") -Force
}
Remove-RepoDirectory (Join-Path $BuildDir "CMakeFiles")

$ConfigureArgs = @(
    "-U", "scn_DIR",
    "-U", "FastFloat_DIR",
    "-U", "fast_float_DIR",
    "-U", "AWJ_SVTAV1HDR_GIT_TAG",
    "-U", "AWJ_LIBAVIF_GIT_REPOSITORY",
    "-U", "AWJ_LIBAVIF_GIT_TAG",
    "-U", "AWJ_JPEGLI_GIT_REPOSITORY",
    "-U", "AWJ_JPEGLI_GIT_TAG",
    "-U", "AWJ_SLINT_GIT_REPOSITORY",
    "-U", "AWJ_SLINT_GIT_TAG",
    "-S", $Repo,
    "-B", $BuildDir,
    "-G", "Visual Studio 18 2026",
    "-A", "x64"
)
if ($VcpkgRoot) {
    $ConfigureArgs += "-DVCPKG_ROOT=$VcpkgRoot"
    $ConfigureArgs += "-DVCPKG_TRIPLET=$VcpkgTriplet"
    $ConfigureArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    $ToolchainPath = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $ConfigureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainPath"
}
$ConfigureArgs += "-DAWJ_VCPKG_LIBRARY_CONFIG=Release"
$ConfigureArgs += "-DVCPKG_OVERLAY_PORTS=$OverlayPorts"
$ConfigureArgs += "-DVCPKG_INSTALLED_DIR=$(Join-Path $BuildDir 'vcpkg_installed')"
$ConfigureArgs += "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$DependencyBuildtrees;--x-packages-root=$DependencyPackages"
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAVIF_ENABLE_RELEASE_IPO=$(if ($EnableLto) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAWJ_ENABLE_ZENRAVIF=$(if ($DisableZenravif) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAWJ_ENABLE_SVTAV1HDR=$(if ($DisableSvtAv1Hdr) { 'OFF' } else { 'ON' })"
$ConfigureArgs += $FetchContentSourceOverrides

if (Test-Path $OutputDir) {
    Get-ChildItem -LiteralPath $OutputDir -Force | Where-Object { $ReleaseFiles -notcontains $_.Name } | Remove-Item -Recurse -Force
}

cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码 $LASTEXITCODE。"
}
cmake --build $BuildDir --config Release --target AWJ AWJ-com --parallel

if ($LASTEXITCODE -ne 0) {
    throw "Release 构建失败，退出码 $LASTEXITCODE。"
}

# --- Generate BUILD_INFO.txt ---
$BuildDate = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
$AomVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "aom"
$Dav1dVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "dav1d"
$LibyuvVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "libyuv"
$FetchContentCommit = {
    param([string]$Name)
    $SourceDir = Join-Path $BuildDir "_deps\$Name-src"
    $CommitMarker = Join-Path $SourceDir ".awj-source-commit"
    if (Test-Path -LiteralPath $CommitMarker -PathType Leaf) {
        $Commit = (Get-Content -LiteralPath $CommitMarker -Raw).Trim()
        if ($Commit -notmatch "^[0-9a-f]{40}$") {
            throw "FetchContent commit marker is invalid: $CommitMarker"
        }
        return $Commit
    }
    if (-not (Test-Path -LiteralPath (Join-Path $SourceDir ".git"))) {
        return "not-built"
    }
    $Commit = (& git -C $SourceDir rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $Commit) {
        throw "无法读取 FetchContent commit: $SourceDir"
    }
    return $Commit
}
$SvtAv1HdrCommit = & $FetchContentCommit "svtav1hdr"
$LibavifCommit = & $FetchContentCommit "libavif"
$JpegliCommit = & $FetchContentCommit "jpegli"
$SlintCommit = & $FetchContentCommit "slint"
$GitTagLine = if ($GitTag) { "Git Tag: $GitTag" } else { "Git Tag:" }

$BuildInfoContent = @"
AWJimage $Version
Build Date: $BuildDate
Build Type: Release
Git Commit: $GitCommit
$GitTagLine
Architecture: x64
Source: https://github.com/Dominic485649/AWJimage

Vcpkg baseline: $VcpkgBaseline
AOM: $AomVersion
dav1d: $Dav1dVersion
libyuv: $LibyuvVersion

FetchContent Dependencies (actual commits):
  svt-av1-hdr: $SvtAv1HdrCommit
  libavif:     $LibavifCommit
  jpegli:      $JpegliCommit
  slint:       $SlintCommit
"@

$BuildInfoPath = Join-Path $OutputDir "BUILD_INFO.txt"
$BuildInfoWriteError = $null
for ($BuildInfoAttempt = 1; $BuildInfoAttempt -le 8; ++$BuildInfoAttempt) {
    try {
        Set-Content -LiteralPath $BuildInfoPath -Value $BuildInfoContent -NoNewline -Encoding UTF8
        $BuildInfoWriteError = $null
        break
    } catch {
        $BuildInfoWriteError = $_
        if ($BuildInfoAttempt -lt 8) {
            Start-Sleep -Milliseconds 250
        }
    }
}
if ($BuildInfoWriteError) {
    throw $BuildInfoWriteError
}
Write-Host "  $BuildInfoPath"

# Copy license and notice files to output directory
Copy-Item -LiteralPath (Join-Path $Repo "LICENSE") -Destination (Join-Path $OutputDir "LICENSE") -Force
Copy-Item -LiteralPath (Join-Path $Repo "THIRD_PARTY_NOTICES.txt") -Destination (Join-Path $OutputDir "THIRD_PARTY_NOTICES.txt") -Force

if (Test-Path $OutputDir) {
    Get-ChildItem -LiteralPath $OutputDir -Force | Where-Object { $ReleaseFiles -notcontains $_.Name } | Remove-Item -Recurse -Force
}

Write-Host ""
Write-Host "Release 输出:"
Write-Host "  $OutputDir\AWJ.exe"
Write-Host "  $OutputDir\AWJ.com"
Write-Host "  svt-av1-hdr: 已静态集成到主程序。"
