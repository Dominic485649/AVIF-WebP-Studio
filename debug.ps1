param(
    [string]$MagickRoot = "",
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "",
    [switch]$StaticRuntime,
    [switch]$DynamicRuntime,
    [switch]$SharedSlint,
    [ValidateSet("Static", "Dynamic")]
    [string]$MagickLinkage = "Static",
    [string]$ImageMagickRef = "6ad8928f61d4abf3fe17646d7083bb6866eae92e",
    [switch]$FullMagickBuild,
    [switch]$InstallMfc,
    [switch]$NoVcpkgInstall,
    [switch]$UseScoopFallback
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSCommandPath
$BuildDir = Join-Path $Repo "build\x64\Debug"

if (-not $VcpkgRoot) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } elseif (Test-Path "D:\Scoop\apps\vcpkg\current") {
        $VcpkgRoot = "D:\Scoop\apps\vcpkg\current"
    }
}

function Test-MagickRuntimeLooksStatic([string]$Root) {
    if (-not $Root -or -not (Test-Path -LiteralPath $Root -PathType Container)) {
        return $false
    }

    $Dll = Get-ChildItem -LiteralPath $Root -Filter "*.dll" -File -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    return $null -eq $Dll
}

function Ensure-VcpkgPackage([string]$Root, [string]$Triplet, [string]$Port, [string]$PackageShareName, [bool]$NoInstall) {
    if (-not $Root) {
        throw "未找到 vcpkg。请设置 VCPKG_ROOT，或传入 -VcpkgRoot。"
    }

    $ShareDir = Join-Path $Root "installed\$Triplet\share\$PackageShareName"
    if (Test-Path -LiteralPath $ShareDir -PathType Container) {
        return
    }

    if ($NoInstall) {
        throw "未安装 $Port`:$Triplet。请先运行: `"$Root\vcpkg.exe`" install $Port`:$Triplet"
    }

    $VcpkgExe = Join-Path $Root "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $VcpkgExe -PathType Leaf)) {
        throw "未找到 vcpkg.exe: $VcpkgExe"
    }

    Write-Host "未发现 $Port`:$Triplet，开始使用 vcpkg 安装..."
    & $VcpkgExe install "$Port`:$Triplet"
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg install $Port`:$Triplet 失败，退出码 $LASTEXITCODE。"
    }
}

if (-not $MagickRoot) {
    $SelfBuilt = Join-Path $Repo "third_party\imagemagick-runtime\x64\Debug"
    if (Test-Path (Join-Path $SelfBuilt "include\MagickWand\MagickWand.h")) {
        $MagickRoot = $SelfBuilt
    } elseif ($UseScoopFallback -and (Test-Path "D:\Scoop\apps\imagemagick\current\include\MagickWand\MagickWand.h")) {
        $MagickRoot = "D:\Scoop\apps\imagemagick\current"
        Write-Warning "未发现自编译 ImageMagick，按 -UseScoopFallback 临时使用 Scoop ImageMagick。"
    } else {
        Write-Host "未发现自编译 ImageMagick，开始自动构建 Debug $MagickLinkage 运行时..."
        $MagickBuildScript = Join-Path $Repo "scripts\build-magick.ps1"
        $MagickBuildArgs = @{
            Configuration = "Debug"
            Arch = "x64"
            Linkage = $MagickLinkage
            ImageMagickRef = $ImageMagickRef
        }
        if ($FullMagickBuild) {
            $MagickBuildArgs.FullBuild = $true
        }
        if ($InstallMfc) {
            $MagickBuildArgs.InstallMfc = $true
        }

        & $MagickBuildScript @MagickBuildArgs
        if ($LASTEXITCODE -ne 0) {
            throw "ImageMagick 自动构建失败，退出码 $LASTEXITCODE。"
        }
        if (-not (Test-Path (Join-Path $SelfBuilt "include\MagickWand\MagickWand.h"))) {
            throw "ImageMagick 自动构建完成后仍未找到运行时: $SelfBuilt"
        }
        $MagickRoot = $SelfBuilt
    }
}

if ($StaticRuntime -and $DynamicRuntime) {
    throw "不能同时指定 -StaticRuntime 和 -DynamicRuntime。"
}

$UseStaticRuntime = [bool]$StaticRuntime
if (-not $StaticRuntime -and -not $DynamicRuntime -and (Test-MagickRuntimeLooksStatic $MagickRoot)) {
    $UseStaticRuntime = $true
    Write-Host "检测到静态 ImageMagick runtime，自动使用 /MTd 运行库。"
}

if (-not $VcpkgTriplet) {
    $VcpkgTriplet = if ($UseStaticRuntime) { "x64-windows-static" } else { "x64-windows" }
}

Ensure-VcpkgPackage $VcpkgRoot $VcpkgTriplet "scnlib" "scnlib" $NoVcpkgInstall

$ConfigureArgs = @(
    "-U", "scn_DIR",
    "-U", "FastFloat_DIR",
    "-U", "fast_float_DIR",
    "-U", "MAGICKWAND_LIBRARY",
    "-U", "MAGICKCORE_LIBRARY",
    "-S", $Repo,
    "-B", $BuildDir,
    "-G", "Visual Studio 18 2026",
    "-A", "x64"
)
if ($VcpkgRoot) {
    $ConfigureArgs += "-DVCPKG_ROOT=$VcpkgRoot"
    $ConfigureArgs += "-DVCPKG_TRIPLET=$VcpkgTriplet"
}
if ($MagickRoot) {
    $ConfigureArgs += "-DMAGICK_ROOT=$MagickRoot"
}
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"

cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码 $LASTEXITCODE。"
}
cmake --build $BuildDir --config Debug --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Debug 构建失败，退出码 $LASTEXITCODE。"
}

$OutputDir = Join-Path $Repo "bin\x64\Debug"
$StaleArtifacts = @(
    "avif_core.dll",
    "avif_core.pdb"
)
foreach ($LegacyPrefix in @(
    ("AVIF" + "Console" + "Cpp"),
    ("AVIF" + "Console" + "Cli"),
    ("AVIF" + "Studio")
)) {
    $StaleArtifacts += @("$LegacyPrefix.exe", "$LegacyPrefix.pdb")
}
if (-not $SharedSlint) {
    $StaleArtifacts += @("slint_cpp.dll", "slint-compiler.exe", "slint_compiler.pdb")
}
foreach ($Name in $StaleArtifacts) {
    $Path = Join-Path $OutputDir $Name
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
}

Write-Host ""
Write-Host "Debug 输出:"
Write-Host "  $OutputDir\AVIF-WebP-Cli.exe"
Write-Host "  $OutputDir\AVIF-WebP-Studio.exe"
