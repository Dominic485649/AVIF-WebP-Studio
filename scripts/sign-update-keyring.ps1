[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$KeyringPath,
    [Parameter(Mandatory)] [hashtable]$RootSeedFiles,
    [string]$SignerPath = ""
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$RepoRoot = [IO.Path]::GetFullPath($Repo).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar

function Assert-InsideRepo([string]$Path, [string]$Label) {
    $Resolved = [IO.Path]::GetFullPath($Path)
    if (-not $Resolved.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝在仓库外读取或写入 ${Label}: $Resolved"
    }
    return $Resolved
}

function Assert-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label 不存在: $Path" }
    return $Path
}

function Parse-UtcStamp([string]$Value, [string]$Name) {
    try {
        return [DateTimeOffset]::ParseExact(
            $Value, "yyyy-MM-dd'T'HH:mm:ss'Z'",
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal)
    } catch {
        throw "$Name 必须是 YYYY-MM-DDTHH:MM:SSZ。"
    }
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $Temporary = "$Path.tmp-$PID"
    [IO.File]::WriteAllText($Temporary, $Text, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $Temporary -Destination $Path -Force
}

function Get-CmakePublicKey([string]$Name) {
    $Cmake = Get-Content -LiteralPath (Join-Path $Repo "CMakeLists.txt") -Raw
    $Match = [regex]::Match($Cmake, '(?m)^set\(' + [regex]::Escape($Name) + '\s+"(?<value>[0-9a-f]{64})"')
    if (-not $Match.Success) { throw "无法从 CMakeLists.txt 读取 $Name。" }
    return $Match.Groups['value'].Value
}

$KeyringPath = Assert-InsideRepo $KeyringPath "update keyring"
Assert-File $KeyringPath "update keyring" | Out-Null
if (-not $SignerPath) { $SignerPath = Join-Path $Repo "bin\x64\Release\awj_update_manifest_sign.exe" }
$SignerPath = Assert-File ([IO.Path]::GetFullPath($SignerPath)) "manifest 签名工具"

try { $Keyring = Get-Content -LiteralPath $KeyringPath -Raw | ConvertFrom-Json -AsHashtable -DateKind String }
catch { throw "update keyring 不是有效 JSON。" }
if ($Keyring.schema -ne 1 -or [UInt64]$Keyring.sequence -eq 0 -or
    -not $Keyring.release_keys -or @($Keyring.release_keys).Count -gt 16) {
    throw "update keyring 的 schema、sequence 或 release_keys 非法。"
}
$IssuedAt = Parse-UtcStamp ([string]$Keyring.issued_at) "issued_at"
$ExpiresAt = Parse-UtcStamp ([string]$Keyring.expires_at) "expires_at"
if ($ExpiresAt -le $IssuedAt -or $ExpiresAt - $IssuedAt -gt [TimeSpan]::FromDays(180)) {
    throw "update keyring 有效期必须为 1 秒到 180 天。"
}
$SeenReleaseIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($Key in @($Keyring.release_keys)) {
    if ($Key.key_id -notmatch '^[a-z0-9-]{1,64}$' -or
        $Key.public_key -notmatch '^[0-9a-f]{64}$' -or
        -not $SeenReleaseIds.Add([string]$Key.key_id)) {
        throw "update keyring 的发布密钥 ID 或公钥非法/重复。"
    }
    $Start = Parse-UtcStamp ([string]$Key.not_before) "release_keys.not_before"
    $End = Parse-UtcStamp ([string]$Key.expires_at) "release_keys.expires_at"
    if ($End -le $Start -or $End - $Start -gt [TimeSpan]::FromDays(366)) {
        throw "update keyring 的发布密钥有效期必须为 1 秒到 366 天。"
    }
}

$Roots = [ordered]@{
    'root-legacy-2026' = Get-CmakePublicKey 'AWJ_UPDATE_PUBLIC_KEY_HEX'
    'root-recovery-a-2026' = Get-CmakePublicKey 'AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX'
    'root-recovery-b-2026' = Get-CmakePublicKey 'AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX'
}
if ($RootSeedFiles.Count -lt 2 -or $RootSeedFiles.Count -gt $Roots.Count) {
    throw "必须提供两到三把不同根密钥的 seed。"
}

$TemporaryDirectory = Assert-InsideRepo (Join-Path $Repo "build\keyring-sign-$PID") "签名临时目录"
if (Test-Path -LiteralPath $TemporaryDirectory) { Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force }
New-Item -ItemType Directory -Path $TemporaryDirectory | Out-Null
try {
    $Signatures = [Collections.Generic.List[object]]::new()
    foreach ($RootId in @($RootSeedFiles.Keys | Sort-Object)) {
        if (-not $Roots.Contains($RootId)) { throw "未知根密钥 ID: $RootId" }
        $SeedPath = Assert-File ([IO.Path]::GetFullPath([string]$RootSeedFiles[$RootId])) "根 seed"
        $PublicKey = ((& $SignerPath --print-public-key $SeedPath | Select-Object -First 1).Trim())
        if ($LASTEXITCODE -ne 0 -or $PublicKey -cne $Roots[$RootId]) {
            throw "根 seed 与编译进客户端的 $RootId 公钥不匹配。"
        }
        $SignaturePath = Join-Path $TemporaryDirectory "$RootId.sig"
        & $SignerPath $KeyringPath $SeedPath $SignaturePath | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$RootId 签名失败。" }
        & $SignerPath --verify $KeyringPath $PublicKey $SignaturePath | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$RootId 签名自检失败。" }
        $Signatures.Add([ordered]@{
            key_id = [string]$RootId
            signature = (Get-Content -LiteralPath $SignaturePath -Raw).Trim()
        })
    }
    $Envelope = ([ordered]@{ schema = 1; signatures = @($Signatures) } |
        ConvertTo-Json -Depth 8).Replace("`r`n", "`n") + "`n"
    Write-Utf8NoBom "$KeyringPath.sig" $Envelope
} finally {
    if (Test-Path -LiteralPath $TemporaryDirectory) {
        Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force
    }
}

Write-Host "Signed update keyring: $KeyringPath"
