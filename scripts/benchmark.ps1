#requires -Version 7.4

[CmdletBinding()]
param(
    [string]$InputRoot = 'D:\图片\benchmark\test',
    [string]$Executable = '',
    [string]$ResultsRoot = '',
    [ValidateSet('cli', 'studio', 'shell')]
    [string[]]$Surface = @('cli', 'studio', 'shell'),
    [ValidateSet(1, 4, 12, 13, 613)]
    [int[]]$Count = @(1, 4, 12, 13, 613),
    [ValidateRange(0, 613)]
    [int[]]$AlphaCoverageCount = @(1, 13),
    [ValidateRange(1, 100)]
    [int]$Quality = 80,
    [ValidateRange(0, 10)]
    [int]$Speed = 6,
    [ValidateSet('420', '444')]
    [string]$Chroma = '420',
    [ValidateSet(8, 10, 12)]
    [int]$BitDepth = 8,
    [ValidateRange(0, 3600)]
    [int]$CooldownSeconds = 30,
    [string]$PowerSchemeGuid = '',
    [switch]$Smoke,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { throw 'Median requires at least one value.' }
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    $middle = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
}

function Get-P95([double[]]$Values) {
    if ($Values.Count -eq 0) { throw 'P95 requires at least one value.' }
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    return $sorted[[Math]::Ceiling(0.95 * $sorted.Count) - 1]
}

function Select-Evenly([object[]]$Items, [int]$Take) {
    if ($Take -lt 1 -or $Take -gt $Items.Count) {
        throw "Cannot select $Take items from $($Items.Count)."
    }
    if ($Take -eq $Items.Count) { return @($Items) }
    if ($Take -eq 1) { return @($Items[[Math]::Floor(($Items.Count - 1) / 2)]) }
    return @(for ($i = 0; $i -lt $Take; $i++) {
        $index = [Math]::Floor($i * ($Items.Count - 1) / ($Take - 1))
        $Items[$index]
    })
}

function Get-GitStatusPath([string]$Line) {
    return $Line.Substring(3).Trim('"').Replace('\', '/')
}

function Invoke-SelfTest {
    if ((Get-Median @(5, 1, 3, 2, 4)) -ne 3) { throw 'Median self-test failed.' }
    if ((Get-P95 @(5, 1, 3, 2, 4)) -ne 5) { throw 'P95 self-test failed.' }
    $sample = @(Select-Evenly @(0, 1, 2, 3, 4, 5, 6) 4)
    if (($sample -join ',') -ne '0,2,4,6') { throw 'Selection self-test failed.' }
    if ((Get-GitStatusPath ' M bin/x64/Release/AWJ.exe') -ne 'bin/x64/Release/AWJ.exe') { throw 'Git status self-test failed.' }
    Write-Host 'benchmark.ps1 self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

if (-not $IsWindows) { throw 'This benchmark runner currently requires Windows.' }

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Executable) { $Executable = Join-Path $Repo 'bin\x64\Release\AWJ.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$InputRoot = (Resolve-Path -LiteralPath $InputRoot).Path
if (-not $ResultsRoot) {
    $ResultsRoot = Join-Path $Repo ('build\benchmarks\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$ResultsRoot = [IO.Path]::GetFullPath($ResultsRoot)
New-Item -ItemType Directory -Path $ResultsRoot -Force | Out-Null

function Invoke-Git([string[]]$Arguments) {
    $text = & git -C $Repo @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git failed: git $($Arguments -join ' ')" }
    return ($text -join "`n").Trim()
}

function Get-BuildInfoValue([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    $line = Get-Content -LiteralPath $Path | Where-Object { $_ -match "^$([Regex]::Escape($Name)):\s*" } | Select-Object -First 1
    return ($line -replace "^$([Regex]::Escape($Name)):\s*", '').Trim()
}

function Get-VcpkgPortVersion([string]$Port) {
    $infoDirs = @(
        (Join-Path $Repo 'build\x64\Release\vcpkg_installed\vcpkg\info'),
        (Join-Path $Repo 'vcpkg_installed\vcpkg\info')
    )
    foreach ($dir in $infoDirs) {
        $file = Get-ChildItem -LiteralPath $dir -Filter "${Port}_*.list" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($file.Name -match "^$([Regex]::Escape($Port))_(.+)_x64-") { return $Matches[1] }
    }
    return 'unknown'
}

function Get-PowerScheme {
    $text = (powercfg /getactivescheme 2>&1) -join ' '
    if ($LASTEXITCODE -ne 0 -or $text -notmatch '([0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})') {
        throw "Cannot read the active Windows power scheme: $text"
    }
    return [pscustomobject]@{ Guid = $Matches[1].ToLowerInvariant(); Text = $text.Trim() }
}

function Get-AutomaticThreadBudget([int]$HardwareThreads) {
    if ($HardwareThreads -le 1) { return 1 }
    if ($HardwareThreads -ge 12) { return [Math]::Min($HardwareThreads - 4, 128) }
    if ($HardwareThreads -ge 5) { return $HardwareThreads - 2 }
    return $HardwareThreads - 1
}

function Get-ExpectedEncoderThreads([int]$Budget, [int]$FileCount) {
    if ($FileCount -gt 12) { return 1 }
    $desired = [Math]::Min($FileCount, $Budget)
    for ($parallel = $desired; $parallel -ge 1; $parallel--) {
        if (($Budget % $parallel) -eq 0) { return [int]($Budget / $parallel) }
    }
    return $Budget
}

$Version = (Get-Content -LiteralPath (Join-Path $Repo 'VERSION') -Raw).Trim()
$GitCommit = Invoke-Git @('rev-parse', 'HEAD')
$GitStatus = @(& git -C $Repo status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'git status failed.' }
$GeneratedReleaseFiles = @(
    'bin/x64/Release/AWJ',
    'bin/x64/Release/AWJ.com',
    'bin/x64/Release/AWJ.exe',
    'bin/x64/Release/AWJ.sha256',
    'bin/x64/Release/AWJ.com.sha256',
    'bin/x64/Release/AWJ.exe.sha256',
    'bin/x64/Release/BUILD_INFO.txt',
    'bin/x64/Release/LICENSE',
    'bin/x64/Release/THIRD_PARTY_NOTICES.txt'
)
$SourceChanges = @($GitStatus | Where-Object {
    $path = Get-GitStatusPath $_
    $path -notin $GeneratedReleaseFiles
})
$GeneratedReleaseChanges = @($GitStatus | Where-Object {
    $path = Get-GitStatusPath $_
    $path -in $GeneratedReleaseFiles
})
$GitDirty = $SourceChanges.Count -gt 0
$BuildInfoPath = Join-Path (Split-Path -Parent $Executable) 'BUILD_INFO.txt'
$BuildCommit = Get-BuildInfoValue $BuildInfoPath 'Git Commit'
$BuildVersion = if (Test-Path -LiteralPath $BuildInfoPath) {
    ((Get-Content -LiteralPath $BuildInfoPath -First 1) -replace '^AWJimage\s+', '').Trim()
} else { '' }

if (-not $Smoke) {
    if (-not $PowerSchemeGuid) { throw 'Canonical benchmarks require -PowerSchemeGuid.' }
    if ($GitDirty) { throw 'Canonical benchmarks require a clean worktree.' }
    if (-not $BuildCommit -or $BuildCommit -ne $GitCommit) {
        throw "BUILD_INFO commit '$BuildCommit' does not match workspace commit '$GitCommit'. Rebuild Release first."
    }
    if ($BuildVersion -ne $Version) {
        throw "BUILD_INFO version '$BuildVersion' does not match VERSION '$Version'."
    }
    if ((Get-BuildInfoValue $BuildInfoPath 'Build Type') -ne 'Release') {
        throw 'BUILD_INFO does not identify a Release build. Rebuild with release.ps1.'
    }
} elseif ($BuildCommit -and $BuildCommit -ne $GitCommit) {
    Write-Warning "Smoke run uses build commit '$BuildCommit', workspace is '$GitCommit'."
}

$Power = Get-PowerScheme
if ($PowerSchemeGuid) {
    $expectedPower = $PowerSchemeGuid.Trim('{}').ToLowerInvariant()
    if ($Power.Guid -ne $expectedPower) {
        throw "Active power scheme is $($Power.Guid), expected $expectedPower."
    }
}
$LockedPowerGuid = $Power.Guid
$HardwareThreads = [Environment]::ProcessorCount
$ThreadBudget = Get-AutomaticThreadBudget $HardwareThreads
$AomVersion = Get-BuildInfoValue $BuildInfoPath 'AOM'
if (-not $AomVersion) { $AomVersion = Get-VcpkgPortVersion 'aom' }
$Dav1dVersion = Get-BuildInfoValue $BuildInfoPath 'dav1d'
if (-not $Dav1dVersion) { $Dav1dVersion = Get-VcpkgPortVersion 'dav1d' }
$LibavifCommit = if ((Get-Content -LiteralPath (Join-Path $Repo 'CMakeLists.txt') -Raw) -match 'set\(AWJ_LIBAVIF_GIT_TAG\s+"?([^"\s\)]+)') { $Matches[1] } else { 'unknown' }
$ExecutableSha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash

$SupportedExtensions = @('.jpg', '.jpeg', '.jpe', '.jfif', '.png', '.webp', '.bmp', '.dib', '.rle', '.tif', '.tiff', '.gif', '.jxl', '.avif')
$Magick = (Get-Command magick -ErrorAction Stop).Source

function Get-AlphaClass([IO.FileInfo]$File) {
    if ($File.Length -eq 0) { return 'invalid' }
    if ($File.Extension.ToLowerInvariant() -in @('.jpg', '.jpeg', '.jpe', '.jfif')) { return 'opaque' }
    $frame = $File.FullName + '[0]'
    $text = (& $Magick $frame -alpha extract -format '%[fx:minima]' 'info:' 2>$null) -join ''
    if ($LASTEXITCODE -ne 0) { return 'unknown' }
    [double]$minimum = 0
    if (-not [double]::TryParse($text.Trim(), [Globalization.NumberStyles]::Float, $Invariant, [ref]$minimum)) {
        return 'unknown'
    }
    return $(if ($minimum -lt 0.999999) { 'transparent' } else { 'opaque' })
}

function New-InputInventory {
    $paths = [string[]]@(Get-ChildItem -LiteralPath $InputRoot -File -Recurse |
        Where-Object { $_.Extension.ToLowerInvariant() -in $SupportedExtensions } |
        ForEach-Object FullName)
    [Array]::Sort($paths, [StringComparer]::OrdinalIgnoreCase)
    $items = [Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $paths.Count; $i++) {
        Write-Progress -Activity 'Fingerprinting benchmark input' -Status "$($i + 1) / $($paths.Count)" -PercentComplete (100 * ($i + 1) / $paths.Count)
        $file = Get-Item -LiteralPath $paths[$i]
        $items.Add([pscustomobject]@{
            Index = $i
            FullPath = $file.FullName
            RelativePath = [IO.Path]::GetRelativePath($InputRoot, $file.FullName)
            Bytes = $file.Length
            LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o', $Invariant)
            SHA256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            AlphaClass = Get-AlphaClass $file
        })
    }
    Write-Progress -Activity 'Fingerprinting benchmark input' -Completed
    return @($items)
}

$Inventory = @(New-InputInventory)
if ($Inventory.Count -eq 0) { throw "No supported images found in '$InputRoot'." }
if (-not $Smoke -and 613 -in $Count -and $Inventory.Count -ne 613) {
    throw "Canonical 613-image benchmark requires exactly 613 inputs; found $($Inventory.Count)."
}
$InventoryPath = Join-Path $ResultsRoot 'input-manifest.csv'
$Inventory | Select-Object Index, RelativePath, Bytes, LastWriteTimeUtc, SHA256, AlphaClass |
    Export-Csv -LiteralPath $InventoryPath -NoTypeInformation -Encoding utf8NoBOM
$FingerprintPath = Join-Path $ResultsRoot 'input-files.sha256'
$Inventory | ForEach-Object { "$($_.SHA256) *$($_.RelativePath)" } |
    Set-Content -LiteralPath $FingerprintPath -Encoding utf8NoBOM
$InventorySha256 = (Get-FileHash -LiteralPath $FingerprintPath -Algorithm SHA256).Hash

$InputCache = @{}
$SelectionRoot = Join-Path $ResultsRoot 'selections'
New-Item -ItemType Directory -Path $SelectionRoot -Force | Out-Null

function Get-StagedSelection([string]$AlphaClass, [int]$FileCount) {
    $key = "$AlphaClass-$FileCount"
    if ($InputCache.ContainsKey($key)) { return @($InputCache[$key]) }
    $pool = switch ($AlphaClass) {
        'transparent' { @($Inventory | Where-Object AlphaClass -eq 'transparent') }
        'opaque' { @($Inventory | Where-Object AlphaClass -eq 'opaque') }
        default {
            if ($FileCount -eq $Inventory.Count) { @($Inventory) }
            else { @($Inventory | Where-Object AlphaClass -ne 'invalid') }
        }
    }
    $selected = @(Select-Evenly $pool $FileCount)
    $inputDir = Join-Path $SelectionRoot "$key-input"
    New-Item -ItemType Directory -Path $inputDir -Force | Out-Null
    $staged = [Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $selected.Count; $i++) {
        $extension = [IO.Path]::GetExtension($selected[$i].FullPath).ToLowerInvariant()
        $target = Join-Path $inputDir ('{0:D6}{1}' -f ($i + 1), $extension)
        try { [IO.File]::CreateHardLink($target, $selected[$i].FullPath) }
        catch { Copy-Item -LiteralPath $selected[$i].FullPath -Destination $target }
        $staged.Add([pscustomobject]@{
            Index = $i
            SourcePath = $selected[$i].FullPath
            RelativePath = $selected[$i].RelativePath
            StagedPath = $target
            Bytes = $selected[$i].Bytes
            SHA256 = $selected[$i].SHA256
            AlphaClass = $selected[$i].AlphaClass
        })
    }
    $selectionCsv = Join-Path $SelectionRoot "$key.csv"
    $staged | Export-Csv -LiteralPath $selectionCsv -NoTypeInformation -Encoding utf8NoBOM
    $InputCache[$key] = @($staged)
    return @($staged)
}

function Write-ManifestText([IO.BinaryWriter]$Writer, [string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false, $true).GetBytes($Text)
    $Writer.Write([uint32]$bytes.Length)
    $Writer.Write($bytes)
}

function Write-StudioManifest([string]$Path, [object[]]$Selection, [string]$OutputDir) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $writer = [IO.BinaryWriter]::new($stream, [Text.UTF8Encoding]::new($false), $false)
    try {
        $writer.Write([byte[]](65, 87, 74, 83, 81, 77, 70, 0))
        $writer.Write([uint32]1)
        $writer.Write([uint64]$Selection.Count)
        for ($i = 0; $i -lt $Selection.Count; $i++) {
            $writer.Write([uint64]$i)
            $writer.Write([uint64]$Selection[$i].Bytes)
            $writer.Write([uint32]2)
            $outputPath = Join-Path $OutputDir (([IO.Path]::GetFileNameWithoutExtension($Selection[$i].StagedPath)) + '.avif')
            $fields = @($Selection[$i].StagedPath, '', '', '', '', '', '', '', '', '', $outputPath)
            foreach ($field in $fields) { Write-ManifestText $writer $field }
        }
    } finally {
        $writer.Dispose()
    }
}

function Invoke-Awj([string[]]$Arguments) {
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Executable
    $psi.WorkingDirectory = Split-Path -Parent $Executable
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $psi.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in $Arguments) { [void]$psi.ArgumentList.Add($argument) }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $clock = [Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw 'Failed to start AWJ.exe.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    [long]$peak = 0
    while (-not $process.WaitForExit(100)) {
        try {
            $process.Refresh()
            $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
        } catch { }
    }
    $process.WaitForExit()
    $clock.Stop()
    try {
        $process.Refresh()
        $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
    } catch { }
    try { $cpuSeconds = $process.TotalProcessorTime.TotalSeconds } catch { $cpuSeconds = [double]::NaN }
    if ([double]::IsNaN($cpuSeconds) -or $peak -le 0) { throw 'Windows process CPU or peak-memory accounting failed.' }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        WallSeconds = $clock.Elapsed.TotalSeconds
        CpuSeconds = $cpuSeconds
        PeakWorkingSetBytes = $peak
        Stdout = $stdoutTask.GetAwaiter().GetResult()
        Stderr = $stderrTask.GetAwaiter().GetResult()
    }
}

function Get-CsvSum([object[]]$Rows, [string]$Property) {
    [double]$sum = 0
    foreach ($row in $Rows) {
        $text = [string]$row.$Property
        [double]$value = 0
        if ($text -and [double]::TryParse($text, [Globalization.NumberStyles]::Float, $Invariant, [ref]$value)) { $sum += $value }
    }
    return $sum
}

function Assert-RunSummary([object[]]$Rows, [object[]]$Selection, [int]$ExitCode) {
    if ($Rows.Count -ne $Selection.Count) { throw "summary.csv has $($Rows.Count) rows, expected $($Selection.Count)." }
    $failed = @($Rows | Where-Object status -eq 'failed').Count
    $expectedFailed = @($Selection | Where-Object AlphaClass -eq 'invalid').Count
    if ($failed -ne $expectedFailed) { throw "Run has $failed failures, expected $expectedFailed from the fixed input manifest." }
    $expectedExit = if ($expectedFailed -eq 0) { 0 } else { 2 }
    if ($ExitCode -ne $expectedExit) { throw "AWJ exit code is $ExitCode, expected $expectedExit." }

    $versions = @($Rows.awj_version | Sort-Object -Unique)
    if ($versions.Count -ne 1 -or $versions[0] -ne $Version) { throw "summary.csv AWJ version is '$($versions -join ',')', expected '$Version'." }
    $expectedThreads = Get-ExpectedEncoderThreads $ThreadBudget $Selection.Count
    foreach ($row in @($Rows | Where-Object status -eq 'ok')) {
        if ($row.format -ne 'AVIF' -or $row.user_encoder -ne 'aom' -or $row.user_chroma -ne $Chroma -or
            [int]$row.quality -ne $Quality -or [int]$row.speed -ne $Speed -or
            [int]$row.applied_bit_depth -ne $BitDepth -or
            [int]$row.encoder_threads -ne $expectedThreads) {
            throw "Applied settings drifted for '$($row.input)'."
        }
        if ($row.has_non_opaque_alpha -eq 'true') {
            if ($row.lossless -ne 'true' -or $row.encoder_selected -ne 'aom' -or $row.applied_chroma -ne '444' -or
                [int]$row.final_encoder_quality -ne 100) {
                throw "Transparent AVIF invariant failed for '$($row.input)'."
            }
        } elseif ($row.applied_chroma -ne $Chroma -or [int]$row.final_encoder_quality -ne $Quality) {
            throw "Opaque AVIF settings drifted for '$($row.input)'."
        }
    }
}

$RunsPerCase = if ($Smoke) { 1 } else { 5 }
$WarmupsPerCase = if ($Smoke) { 0 } else { 1 }
if ($Smoke) { $CooldownSeconds = 0 }

$Cases = [Collections.Generic.List[object]]::new()
foreach ($surfaceName in @($Surface | Select-Object -Unique)) {
    foreach ($fileCount in @($Count | Select-Object -Unique)) {
        $Cases.Add([pscustomobject]@{ Surface = $surfaceName; AlphaClass = 'mixed'; Count = $fileCount })
    }
    foreach ($alphaCount in @($AlphaCoverageCount | Where-Object { $_ -gt 0 } | Select-Object -Unique)) {
        $Cases.Add([pscustomobject]@{ Surface = $surfaceName; AlphaClass = 'opaque'; Count = $alphaCount })
        $Cases.Add([pscustomobject]@{ Surface = $surfaceName; AlphaClass = 'transparent'; Count = $alphaCount })
    }
}

$RawRoot = Join-Path $ResultsRoot 'raw'
New-Item -ItemType Directory -Path $RawRoot -Force | Out-Null
$AllRuns = [Collections.Generic.List[object]]::new()

foreach ($case in $Cases) {
    $caseId = "$($case.Surface)-$($case.AlphaClass)-$($case.Count)"
    $selection = @(Get-StagedSelection $case.AlphaClass $case.Count)
    $inputDir = Split-Path -Parent $selection[0].StagedPath
    $caseRaw = Join-Path $RawRoot $caseId
    New-Item -ItemType Directory -Path $caseRaw -Force | Out-Null
    $iterations = $WarmupsPerCase + $RunsPerCase
    for ($iteration = 0; $iteration -lt $iterations; $iteration++) {
        $isWarmup = $iteration -lt $WarmupsPerCase
        $runNumber = if ($isWarmup) { 0 } else { $iteration - $WarmupsPerCase + 1 }
        $label = if ($isWarmup) { 'warmup' } else { "run-$runNumber" }
        Write-Host "[$caseId] $label"

        $currentPower = Get-PowerScheme
        if ($currentPower.Guid -ne $LockedPowerGuid) { throw 'Windows power scheme changed during the benchmark.' }
        $outputDir = Join-Path $ResultsRoot "work\$caseId-$label"
        New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
        $common = @(
            '--input', $inputDir,
            '--output', $outputDir,
            '--format', 'avif',
            '--avif-encoder', 'aom',
            '--quality', [string]$Quality,
            '--speed', [string]$Speed,
            '--chroma', $Chroma,
            '--bit-depth', [string]$BitDepth,
            '--alpha', 'auto',
            '--threads', 'auto',
            '--memory-limit', 'auto',
            '--image-size-limit', 'none',
            '--template', '{name}',
            '--timeout-encode', '120',
            '--collision', 'overwrite',
            '--keep-metadata',
            '--no-wic-fallback',
            '--experimental-encoders',
            '--no-experimental-clamped-grid-padding',
            '--large-image-priority', 'zenrav1e',
            '--no-unlock-max-input-file-bytes',
            '--summary',
            '--no-log'
        )
        $arguments = switch ($case.Surface) {
            'studio' {
                $manifest = Join-Path $outputDir 'studio.awjq'
                Write-StudioManifest $manifest $selection $outputDir
                @($common + @('--studio-queue-manifest', $manifest))
            }
            'shell' { @('--shell-convert') + $common }
            default { $common }
        }

        $process = Invoke-Awj $arguments
        $summaryPath = Join-Path $outputDir 'summary.csv'
        if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
            throw "AWJ did not produce summary.csv. Exit=$($process.ExitCode); stderr=$($process.Stderr)"
        }
        $rows = @(Import-Csv -LiteralPath $summaryPath)
        Assert-RunSummary $rows $selection $process.ExitCode

        Copy-Item -LiteralPath $summaryPath -Destination (Join-Path $caseRaw "$label-summary.csv")
        Set-Content -LiteralPath (Join-Path $caseRaw "$label.stdout.log") -Value $process.Stdout -Encoding utf8NoBOM
        Set-Content -LiteralPath (Join-Path $caseRaw "$label.stderr.log") -Value $process.Stderr -Encoding utf8NoBOM

        $decode = Get-CsvSum $rows 'decode_seconds'
        $prepare = Get-CsvSum $rows 'prepare_seconds'
        $encode = Get-CsvSum $rows 'encode_seconds'
        $write = Get-CsvSum $rows 'write_seconds'
        $stageTotal = $decode + $prepare + $encode + $write
        $AllRuns.Add([pscustomobject]@{
            CaseId = $caseId
            Surface = $case.Surface
            AlphaClass = $case.AlphaClass
            FileCount = $case.Count
            Warmup = $isWarmup
            Run = $runNumber
            ExitCode = $process.ExitCode
            WallSeconds = $process.WallSeconds
            CpuSeconds = $process.CpuSeconds
            PeakWorkingSetBytes = $process.PeakWorkingSetBytes
            ThroughputImagesPerSecond = $case.Count / $process.WallSeconds
            CoreSeconds = Get-CsvSum $rows 'seconds'
            DecodeSeconds = $decode
            PrepareSeconds = $prepare
            EncodeSeconds = $encode
            WriteSeconds = $write
            EncodeStagePercent = if ($stageTotal -gt 0) { 100 * $encode / $stageTotal } else { 0 }
            SuccessCount = @($rows | Where-Object status -eq 'ok').Count
            FailedCount = @($rows | Where-Object status -eq 'failed').Count
            EncoderIds = (@($rows.encoder_id | Where-Object { $_ } | Sort-Object -Unique) -join '+')
            DecoderIds = (@($rows.decoder_id | Where-Object { $_ } | Sort-Object -Unique) -join '+')
            EncoderThreads = (@($rows.encoder_threads | Where-Object { $_ } | Sort-Object -Unique) -join '+')
            AwjVersion = $Version
            BuildCommit = $BuildCommit
            WorkspaceCommit = $GitCommit
            AomVersion = $AomVersion
            LibavifCommit = $LibavifCommit
            Dav1dVersion = $Dav1dVersion
            PowerSchemeGuid = $LockedPowerGuid
        })
        Remove-Item -LiteralPath $outputDir -Recurse -Force
        if ($iteration -lt ($iterations - 1) -and $CooldownSeconds -gt 0) {
            Start-Sleep -Seconds $CooldownSeconds
        }
    }
}

$RunsPath = Join-Path $ResultsRoot 'runs.csv'
$AllRuns | Export-Csv -LiteralPath $RunsPath -NoTypeInformation -Encoding utf8NoBOM
$Measured = @($AllRuns | Where-Object { -not $_.Warmup })
$Aggregates = [Collections.Generic.List[object]]::new()
foreach ($case in $Cases) {
    $caseId = "$($case.Surface)-$($case.AlphaClass)-$($case.Count)"
    $runs = @($Measured | Where-Object CaseId -eq $caseId)
    $Aggregates.Add([pscustomobject]@{
        CaseId = $caseId
        Surface = $case.Surface
        AlphaClass = $case.AlphaClass
        FileCount = $case.Count
        Runs = $runs.Count
        WallMedianSeconds = Get-Median @($runs.WallSeconds)
        WallP95Seconds = Get-P95 @($runs.WallSeconds)
        CpuMedianSeconds = Get-Median @($runs.CpuSeconds)
        CpuP95Seconds = Get-P95 @($runs.CpuSeconds)
        PeakMemoryMedianBytes = Get-Median @($runs.PeakWorkingSetBytes)
        PeakMemoryP95Bytes = Get-P95 @($runs.PeakWorkingSetBytes)
        ThroughputMedianImagesPerSecond = Get-Median @($runs.ThroughputImagesPerSecond)
        ThroughputP95ImagesPerSecond = Get-P95 @($runs.ThroughputImagesPerSecond)
        CoreMedianSeconds = Get-Median @($runs.CoreSeconds)
        CoreP95Seconds = Get-P95 @($runs.CoreSeconds)
        DecodeMedianSeconds = Get-Median @($runs.DecodeSeconds)
        PrepareMedianSeconds = Get-Median @($runs.PrepareSeconds)
        EncodeMedianSeconds = Get-Median @($runs.EncodeSeconds)
        WriteMedianSeconds = Get-Median @($runs.WriteSeconds)
        EncodeStageMedianPercent = Get-Median @($runs.EncodeStagePercent)
        SuccessCount = $runs[0].SuccessCount
        FailedCount = $runs[0].FailedCount
        EncoderIds = $runs[0].EncoderIds
        EncoderThreads = $runs[0].EncoderThreads
    })
}

$SummaryPath = Join-Path $ResultsRoot 'summary.csv'
$Aggregates | Export-Csv -LiteralPath $SummaryPath -NoTypeInformation -Encoding utf8NoBOM

try { $CpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name).Trim() }
catch { $CpuName = 'unknown' }
$Metadata = [pscustomobject]@{
    canonical = -not [bool]$Smoke
    generated_at = [DateTimeOffset]::Now.ToString('o', $Invariant)
    input_root = $InputRoot
    input_count = $Inventory.Count
    input_manifest_sha256 = $InventorySha256
    executable = $Executable
    executable_sha256 = $ExecutableSha256
    build_type = 'Release'
    awj_version = $Version
    build_commit = $BuildCommit
    workspace_commit = $GitCommit
    workspace_dirty = $GitDirty
    generated_release_changes = $GeneratedReleaseChanges
    encoder_versions = [pscustomobject]@{ aom = $AomVersion; libavif_commit = $LibavifCommit; dav1d = $Dav1dVersion }
    profile = [pscustomobject]@{ format = 'avif'; encoder = 'aom'; quality = $Quality; speed = $Speed; chroma = $Chroma; bit_depth = $BitDepth; alpha = 'auto'; threads = 'auto'; memory = 'auto' }
    protocol = [pscustomobject]@{ warmups = $WarmupsPerCase; measured_runs = $RunsPerCase; cooldown_seconds = $CooldownSeconds; p95 = 'nearest-rank' }
    machine = [pscustomobject]@{ cpu = $CpuName; hardware_threads = $HardwareThreads; automatic_thread_budget = $ThreadBudget; power_scheme_guid = $LockedPowerGuid; power_scheme = $Power.Text }
}
$MetadataPath = Join-Path $ResultsRoot 'metadata.json'
$Metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $MetadataPath -Encoding utf8NoBOM

function Format-Number([double]$Value, [string]$Pattern = 'F3') { return $Value.ToString($Pattern, $Invariant) }
$Report = [Collections.Generic.List[string]]::new()
$Report.Add('# AWJ reproducible benchmark')
$Report.Add('')
$Report.Add("Canonical: **$(-not [bool]$Smoke)**  ")
$Report.Add("AWJ: **$Version**  ")
$Report.Add("Commit: **$BuildCommit**  ")
$Report.Add("Encoder: **libaom $AomVersion / libavif $($LibavifCommit.Substring(0, [Math]::Min(12, $LibavifCommit.Length)))**  ")
$Report.Add("Input manifest: **$InventorySha256**  ")
$Report.Add("Power: **$LockedPowerGuid**")
$Report.Add('')
$Report.Add("Profile: AVIF, AOM, quality=$Quality, speed=$Speed, chroma=$Chroma, bit-depth=$BitDepth, alpha=auto, Release, $WarmupsPerCase warmup and $RunsPerCase measured runs.")
$Report.Add('')
$Report.Add('| Case | Wall median / P95 (s) | Process CPU median / P95 (s) | Item seconds sum median / P95 | Peak median / P95 (MiB) | Throughput median (img/s) | decode / prepare / encode / write median (s) | Encode share |')
$Report.Add('|---|---:|---:|---:|---:|---:|---:|---:|')
foreach ($row in $Aggregates) {
    $peakMedian = $row.PeakMemoryMedianBytes / 1MB
    $peakP95 = $row.PeakMemoryP95Bytes / 1MB
    $Report.Add("| $($row.CaseId) | $(Format-Number $row.WallMedianSeconds) / $(Format-Number $row.WallP95Seconds) | $(Format-Number $row.CpuMedianSeconds) / $(Format-Number $row.CpuP95Seconds) | $(Format-Number $row.CoreMedianSeconds) / $(Format-Number $row.CoreP95Seconds) | $(Format-Number $peakMedian 'F1') / $(Format-Number $peakP95 'F1') | $(Format-Number $row.ThroughputMedianImagesPerSecond) | $(Format-Number $row.DecodeMedianSeconds) / $(Format-Number $row.PrepareMedianSeconds) / $(Format-Number $row.EncodeMedianSeconds) / $(Format-Number $row.WriteMedianSeconds) | $(Format-Number $row.EncodeStageMedianPercent 'F1')% |")
}
$ReportPath = Join-Path $ResultsRoot 'report.md'
$Report | Set-Content -LiteralPath $ReportPath -Encoding utf8NoBOM

Write-Host "Benchmark complete: $ReportPath"
Write-Host "Raw runs:          $RunsPath"
Write-Host "Aggregate CSV:      $SummaryPath"
Write-Host "Metadata:           $MetadataPath"
