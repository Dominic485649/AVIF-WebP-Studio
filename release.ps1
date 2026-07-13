param(
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "",
    [switch]$StaticRuntime,
    [switch]$DynamicRuntime,
    [switch]$SharedSlint,
    [switch]$NoVcpkgInstall,
    [switch]$EnableLto,
    [switch]$DisableZenravif,
    [switch]$DisableSvtAv1Hdr
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSCommandPath
$BuildDir = Join-Path $Repo "build\x64\Release"
$OutputDir = Join-Path $Repo "bin\x64\Release"
$Version = (Get-Content (Join-Path $Repo "VERSION") -Raw).Trim()
$VcpkgConfiguration = Get-Content (Join-Path $Repo "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$VcpkgBaseline = $VcpkgConfiguration.'default-registry'.baseline
$GitCommit = (git -C $Repo rev-parse HEAD).Trim()
$GitDirty = [bool](git -C $Repo status --porcelain)
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
        $RepoManifestInstalled = Join-Path $Repo "vcpkg_installed\$Triplet\share\$PackageShareName"
        $GlobalInstalled = Join-Path $Root "installed\$Triplet\share\$PackageShareName"
        if ((Test-Path -LiteralPath $BuildManifestInstalled -PathType Container) -or
            (Test-Path -LiteralPath $RepoManifestInstalled -PathType Container) -or
            (Test-Path -LiteralPath $GlobalInstalled -PathType Container)) {
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
        & $VcpkgExe install --triplet $Triplet
        if ($LASTEXITCODE -ne 0) {
            throw "vcpkg install --triplet $Triplet 失败，退出码 $LASTEXITCODE。"
        }
    }
    finally {
        Pop-Location
    }
}

function Get-VcpkgPackageVersion([string]$Root, [string]$Triplet, [string]$Package) {
    $InfoDirs = @(
        (Join-Path $BuildDir "vcpkg_installed\vcpkg\info"),
        (Join-Path $Repo "vcpkg_installed\vcpkg\info")
    )
    if ($Root) {
        $InfoDirs += Join-Path $Root "installed\vcpkg\info"
    }
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

$ConfigureArgs = @(
    "-U", "scn_DIR",
    "-U", "FastFloat_DIR",
    "-U", "fast_float_DIR",
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
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAVIF_ENABLE_RELEASE_IPO=$(if ($EnableLto) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAWJ_ENABLE_ZENRAVIF=$(if ($DisableZenravif) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAWJ_ENABLE_SVTAV1HDR=$(if ($DisableSvtAv1Hdr) { 'OFF' } else { 'ON' })"

$ReleaseFiles = @("AWJ.exe", "AWJ.com", "AWJ.exe.sha256", "AWJ.com.sha256", "AWJ", "AWJ.sha256", "LICENSE", "THIRD_PARTY_NOTICES.txt", "BUILD_INFO.txt")
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

$BuildInfoContent = @"
AWJimage $Version
Build Date: $BuildDate
Build Type: Release
Git Commit: $GitCommit
Git Tag: $GitTag
Architecture: x64
Source: https://github.com/Dominic485649/AWJimage

Vcpkg baseline: $VcpkgBaseline
AOM: $AomVersion
dav1d: $Dav1dVersion

FetchContent Dependencies (pinned commits):
  svt-av1-hdr: cfb4e17693ae16945a7fe288d45437243d96c12e
  libavif:     c5240fc79fe5c2407e10afd35f5505ef6333ea49
  jpegli:      031a0077f5799a6041004267fc12b956c1f52a20
  slint:       cf62c975c311e7036d599ed8ed0b7e6a8386a934
"@

$BuildInfoPath = Join-Path $OutputDir "BUILD_INFO.txt"
Set-Content -LiteralPath $BuildInfoPath -Value $BuildInfoContent -NoNewline -Encoding UTF8
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
