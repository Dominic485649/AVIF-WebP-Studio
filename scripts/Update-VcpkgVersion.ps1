<#
.SYNOPSIS
    Synchronizes the version number from the VERSION file into vcpkg.json.
.DESCRIPTION
    Reads the VERSION file at the repository root, validates it as MAJOR.MINOR.PATCH,
    and updates the "version" field in vcpkg.json. Removes any legacy version-string,
    version-semver, or version-date fields to keep the manifest clean.
    Idempotent: running multiple times produces the same result.
.PARAMETER Repo
    Path to the repository root. Defaults to the parent directory of this script.
#>
param(
    [string]$Repo = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = 'Stop'

$versionFile = Join-Path $Repo 'VERSION'
$vcpkgFile   = Join-Path $Repo 'vcpkg.json'

# --- Read & validate VERSION ---
if (-not (Test-Path $versionFile)) {
    Write-Error "VERSION file not found at $versionFile"
    exit 1
}

$version = (Get-Content $versionFile -Raw).Trim()

if ($version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "VERSION '$version' is not valid MAJOR.MINOR.PATCH"
    exit 1
}

# --- Read & parse vcpkg.json ---
if (-not (Test-Path $vcpkgFile)) {
    Write-Error "vcpkg.json not found at $vcpkgFile"
    exit 1
}

$raw = Get-Content $vcpkgFile -Raw
$manifest = $raw | ConvertFrom-Json

# Idempotency check: skip if already up-to-date and no stale fields exist
$hasStale = $false
foreach ($key in @('version-string', 'version-semver', 'version-date')) {
    if ($manifest.PSObject.Properties.Name -contains $key) {
        $hasStale = $true
        break
    }
}

if ($manifest.version -eq $version -and -not $hasStale) {
    Write-Host "vcpkg.json version already up to date ($version)"
    exit 0
}

# --- Remove legacy version fields ---
foreach ($key in @('version-string', 'version-semver', 'version-date')) {
    if ($manifest.PSObject.Properties.Name -contains $key) {
        $manifest.PSObject.Properties.Remove($key)
    }
}

# --- Set version ---
$manifest.version = $version

# --- Rebuild ordered output: name, version, then remaining keys ---
$ordered = [ordered]@{}

# Canonical order for the first few fields
$priorityKeys = @('name', 'version', '$schema', 'maintainers', 'description',
                  'homepage', 'license', 'supports', 'dependencies',
                  'overrides', 'registries', 'builtin-baseline')

foreach ($key in $priorityKeys) {
    if ($manifest.PSObject.Properties.Name -contains $key) {
        $ordered[$key] = $manifest.$key
    }
}

# Append any remaining keys not in the priority list
foreach ($prop in $manifest.PSObject.Properties) {
    if (-not $ordered.Contains($prop.Name)) {
        $ordered[$prop.Name] = $prop.Value
    }
}

# --- Write back ---
$json = $ordered | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($vcpkgFile, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Host "vcpkg.json version updated to $version"
