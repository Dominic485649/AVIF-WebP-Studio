[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$WindowsExePath,
    [Parameter(Mandatory)] [string]$WindowsComPath,
    [Parameter(Mandatory)] [string]$LinuxBinaryPath,
    [string]$Version = "",
    [ValidateSet("stable", "prerelease")] [string]$Channel = "prerelease",
    [UInt64]$ArchiveManifestSequence = 0,
    [UInt64]$LegacyManifestSequence = 0,
    [switch]$BridgeRelease,
    [string]$PublishedAtUtc = "",
    [string]$UpdateSigningSeedFile = $env:AWJ_UPDATE_SIGNING_SEED_FILE,
    [string]$UpdatePublicKeyHex = $env:AWJ_UPDATE_PUBLIC_KEY_HEX,
    [string]$SignerPath = "",
    [switch]$SkipManifests
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $Repo "VERSION") -Raw).Trim() }
if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw "VERSION 必须是 MAJOR.MINOR.PATCH。"
}
if (-not $PublishedAtUtc) { $PublishedAtUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ") }
if ($PublishedAtUtc -notmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$') {
    throw "-PublishedAtUtc 必须是 YYYY-MM-DDTHH:MM:SSZ。"
}

function Get-RepoPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path)
}

function Assert-File([string]$Path, [string]$Label) {
    $Resolved = Get-RepoPath $Path
    if (-not (Test-Path -LiteralPath $Resolved -PathType Leaf)) { throw "$Label 不存在: $Resolved" }
    return $Resolved
}

function Remove-OnlyInsideRepo([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $Root = (Get-RepoPath $Repo).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $Resolved = Get-RepoPath $Path
    if (-not $Resolved.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除仓库外目录: $Resolved"
    }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $Temporary = "$Path.tmp-$PID"
    [IO.File]::WriteAllText($Temporary, $Text, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $Temporary -Destination $Path -Force
}

function Write-Checksum([string]$Path) {
    $Hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$Path.sha256" -Value "$Hash  $([IO.Path]::GetFileName($Path))" -NoNewline -Encoding utf8
}

function Get-Asset([string]$Path, [string]$Url) {
    return [ordered]@{
        url = $Url
        size = [UInt64](Get-Item -LiteralPath $Path).Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-ChangelogBody([string]$Path) {
    $Text = (Get-Content -LiteralPath $Path -Raw).Replace("`r`n", "`n")
    $Match = [regex]::Match($Text, "(?ms)^## $([regex]::Escape($Version)) - [^`n]+`n(?<body>.*?)(?=^## |\z)")
    if (-not $Match.Success -or -not $Match.Groups['body'].Value.Trim()) {
        throw "更新日志缺少 $Version 的独立条目: $Path"
    }
    return $Match.Groups['body'].Value.Trim()
}

function Get-SortedEntries($Entries) {
    return @($Entries | Sort-Object {
        $p = ([string]$_.version).Split('.') | ForEach-Object { [UInt32]$_ }
        '{0:D10}.{1:D10}.{2:D10}' -f $p[0], $p[1], $p[2]
    })
}

function Sign-Manifest([string]$ManifestPath, [string]$SignaturePath) {
    if (-not $UpdateSigningSeedFile -or -not (Test-Path -LiteralPath $UpdateSigningSeedFile -PathType Leaf)) {
        throw "签名需要仓库外的 -UpdateSigningSeedFile。"
    }
    if ($UpdatePublicKeyHex -notmatch '^[0-9a-f]{64}$') { throw "需要 64 位小写 -UpdatePublicKeyHex。" }
    & $SignerPath $ManifestPath $UpdateSigningSeedFile $SignaturePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "manifest 签名失败。" }
    & $SignerPath --verify $ManifestPath $UpdatePublicKeyHex $SignaturePath
    if ($LASTEXITCODE -ne 0) { throw "manifest 签名自检失败。" }
}

function Get-ArchiveMembers([string]$Directory, [string]$ArchiveUrl) {
    return @(
        Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName | ForEach-Object {
            $Relative = $_.FullName.Substring($Directory.Length).TrimStart('\', '/').Replace('\', '/')
            [ordered]@{ path = $Relative; url = $ArchiveUrl; size = [UInt64]$_.Length;
                         sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
}

function Add-Archive([string]$PackageDirectory, [string]$ArchivePath) {
    Push-Location $PackageDirectory
    try {
        & 7z.exe a -t7z $ArchivePath .\* -m0=lzma2 -mx=9 -mmt=1 -mf=off | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "7z 创建失败: $ArchivePath" }
    } finally { Pop-Location }
    & 7z.exe t $ArchivePath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7z 完整性检查失败: $ArchivePath" }
}

function Assert-ArchiveRoundTrip([string]$ArchivePath, [string]$PackageDirectory, [string]$VerifyDirectory) {
    Remove-OnlyInsideRepo $VerifyDirectory
    New-Item -ItemType Directory -Path $VerifyDirectory -Force | Out-Null
    & 7z.exe x $ArchivePath "-o$VerifyDirectory" -y | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7z 解压验证失败: $ArchivePath" }
    $OriginalFiles = @(Get-ChildItem -LiteralPath $PackageDirectory -File -Recurse)
    $ExtractedFiles = @(Get-ChildItem -LiteralPath $VerifyDirectory -File -Recurse)
    $OriginalNames = @($OriginalFiles | ForEach-Object { $_.FullName.Substring($PackageDirectory.Length).TrimStart('\', '/') } | Sort-Object)
    $ExtractedNames = @($ExtractedFiles | ForEach-Object { $_.FullName.Substring($VerifyDirectory.Length).TrimStart('\', '/') } | Sort-Object)
    if ((Compare-Object $OriginalNames $ExtractedNames).Count) {
        throw "归档包含缺失或额外成员: $ArchivePath"
    }
    foreach ($Original in $OriginalFiles) {
        $Relative = $Original.FullName.Substring($PackageDirectory.Length).TrimStart('\', '/')
        $Extracted = Join-Path $VerifyDirectory $Relative
        if (-not (Test-Path -LiteralPath $Extracted -PathType Leaf) -or
            (Get-FileHash -LiteralPath $Original.FullName -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $Extracted -Algorithm SHA256).Hash) {
            throw "归档往返哈希不匹配: $Relative"
        }
    }
}

$WindowsExePath = Assert-File $WindowsExePath "AWJ.exe"
$WindowsComPath = Assert-File $WindowsComPath "AWJ.com"
$LinuxBinaryPath = Assert-File $LinuxBinaryPath "Linux AWJ"
if (([IO.Path]::GetFileName($WindowsExePath) -ne "AWJ.exe") -or
    ([IO.Path]::GetFileName($WindowsComPath) -ne "AWJ.com") -or
    ([IO.Path]::GetFileName($LinuxBinaryPath) -ne "AWJ")) {
    throw "输入文件名必须严格为 AWJ.exe、AWJ.com 和 AWJ。"
}
if (-not (Get-Command 7z.exe -ErrorAction SilentlyContinue)) { throw "未找到 7z.exe。" }
if (-not $SkipManifests) {
    if (-not $SignerPath) { $SignerPath = Join-Path (Split-Path -Parent $WindowsExePath) "awj_update_manifest_sign.exe" }
    $SignerPath = Assert-File $SignerPath "manifest 签名工具"
}

$Stage = Join-Path $Repo "build\release\$Version"
Remove-OnlyInsideRepo $Stage
$Assets = Join-Path $Stage "assets"
$WinPackage = Join-Path $Stage "package\AWJ_Win"
$LinuxPackage = Join-Path $Stage "package\AWJ_Linux"
New-Item -ItemType Directory -Path $Assets, $WinPackage, $LinuxPackage -Force | Out-Null

$Commit = (git -C $Repo rev-parse HEAD).Trim()
$VcpkgConfiguration = Get-Content -LiteralPath (Join-Path $Repo "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$Baseline = $VcpkgConfiguration.'default-registry'.baseline
$CmakeSource = Get-Content -LiteralPath (Join-Path $Repo "CMakeLists.txt") -Raw
function Get-CmakePinnedSource([string]$Name) {
    $Pattern = '(?m)^set\(' + [regex]::Escape($Name) + '\s+"(?<value>[^"]+)"'
    $Match = [regex]::Match($CmakeSource, $Pattern)
    if (-not $Match.Success) { throw "无法从 CMakeLists.txt 读取 $Name。" }
    return $Match.Groups['value'].Value
}
$SvtAv1HdrCommit = Get-CmakePinnedSource "AWJ_SVTAV1HDR_GIT_TAG"
$LibavifCommit = Get-CmakePinnedSource "AWJ_LIBAVIF_GIT_TAG"
$JpegliCommit = Get-CmakePinnedSource "AWJ_JPEGLI_GIT_TAG"
$SlintCommit = Get-CmakePinnedSource "AWJ_SLINT_GIT_TAG"
function Stage-Package([string]$PackageDirectory, [string]$Binary, [string]$Platform) {
    Copy-Item -LiteralPath $Binary -Destination (Join-Path $PackageDirectory ([IO.Path]::GetFileName($Binary))) -Force
    Write-Checksum (Join-Path $PackageDirectory ([IO.Path]::GetFileName($Binary)))
    Copy-Item -LiteralPath (Join-Path $Repo "LICENSE"), (Join-Path $Repo "THIRD_PARTY_NOTICES.txt") -Destination $PackageDirectory -Force
    $LicenseDirectory = Join-Path $PackageDirectory "THIRD_PARTY_LICENSES"
    New-Item -ItemType Directory -Path $LicenseDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Repo "licenses\libplacebo-LGPL-2.1-or-later.txt") -Destination $LicenseDirectory -Force
    $Info = @"
AWJimage $Version
Build Type: Release
Git Commit: $Commit
Architecture: x64
Platform: $Platform
Vcpkg baseline: $Baseline
SVT-AV1-HDR commit: $SvtAv1HdrCommit
libavif commit: $LibavifCommit
Jpegli commit: $JpegliCommit
Slint commit: $SlintCommit
libplacebo: v7.360.1
libarchive: v3.8.9
Source: https://github.com/Dominic485649/AWJimage
"@
    Write-Utf8NoBom (Join-Path $PackageDirectory "BUILD_INFO.txt") $Info
}
Stage-Package $WinPackage $WindowsExePath "Windows"
Copy-Item -LiteralPath $WindowsComPath -Destination (Join-Path $WinPackage "AWJ.com") -Force
Write-Checksum (Join-Path $WinPackage "AWJ.com")
Stage-Package $LinuxPackage $LinuxBinaryPath "Linux"

$WindowsArchive = Join-Path $Assets "AWJ_Win.7z"
$LinuxArchive = Join-Path $Assets "AWJ_Linux.7z"
Add-Archive $WinPackage $WindowsArchive
Add-Archive $LinuxPackage $LinuxArchive
$WinVerify = Join-Path $Stage "verify\AWJ_Win"
$LinuxVerify = Join-Path $Stage "verify\AWJ_Linux"
Assert-ArchiveRoundTrip $WindowsArchive $WinPackage $WinVerify
Assert-ArchiveRoundTrip $LinuxArchive $LinuxPackage $LinuxVerify
$GuiSmoke = Start-Process -FilePath (Join-Path $WinVerify "AWJ.exe") -ArgumentList "--help" -PassThru -Wait -WindowStyle Hidden
if ($GuiSmoke.ExitCode -ne 0) { throw "Windows AWJ.exe --help smoke 失败。" }
& (Join-Path $WinVerify "AWJ.com") --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Windows AWJ.com --help smoke 失败。" }
if ($BridgeRelease) {
    Copy-Item -LiteralPath $WindowsExePath, $WindowsComPath -Destination $Assets -Force
}

if (-not $SkipManifests) {
    $ReleaseBase = "https://github.com/Dominic485649/AWJimage/releases"
    $Changelog = [ordered]@{ 'zh-CN' = Get-ChangelogBody (Join-Path $Repo "CHANGELOG.md"); en = Get-ChangelogBody (Join-Path $Repo "CHANGELOG.en.md") }
    if (-not $BridgeRelease) {
        if ($ArchiveManifestSequence -eq 0) { throw "1.0.4 归档更新需要 -ArchiveManifestSequence。" }
        $ManifestPath = Join-Path $Repo "update-manifest-v2.json"
        $SignaturePath = "$ManifestPath.sig"
        $Old = if (Test-Path -LiteralPath $ManifestPath) { Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -AsHashtable } else { $null }
        if ($Old) {
            & $SignerPath --verify $ManifestPath $UpdatePublicKeyHex $SignaturePath
            if ($LASTEXITCODE -ne 0) { throw "现有 v2 manifest 签名无效。" }
            if ($ArchiveManifestSequence -le [UInt64]$Old.sequence) { throw "v2 sequence 必须递增。" }
        }
        $WinUrl = "$ReleaseBase/download/$Version/AWJ_Win.7z"
        $LinuxUrl = "$ReleaseBase/download/$Version/AWJ_Linux.7z"
        $Entry = [ordered]@{
            version = $Version; channel = $Channel; published_at = $PublishedAtUtc
            release_url = "$ReleaseBase/tag/$Version"; minimum_updater_version = "1.0.4"; revoked = $false
            assets = [ordered]@{
                windows_x64_archive = [ordered]@{ archive = Get-Asset $WindowsArchive $WinUrl; members = Get-ArchiveMembers $WinPackage $WinUrl }
                linux_x64_archive = [ordered]@{ archive = Get-Asset $LinuxArchive $LinuxUrl; members = Get-ArchiveMembers $LinuxPackage $LinuxUrl }
            }
            changelog = $Changelog
        }
        $Entries = @($(if ($Old) { $Old.entries } else { @() }) + $Entry)
        if (@($Entries | Where-Object { $_.version -eq $Version }).Count -ne 1) { throw "v2 manifest 版本重复。" }
        $SortedEntries = [object[]]@(Get-SortedEntries $Entries)
        $Json = ([ordered]@{ schema = 2; sequence = $ArchiveManifestSequence; entries = $SortedEntries } | ConvertTo-Json -Depth 32).Replace("`r`n", "`n") + "`n"
        if (-not ((ConvertFrom-Json -InputObject $Json).entries -is [System.Array])) { throw "v2 manifest entries 必须序列化为数组。" }
        Write-Utf8NoBom $ManifestPath $Json
        Sign-Manifest $ManifestPath $SignaturePath
    }
    if ($BridgeRelease) {
        if ($LegacyManifestSequence -eq 0) { throw "1.0.5 桥接更新需要 -LegacyManifestSequence。" }
        $LegacyPath = Join-Path $Repo "update-manifest.json"
        $LegacySignature = "$LegacyPath.sig"
        & $SignerPath --verify $LegacyPath $UpdatePublicKeyHex $LegacySignature
        if ($LASTEXITCODE -ne 0) { throw "现有 v1 manifest 签名无效。" }
        $Old = Get-Content -LiteralPath $LegacyPath -Raw | ConvertFrom-Json -AsHashtable
        if ($Old.schema -ne 1 -or $LegacyManifestSequence -le [UInt64]$Old.sequence) { throw "v1 sequence 必须递增。" }
        $Entry = [ordered]@{
            version = $Version; channel = $Channel; published_at = $PublishedAtUtc
            release_url = "$ReleaseBase/tag/$Version"; minimum_updater_version = "1.0.1"; revoked = $false
            assets = [ordered]@{
                windows_x64_exe = Get-Asset (Join-Path $Assets "AWJ.exe") "$ReleaseBase/download/$Version/AWJ.exe"
                windows_x64_com = Get-Asset (Join-Path $Assets "AWJ.com") "$ReleaseBase/download/$Version/AWJ.com"
                linux_x64 = Get-Asset $LinuxArchive "$ReleaseBase/download/$Version/AWJ_Linux.7z"
            }
            changelog = $Changelog
        }
        $Entries = @($Old.entries + $Entry)
        if (@($Entries | Where-Object { $_.version -eq $Version }).Count -ne 1) { throw "v1 manifest 版本重复。" }
        $SortedEntries = [object[]]@(Get-SortedEntries $Entries)
        $Json = ([ordered]@{ schema = 1; sequence = $LegacyManifestSequence; entries = $SortedEntries } | ConvertTo-Json -Depth 20).Replace("`r`n", "`n") + "`n"
        if (-not ((ConvertFrom-Json -InputObject $Json).entries -is [System.Array])) { throw "v1 manifest entries 必须序列化为数组。" }
        Write-Utf8NoBom $LegacyPath $Json
        Sign-Manifest $LegacyPath $LegacySignature
    }
}

Get-ChildItem -LiteralPath $Assets -File | Select-Object Name, Length, @{n='SHA256'; e={(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}} | Format-Table -AutoSize
Write-Host "Release stage: $Stage"
