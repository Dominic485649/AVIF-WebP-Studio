#requires -Version 7.4

[CmdletBinding()]
param(
    [string]$InputRoot = 'D:\图片\benchmark\test',
    [string]$AwjExecutable = '',
    [string]$LegacyAwjExecutable = '',
    [string]$DependencyBaselineAwjExecutable = '',
    [string]$FfmpegExecutable = '',
    [string]$MagickExecutable = '',
    [string]$ResultsRoot = '',
    [ValidateSet('All', 'Regression', 'Strict')]
    [string]$Mode = 'All',
    [ValidateRange(0, 300)]
    [int]$CooldownSeconds = 30,
    [string]$PowerSchemeGuid = '',
    [switch]$Smoke,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$AwjQuality = 70
$AwjSpeed = 6
$FfmpegAomQuantizer = 23
$WarmupRuns = if ($Smoke) { 0 } else { 1 }
$MeasuredRuns = if ($Smoke) { 1 } else { 5 }
$StrictCount = if ($Smoke) { 2 } else { 210 }

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { throw 'Median requires values.' }
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    $middle = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
}

function Get-P95([double[]]$Values) {
    if ($Values.Count -eq 0) { throw 'P95 requires values.' }
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    return $sorted[[Math]::Ceiling(0.95 * $sorted.Count) - 1]
}

function Get-AutomaticThreadBudget([int]$HardwareThreads) {
    if ($HardwareThreads -ge 12) { return $HardwareThreads - 4 }
    if ($HardwareThreads -ge 5) { return $HardwareThreads - 2 }
    if ($HardwareThreads -ge 2) { return $HardwareThreads - 1 }
    return 1
}

function Get-ExpectedEncoderThreads([int]$Budget, [int]$FileCount) {
    if ($FileCount -gt 12) { return 1 }
    $desired = [Math]::Min([Math]::Max(1, $FileCount), $Budget)
    for ($files = $desired; $files -ge 1; --$files) {
        if (($Budget % $files) -eq 0) { return [int]($Budget / $files) }
    }
    return $Budget
}

function Get-ExpectedAutoChroma([string]$SourceChroma) {
    if ($SourceChroma -in @('420', '422', '444')) { return $SourceChroma }
    return '420'
}

function New-AwjArguments([string]$InputDirectory, [string]$OutputDirectory,
                          [bool]$LegacyTenBit) {
    $arguments = @('--input', $InputDirectory, '--output', $OutputDirectory,
                   '--summary', '--no-log')
    if ($LegacyTenBit) { $arguments += @('--bit-depth', '10') }
    return $arguments
}

function Invoke-SelfTest {
    if ((Get-Median @(5, 1, 3, 2, 4)) -ne 3) { throw 'Median self-test failed.' }
    if ((Get-P95 @(5, 1, 3, 2, 4)) -ne 5) { throw 'P95 self-test failed.' }
    $hardware = @(1, 2, 4, 5, 11, 12, 16)
    $expected = @(1, 1, 3, 3, 9, 8, 12)
    for ($index = 0; $index -lt $hardware.Count; ++$index) {
        if ((Get-AutomaticThreadBudget $hardware[$index]) -ne $expected[$index]) {
            throw "Thread budget self-test failed for $($hardware[$index])."
        }
    }
    if ((Get-ExpectedEncoderThreads 12 1) -ne 12 -or
        (Get-ExpectedEncoderThreads 12 4) -ne 3 -or
        (Get-ExpectedEncoderThreads 12 12) -ne 1 -or
        (Get-ExpectedEncoderThreads 12 13) -ne 1) {
        throw 'Resource plan self-test failed.'
    }
    if ((Get-ExpectedAutoChroma '420') -ne '420' -or
        (Get-ExpectedAutoChroma '422') -ne '422' -or
        (Get-ExpectedAutoChroma '444') -ne '444' -or
        (Get-ExpectedAutoChroma 'unknown') -ne '420') {
        throw 'Auto chroma self-test failed.'
    }
    if (((New-AwjArguments 'in' 'out' $false) -join '|') -ne
        '--input|in|--output|out|--summary|--no-log') {
        throw 'Default argument self-test failed.'
    }
    if (((New-AwjArguments 'in' 'out' $true) -join '|') -ne
        '--input|in|--output|out|--summary|--no-log|--bit-depth|10') {
        throw 'Legacy argument self-test failed.'
    }
    if ($AwjQuality -ne 70 -or $AwjSpeed -ne 6 -or $FfmpegAomQuantizer -ne 23) {
        throw 'Profile self-test failed.'
    }
    Write-Host 'benchmark.ps1 self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

if (-not $IsWindows) { throw 'This benchmark requires Windows.' }

function Resolve-Executable([string]$Value, [string]$Command, [string]$Default = '') {
    if ($Value) { return (Resolve-Path -LiteralPath $Value).Path }
    if ($Default) { return (Resolve-Path -LiteralPath $Default).Path }
    return (Get-Command $Command -ErrorAction Stop).Source
}

function Get-PowerScheme {
    $text = (powercfg /getactivescheme 2>&1) -join ' '
    if ($LASTEXITCODE -ne 0 -or
        $text -notmatch '([0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})') {
        throw "Cannot read active Windows power scheme: $text"
    }
    return [pscustomobject]@{ Guid = $Matches[1].ToLowerInvariant(); Text = $text.Trim() }
}

function Get-BuildInfoValue([string]$Path, [string]$Key) {
    $prefix = $Key + ':'
    $line = Get-Content -LiteralPath $Path | Where-Object {
        $_.StartsWith($prefix, [StringComparison]::Ordinal)
    } | Select-Object -First 1
    if (-not $line) { return '' }
    return ([string]$line).Substring($prefix.Length).Trim()
}

function Get-AwjDescriptor([string]$Role, [string]$Path, [bool]$LegacyTenBit) {
    $infoPath = Join-Path (Split-Path -Parent $Path) 'BUILD_INFO.txt'
    if (-not (Test-Path -LiteralPath $infoPath -PathType Leaf)) {
        throw "$Role has no BUILD_INFO.txt next to its executable."
    }
    $version = ((Get-Content -LiteralPath $infoPath -First 1) -replace '^AWJimage\s+', '').Trim()
    $type = Get-BuildInfoValue $infoPath 'Build Type'
    $commit = Get-BuildInfoValue $infoPath 'Git Commit'
    $aom = Get-BuildInfoValue $infoPath 'AOM'
    if (-not $version -or $type -ne 'Release' -or -not $commit -or -not $aom) {
        throw "$Role BUILD_INFO must identify version, Release, Git Commit, and AOM."
    }
    if (-not $Smoke -and $commit.EndsWith('-dirty', [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Role is a dirty build. Canonical data requires a clean build."
    }
    return [pscustomobject]@{
        Role = $Role
        Path = $Path
        SHA256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        Version = $version
        BuildType = $type
        Commit = $commit
        Aom = $aom
        Dav1d = Get-BuildInfoValue $infoPath 'dav1d'
        Libyuv = Get-BuildInfoValue $infoPath 'libyuv'
        VcpkgBaseline = Get-BuildInfoValue $infoPath 'Vcpkg baseline'
        LegacyTenBit = $LegacyTenBit
        BuildInfo = (Get-Content -LiteralPath $infoPath -Raw).TrimEnd()
    }
}

function Add-BenchmarkJobType {
    if ('AwjBenchmarkJob' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public sealed class AwjBenchmarkChild : IDisposable
{
    const uint WAIT_OBJECT_0 = 0;
    const uint WAIT_TIMEOUT = 258;
    const uint INFINITE = 0xffffffff;
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    IntPtr process;
    IntPtr thread;
    readonly int id;
    internal AwjBenchmarkChild(IntPtr p, IntPtr t, uint processId) { process = p; thread = t; id = checked((int)processId); }
    public bool HasExited {
        get {
            if (process == IntPtr.Zero) return true;
            uint value = WaitForSingleObject(process, 0);
            if (value == WAIT_OBJECT_0) return true;
            if (value == WAIT_TIMEOUT) return false;
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
    }
    public int ProcessId { get { return id; } }
    public int ExitCode {
        get {
            uint code;
            if (process == IntPtr.Zero || !GetExitCodeProcess(process, out code)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return unchecked((int)code);
        }
    }
    public void Wait() {
        if (process == IntPtr.Zero) return;
        if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public void Dispose() {
        if (thread != IntPtr.Zero) CloseHandle(thread);
        if (process != IntPtr.Zero) CloseHandle(process);
        thread = IntPtr.Zero;
        process = IntPtr.Zero;
    }
}

public sealed class AwjBenchmarkJob : IDisposable
{
    const uint CREATE_SUSPENDED = 0x00000004;
    const uint CREATE_NO_WINDOW = 0x08000000;
    const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    const int JobObjectBasicAccountingInformation = 1;
    const int JobObjectExtendedLimitInformation = 9;
    [StructLayout(LayoutKind.Sequential)] struct IO_COUNTERS { public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount; }
    [StructLayout(LayoutKind.Sequential)] struct BASIC_LIMIT { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential)] struct EXTENDED_LIMIT { public BASIC_LIMIT BasicLimitInformation; public IO_COUNTERS IoInfo; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }
    [StructLayout(LayoutKind.Sequential)] struct ACCOUNTING { public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime; public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct STARTUPINFO { public uint cb; public string lpReserved, lpDesktop, lpTitle; public uint dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public ushort wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public uint dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateJobObject(IntPtr a, string n);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr h, int c, ref EXTENDED_LIMIT i, uint s);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr h, int c, out EXTENDED_LIMIT i, uint s, IntPtr r);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr h, int c, out ACCOUNTING i, uint s, IntPtr r);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr h, uint c);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcess(string app, StringBuilder command, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO startup, out PROCESS_INFORMATION created);
    IntPtr job;
    public AwjBenchmarkJob() {
        job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        var info = new EXTENDED_LIMIT();
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, checked((uint)Marshal.SizeOf(info)))) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public AwjBenchmarkChild Start(string executable, string[] arguments, string cwd) {
        var startup = new STARTUPINFO();
        startup.cb = checked((uint)Marshal.SizeOf(startup));
        PROCESS_INFORMATION created;
        var command = new StringBuilder(BuildCommandLine(executable, arguments));
        if (!CreateProcess(executable, command, IntPtr.Zero, IntPtr.Zero, false, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, IntPtr.Zero, cwd, ref startup, out created)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            if (!AssignProcessToJobObject(job, created.hProcess)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (ResumeThread(created.hThread) == UInt32.MaxValue) throw new Win32Exception(Marshal.GetLastWin32Error());
            return new AwjBenchmarkChild(created.hProcess, created.hThread, created.dwProcessId);
        } catch {
            CloseHandle(created.hThread);
            CloseHandle(created.hProcess);
            throw;
        }
    }
    public double CpuSeconds {
        get {
            ACCOUNTING info;
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, out info, checked((uint)Marshal.SizeOf(typeof(ACCOUNTING))), IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return (info.TotalUserTime + info.TotalKernelTime) / 10000000.0;
        }
    }
    public long PeakMemoryBytes {
        get {
            EXTENDED_LIMIT info;
            if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, out info, checked((uint)Marshal.SizeOf(typeof(EXTENDED_LIMIT))), IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            ulong bytes = info.PeakJobMemoryUsed.ToUInt64();
            return bytes > Int64.MaxValue ? Int64.MaxValue : checked((long)bytes);
        }
    }
    static string BuildCommandLine(string executable, string[] arguments) {
        var result = new StringBuilder(Quote(executable));
        foreach (string argument in arguments) result.Append(' ').Append(Quote(argument));
        return result.ToString();
    }
    static string Quote(string value) {
        if (value.Length > 0 && value.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0) return value;
        var result = new StringBuilder("\"");
        int slashCount = 0;
        foreach (char c in value) {
            if (c == '\\') { ++slashCount; continue; }
            if (c == '"') { result.Append('\\', slashCount * 2 + 1).Append('"'); slashCount = 0; continue; }
            result.Append('\\', slashCount).Append(c);
            slashCount = 0;
        }
        result.Append('\\', slashCount * 2).Append('"');
        return result.ToString();
    }
    public void Dispose() {
        if (job != IntPtr.Zero) CloseHandle(job);
        job = IntPtr.Zero;
    }
}
'@
}

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$InputRoot = (Resolve-Path -LiteralPath $InputRoot).Path
$AwjExecutable = Resolve-Executable $AwjExecutable '' (Join-Path $Repo 'bin\x64\Release\AWJ.exe')
$MagickExecutable = Resolve-Executable $MagickExecutable 'magick'
$RunRegression = $Mode -in @('All', 'Regression')
$RunStrict = $Mode -in @('All', 'Strict')
if ($RunStrict) {
    $FfmpegExecutable = Resolve-Executable $FfmpegExecutable 'ffmpeg'
    $FfprobeExecutable = Join-Path (Split-Path -Parent $FfmpegExecutable) 'ffprobe.exe'
    if (-not (Test-Path -LiteralPath $FfprobeExecutable -PathType Leaf)) {
        $FfprobeExecutable = (Get-Command ffprobe -ErrorAction Stop).Source
    }
}
if (-not $ResultsRoot) {
    $ResultsRoot = Join-Path $Repo ('build\benchmarks\awj-0.10.4-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$ResultsRoot = [IO.Path]::GetFullPath($ResultsRoot)
if (Test-Path -LiteralPath $ResultsRoot) {
    if (@(Get-ChildItem -LiteralPath $ResultsRoot -Force).Count) {
        throw "Results root must be new or empty: $ResultsRoot"
    }
} else {
    New-Item -ItemType Directory -Path $ResultsRoot | Out-Null
}
$StageRoot = Join-Path $ResultsRoot 'staged-inputs'
$WorkRoot = Join-Path $ResultsRoot 'work'
New-Item -ItemType Directory -Path $StageRoot, $WorkRoot -Force | Out-Null

$Power = Get-PowerScheme
if (-not $Smoke -and -not $PowerSchemeGuid) { throw 'Canonical runs require -PowerSchemeGuid.' }
if ($PowerSchemeGuid -and $Power.Guid -ne $PowerSchemeGuid.Trim('{}').ToLowerInvariant()) {
    throw "Power scheme is $($Power.Guid), expected $PowerSchemeGuid."
}
$HardwareThreads = [Environment]::ProcessorCount
$ThreadBudget = Get-AutomaticThreadBudget $HardwareThreads
$Target = Get-AwjDescriptor 'AWJ 0.10.4' $AwjExecutable $false
$ExpectedVersion = (Get-Content -LiteralPath (Join-Path $Repo 'VERSION') -Raw).Trim()
if ($Target.Version -ne $ExpectedVersion) { throw "Target BUILD_INFO version is $($Target.Version), expected $ExpectedVersion." }
$Legacy = $null
if ($LegacyAwjExecutable) {
    $LegacyAwjExecutable = Resolve-Executable $LegacyAwjExecutable '' ''
    $Legacy = Get-AwjDescriptor 'AWJ 0.10.3 baseline' $LegacyAwjExecutable $true
    if (-not $Smoke -and $Legacy.Version -ne '0.10.3') { throw "Legacy baseline is $($Legacy.Version), expected 0.10.3." }
}
$DependencyBaseline = $null
if ($DependencyBaselineAwjExecutable) {
    $DependencyBaselineAwjExecutable = Resolve-Executable $DependencyBaselineAwjExecutable '' ''
    $DependencyBaseline = Get-AwjDescriptor 'AWJ 0.10.4 dependency baseline' $DependencyBaselineAwjExecutable $false
}
$Ffmpeg = $null
if ($RunStrict) {
    $versionOutput = @(& $FfmpegExecutable -version 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'ffmpeg -version failed.' }
    $probe = @(& $FfmpegExecutable -nostdin -hide_banner -loglevel verbose -f lavfi -i 'color=c=black:s=16x16:r=1' -frames:v 1 -c:v libaom-av1 -usage allintra -still-picture 1 -cpu-used $AwjSpeed -crf $FfmpegAomQuantizer -b:v 0 -pix_fmt yuv420p10le -threads 1 -row-mt 1 -aom-params 'tune=iq' -f null - 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'ffmpeg libaom probe failed.' }
    $probeText = $probe -join [Environment]::NewLine
    if ($probeText -notmatch '\[libaom-av1[^\]]*\]\s+(\d+\.\d+\.\d+)') { throw 'Cannot identify ffmpeg libaom version.' }
    $Ffmpeg = [pscustomobject]@{
        Role = 'ffmpeg'
        Path = $FfmpegExecutable
        SHA256 = (Get-FileHash -LiteralPath $FfmpegExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
        Version = [string]$versionOutput[0]
        Configuration = [string]($versionOutput | Where-Object { $_ -like 'configuration:*' } | Select-Object -First 1)
        Aom = $Matches[1]
    }
}

function Test-ActuallyOpaque([string]$Path) {
    $value = @(& $MagickExecutable ($Path + '[0]') -alpha extract -format '%[fx:minima]' info: 2>$null)
    if ($LASTEXITCODE -ne 0) { throw "Alpha probe failed for $Path." }
    [double]$minimum = 0
    if (-not [double]::TryParse(($value -join '').Trim(),
                                [Globalization.NumberStyles]::Float,
                                $Invariant, [ref]$minimum)) {
        throw "Alpha probe returned an invalid value for $Path."
    }
    return $minimum -ge 0.999999
}

function Get-ImageInventory([IO.FileInfo[]]$Files) {
    $byPath = @{}
    foreach ($file in $Files) { $byPath[$file.FullName.ToLowerInvariant()] = $file }
    $result = [Collections.Generic.List[object]]::new()
    foreach ($file in $Files | Where-Object Length -eq 0) {
        $result.Add([pscustomobject]@{
            Path = $file.FullName; Width = 0; Height = 0; Pixels = [long]0; Bytes = [long]0
            BitDepth = 0; Opaque = $false; HasStrictMetadata = $false
            SHA256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
    $nonEmpty = @($Files | Where-Object Length -gt 0)
    for ($offset = 0; $offset -lt $nonEmpty.Count; $offset += 32) {
        $last = [Math]::Min($offset + 31, $nonEmpty.Count - 1)
        $paths = @($nonEmpty[$offset..$last] | ForEach-Object { $_.FullName + '[0]' })
        $lines = @(& $MagickExecutable identify -ping -format '%i|%w|%h|%z|%[opaque]\n' @paths 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "Image probe failed: $($lines -join [Environment]::NewLine)" }
        foreach ($line in $lines) {
            $parts = [string]$line -split '\|', 5
            if ($parts.Count -ne 5) { throw "Unexpected identify output: $line" }
            $path = [IO.Path]::GetFullPath(($parts[0] -replace '\[0\]$', ''))
            $key = $path.ToLowerInvariant()
            if (-not $byPath.ContainsKey($key)) { throw "Unexpected identified path: $path" }
            $result.Add([pscustomobject]@{
                Path = $path
                Width = [int]$parts[1]
                Height = [int]$parts[2]
                Pixels = [long]$parts[1] * [long]$parts[2]
                Bytes = [long]$byPath[$key].Length
                BitDepth = [int]$parts[3]
                Opaque = Test-ActuallyOpaque $path
                HasStrictMetadata = $false
                SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
    }
    if ($result.Count -ne $Files.Count) { throw "Identified $($result.Count) files, expected $($Files.Count)." }
    return @($result | Sort-Object Pixels, Bytes, Path)
}

function Test-HasStrictMetadata([string]$Path) {
    $text = @(& $MagickExecutable identify -ping -verbose ($Path + '[0]') 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Metadata probe failed for $Path." }
    return (($text -join [Environment]::NewLine) -match '(?im)^\s*(?:profile-)?(?:icc|exif|xmp)(?:-|:)')
}

function Take-First([object[]]$Items, [int]$Count, [string]$Name) {
    if ($Items.Count -lt $Count) { throw "$Name needs $Count images, found $($Items.Count)." }
    if ($Count -eq 1) { return @($Items[0]) }
    return @($Items[0..($Count - 1)])
}

function New-Records([object[]]$Items, [string]$Scenario) {
    $records = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $Items.Count; ++$index) {
        $item = $Items[$index]
        $records.Add([pscustomobject]@{
            Scenario = $Scenario; Order = $index + 1; OriginalPath = $item.Path
            StagedName = ('{0:D4}{1}' -f ($index + 1), [IO.Path]::GetExtension($item.Path).ToLowerInvariant())
            StagedPath = ''; Width = $item.Width; Height = $item.Height; Pixels = $item.Pixels
            Bytes = $item.Bytes; BitDepth = $item.BitDepth; Opaque = $item.Opaque
            HasStrictMetadata = $item.HasStrictMetadata; ExpectedFailure = $item.Bytes -eq 0
            SHA256 = $item.SHA256
        })
    }
    return @($records)
}

function Stage-Records([object[]]$Records, [string]$Id) {
    $directory = Join-Path $StageRoot $Id
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $mode = 'hardlink'
    foreach ($record in $Records) {
        $destination = Join-Path $directory $record.StagedName
        try {
            New-Item -ItemType HardLink -Path $destination -Target $record.OriginalPath -ErrorAction Stop | Out-Null
        } catch {
            Copy-Item -LiteralPath $record.OriginalPath -Destination $destination -Force
            $mode = 'copy fallback'
        }
        $record.StagedPath = $destination
    }
    return [pscustomobject]@{ Directory = $directory; LinkMode = $mode }
}

function Reset-RunDirectory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe work path: $full" }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    New-Item -ItemType Directory -Path $full -Force | Out-Null
    return $full
}

function Remove-RunDirectory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe work path: $full" }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}

function Invoke-JobBatch([object[]]$Jobs, [int]$Concurrency) {
    $active = [Collections.Generic.List[object]]::new()
    $next = 0
    $tree = [AwjBenchmarkJob]::new()
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        while ($next -lt $Jobs.Count -or $active.Count -gt 0) {
            while ($next -lt $Jobs.Count -and $active.Count -lt $Concurrency) {
                $job = $Jobs[$next]
                $child = $tree.Start($job.Executable, [string[]]$job.Arguments,
                                     (Split-Path -Parent $job.Executable))
                $active.Add([pscustomobject]@{ Job = $job; Child = $child })
                ++$next
            }
            for ($index = $active.Count - 1; $index -ge 0; --$index) {
                $entry = $active[$index]
                if (-not $entry.Child.HasExited) { continue }
                $entry.Child.Wait()
                $exitCode = $entry.Child.ExitCode
                $entry.Child.Dispose()
                $active.RemoveAt($index)
                if ($exitCode -notin @($entry.Job.AllowedExitCodes)) {
                    throw "$($entry.Job.Label) exited $exitCode."
                }
            }
            if ($active.Count) { Start-Sleep -Milliseconds 20 }
        }
        $clock.Stop()
        return [pscustomobject]@{
            WallSeconds = $clock.Elapsed.TotalSeconds
            CpuSeconds = $tree.CpuSeconds
            PeakMemoryBytes = $tree.PeakMemoryBytes
        }
    } finally {
        $clock.Stop()
        foreach ($entry in $active) {
            try { $entry.Child.Dispose() } catch { }
        }
        $tree.Dispose()
    }
}

function Get-CsvSum([object[]]$Rows, [string]$Property) {
    [double]$sum = 0
    foreach ($row in $Rows) {
        [double]$value = 0
        if ([double]::TryParse([string]$row.$Property, [Globalization.NumberStyles]::Float,
                               $Invariant, [ref]$value) -and $value -ge 0) { $sum += $value }
    }
    return $sum
}

function Get-ExpectedAutoBitDepth([object]$Record) {
    if ($Record.BitDepth -ge 12) { return 12 }
    return 10
}

function Assert-AwjSummary([object[]]$Rows, [object]$Scenario) {
    if ($Rows.Count -ne $Scenario.Inputs.Count) { throw "AWJ summary row count mismatch for $($Scenario.Id)." }
    $failed = @($Rows | Where-Object status -eq 'failed')
    if ($failed.Count -ne $Scenario.ExpectedFailures) { throw "AWJ failure count mismatch for $($Scenario.Id)." }
    $byPath = @{}
    foreach ($record in $Scenario.Inputs) {
        $byPath[[IO.Path]::GetFullPath($record.StagedPath).ToLowerInvariant()] = $record
        $byPath[$record.StagedName.ToLowerInvariant()] = $record
    }
    foreach ($row in $Rows | Where-Object status -eq 'ok') {
        $input = [string]$row.input
        $key = if ([IO.Path]::IsPathRooted($input)) {
            [IO.Path]::GetFullPath($input).ToLowerInvariant()
        } else {
            [IO.Path]::GetFileName($input).ToLowerInvariant()
        }
        if (-not $byPath.ContainsKey($key)) { throw "AWJ summary unknown input: $($row.input)" }
        $record = $byPath[$key]
        $lossless = ([string]$row.lossless).Equals('true', [StringComparison]::OrdinalIgnoreCase)
        $finalQuality = $AwjQuality
        $chroma = Get-ExpectedAutoChroma ([string]$row.source_chroma)
        if ($row.format -ne 'AVIF' -or $row.encoder_id -ne 'aom' -or
            [int]$row.quality -ne $AwjQuality -or [int]$row.final_encoder_quality -ne $finalQuality -or
            [int]$row.speed -ne $AwjSpeed -or $row.applied_chroma -ne $chroma -or
            [int]$row.applied_bit_depth -ne (Get-ExpectedAutoBitDepth $record) -or
            [int]$row.encoder_threads -ne $Scenario.AwjEncoderThreads -or
            $lossless -or
            (-not $record.Opaque -and $row.applied_alpha -ne 'kept')) {
            throw "AWJ applied settings drifted for $($row.input)."
        }
    }
}

function New-FfmpegArguments([string]$InputPath, [string]$OutputPath) {
    return @('-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
             '-threads', '1', '-filter_threads', '1', '-filter_complex_threads', '1',
             '-i', $InputPath, '-frames:v', '1', '-an', '-sn', '-dn',
             '-map_metadata', '-1', '-vf', 'scale=in_range=pc:out_range=pc:out_color_matrix=bt709',
             '-pix_fmt', 'yuv420p10le', '-c:v', 'libaom-av1', '-usage', 'allintra',
             '-still-picture', '1', '-cpu-used', [string]$AwjSpeed,
             '-crf', [string]$FfmpegAomQuantizer, '-b:v', '0', '-lag-in-frames', '0',
             '-row-mt', '1', '-aom-params', 'tune=iq', '-color_range', 'pc',
             '-color_primaries', 'bt709', '-color_trc', 'iec61966-2-1',
             '-colorspace', 'bt709', '-f', 'avif', $OutputPath)
}

function Invoke-AwjCase([object]$Scenario, [object]$Descriptor, [string]$RunDirectory) {
    $output = Reset-RunDirectory (Join-Path $RunDirectory 'awj')
    $job = [pscustomobject]@{
        Label = "$($Descriptor.Role) $($Scenario.Id)"
        Executable = $Descriptor.Path
        Arguments = New-AwjArguments $Scenario.InputDirectory $output $Descriptor.LegacyTenBit
        AllowedExitCodes = if ($Scenario.ExpectedFailures) { @(0, 2) } else { @(0) }
    }
    $metrics = Invoke-JobBatch @($job) 1
    $summary = Join-Path $output 'summary.csv'
    if (-not (Test-Path -LiteralPath $summary -PathType Leaf)) { throw "$($Descriptor.Role) wrote no summary.csv." }
    $rows = @(Import-Csv -LiteralPath $summary)
    Assert-AwjSummary $rows $Scenario
    $ok = @($rows | Where-Object status -eq 'ok')
    $result = [pscustomobject]@{
        Metrics = $metrics
        OutputBytes = Get-CsvSum $rows 'output_bytes'
        SuccessCount = $ok.Count
        FailedCount = @($rows | Where-Object status -eq 'failed').Count
        FailureNote = (@($rows | Where-Object status -eq 'failed' | ForEach-Object { $_.message }) -join ' | ')
        DecodeSeconds = Get-CsvSum $ok 'decode_seconds'
        PrepareSeconds = Get-CsvSum $ok 'prepare_seconds'
        EncodeSeconds = Get-CsvSum $ok 'encode_seconds'
        RgbToYuvSeconds = Get-CsvSum $ok 'avif_rgb_to_yuv_seconds'
        AddImageSeconds = Get-CsvSum $ok 'avif_add_image_seconds'
        FinishSeconds = Get-CsvSum $ok 'avif_finish_seconds'
        OutputCopySeconds = Get-CsvSum $ok 'avif_output_copy_seconds'
        WriteSeconds = Get-CsvSum $ok 'write_seconds'
    }
    Remove-RunDirectory $output
    return $result
}

function Invoke-FfmpegCase([object]$Scenario, [string]$RunDirectory) {
    $output = Reset-RunDirectory (Join-Path $RunDirectory 'ffmpeg')
    $jobs = [Collections.Generic.List[object]]::new()
    foreach ($record in $Scenario.Inputs) {
        $name = ([IO.Path]::GetFileNameWithoutExtension($record.StagedName)) + '.avif'
        $jobs.Add([pscustomobject]@{
            Label = "ffmpeg $($record.StagedName)"
            Executable = $FfmpegExecutable
            Arguments = New-FfmpegArguments $record.StagedPath (Join-Path $output $name)
            AllowedExitCodes = @(0)
        })
    }
    $metrics = Invoke-JobBatch @($jobs) $Scenario.FfmpegConcurrency
    $outputs = @(Get-ChildItem -LiteralPath $output -Filter '*.avif' -File | Where-Object Length -gt 0)
    if ($outputs.Count -ne $Scenario.SuccessCount) { throw "ffmpeg output count mismatch for $($Scenario.Id)." }
    foreach ($file in $outputs) {
        $probe = @(& $FfprobeExecutable -v error -select_streams v:0 -show_entries 'stream=codec_name,pix_fmt' -of default=noprint_wrappers=1 $file.FullName 2>&1)
        $text = $probe -join [Environment]::NewLine
        if ($LASTEXITCODE -ne 0 -or $text -notmatch 'codec_name=av1' -or $text -notmatch 'pix_fmt=yuv420p10le') {
            throw "ffmpeg output validation failed: $($file.FullName)"
        }
    }
    $result = [pscustomobject]@{
        Metrics = $metrics
        OutputBytes = [double](($outputs | Measure-Object Length -Sum).Sum)
        SuccessCount = $outputs.Count
        FailedCount = 0
        FailureNote = ''
        DecodeSeconds = $null; PrepareSeconds = $null; EncodeSeconds = $null
        RgbToYuvSeconds = $null; AddImageSeconds = $null; FinishSeconds = $null
        OutputCopySeconds = $null; WriteSeconds = $null
    }
    Remove-RunDirectory $output
    return $result
}

$Files = @(Get-ChildItem -LiteralPath $InputRoot -File -Recurse)
if (-not $Smoke -and $Files.Count -ne 613) { throw "Canonical corpus requires 613 files, found $($Files.Count)." }
$Inventory = @(Get-ImageInventory $Files)
$Opaque = @($Inventory | Where-Object { $_.Bytes -gt 0 -and $_.Opaque })
$Transparent = @($Inventory | Where-Object { $_.Bytes -gt 0 -and -not $_.Opaque })
if (-not $Opaque.Count -or -not $Transparent.Count) { throw 'Corpus needs opaque and transparent inputs.' }
$StrictItems = @()
if ($RunStrict) {
    $items = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $Opaque.Count -and $items.Count -lt $StrictCount; ++$index) {
        $percent = 100 * $index / [Math]::Max(1, $Opaque.Count)
        Write-Progress -Activity 'Checking strict subset metadata' -Status "$($items.Count) / $StrictCount" -PercentComplete $percent
        $candidate = $Opaque[$index]
        $candidate.HasStrictMetadata = Test-HasStrictMetadata $candidate.Path
        if (-not $candidate.HasStrictMetadata -and $candidate.BitDepth -le 10) {
            $items.Add($candidate)
        }
    }
    Write-Progress -Activity 'Checking strict subset metadata' -Completed
    if ($items.Count -ne $StrictCount) { throw "Strict subset found $($items.Count), needs $StrictCount." }
    $StrictItems = @($items.ToArray())
}

function New-Scenario([string]$Id, [string]$Description, [object[]]$Inputs, [string[]]$Tools) {
    $threads = Get-ExpectedEncoderThreads $ThreadBudget $Inputs.Count
    $staged = Stage-Records $Inputs $Id
    return [pscustomobject]@{
        Id = $Id; Description = $Description; Inputs = $Inputs
        InputDirectory = $staged.Directory; LinkMode = $staged.LinkMode
        TotalPixels = [long](($Inputs | Measure-Object Pixels -Sum).Sum)
        SuccessPixels = [long](($Inputs | Where-Object { -not $_.ExpectedFailure } | Measure-Object Pixels -Sum).Sum)
        SuccessCount = @($Inputs | Where-Object { -not $_.ExpectedFailure }).Count
        ExpectedFailures = @($Inputs | Where-Object ExpectedFailure).Count
        AwjEncoderThreads = $threads; AwjFileConcurrency = [int]($ThreadBudget / $threads)
        FfmpegConcurrency = [int][Math]::Min($ThreadBudget, $Inputs.Count)
        Tools = $Tools; WarmupRuns = $WarmupRuns; MeasuredRuns = $MeasuredRuns
    }
}

$Scenarios = [Collections.Generic.List[object]]::new()
if ($RunRegression) {
    foreach ($count in $(if ($Smoke) { @(1) } else { @(1, 4, 12, 13) })) {
        $records = New-Records (Take-First $Opaque $count "opaque-$count") "opaque-$count"
        $Scenarios.Add((New-Scenario "opaque-$count" "opaque default AVIF, $count image(s)" $records @('AWJ')))
    }
    if (-not $Smoke) {
        $batchTools = if ($DependencyBaseline) { @('DependencyBaseline', 'AWJ') } else { @('AWJ') }
        $Scenarios.Add((New-Scenario 'batch-613' 'fixed 613 image CLI corpus' (New-Records $Inventory 'batch-613') $batchTools))
    }
$Scenarios.Add((New-Scenario 'transparent-12' 'transparent default AVIF path' (New-Records (Take-First $Transparent 12 'transparent-12') 'transparent-12') @('AWJ')))
}
if ($RunStrict) {
    $tools = [Collections.Generic.List[string]]::new()
    if ($Legacy) { $tools.Add('Legacy0103') }
    $tools.Add('AWJ')
    $tools.Add('ffmpeg')
    $Scenarios.Add((New-Scenario 'strict-210' 'opaque without ICC EXIF XMP' (New-Records $StrictItems 'strict-210') @($tools)))
}

$Manifest = [Collections.Generic.List[object]]::new()
foreach ($scenario in $Scenarios) {
    foreach ($record in $scenario.Inputs) {
        $Manifest.Add([pscustomobject]@{
            Scenario = $scenario.Id; LinkMode = $scenario.LinkMode; Order = $record.Order
            OriginalPath = $record.OriginalPath; StagedPath = $record.StagedPath
            Width = $record.Width; Height = $record.Height; Pixels = $record.Pixels
            Bytes = $record.Bytes; BitDepth = $record.BitDepth; Opaque = $record.Opaque
            HasStrictMetadata = $record.HasStrictMetadata; ExpectedFailure = $record.ExpectedFailure
            SHA256 = $record.SHA256
        })
    }
}
$ManifestPath = Join-Path $ResultsRoot 'inputs.csv'
$Manifest | Export-Csv -LiteralPath $ManifestPath -NoTypeInformation -Encoding utf8NoBOM
$ManifestHash = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

Add-BenchmarkJobType
$Runs = [Collections.Generic.List[object]]::new()
$RunPath = Join-Path $ResultsRoot 'runs.csv'
$total = 0
foreach ($scenario in $Scenarios) { $total += $scenario.Tools.Count * ($scenario.WarmupRuns + $scenario.MeasuredRuns) }
$complete = 0
foreach ($scenario in $Scenarios) {
    for ($iteration = 0; $iteration -lt ($scenario.WarmupRuns + $scenario.MeasuredRuns); ++$iteration) {
        $warmup = $iteration -lt $scenario.WarmupRuns
        $number = if ($warmup) { 0 } else { $iteration - $scenario.WarmupRuns + 1 }
        $label = if ($warmup) { 'warmup' } else { "run-$number" }
        for ($order = 0; $order -lt $scenario.Tools.Count; ++$order) {
            if ((Get-PowerScheme).Guid -ne $Power.Guid) { throw 'Power scheme changed during run.' }
            $tool = $scenario.Tools[$order]
            $descriptor = if ($tool -eq 'AWJ') { $Target } elseif ($tool -eq 'Legacy0103') { $Legacy } elseif ($tool -eq 'DependencyBaseline') { $DependencyBaseline } else { $null }
            $runDirectory = Join-Path $WorkRoot "$($scenario.Id)-$label-$($order + 1)"
            Write-Host "[$($scenario.Id)] $label $tool"
            $result = if ($tool -eq 'ffmpeg') { Invoke-FfmpegCase $scenario $runDirectory } else { Invoke-AwjCase $scenario $descriptor $runDirectory }
            Remove-RunDirectory $runDirectory
            $role = if ($tool -eq 'ffmpeg') { 'ffmpeg' } else { $descriptor.Role }
            $aom = if ($tool -eq 'ffmpeg') { $Ffmpeg.Aom } else { $descriptor.Aom }
            $Runs.Add([pscustomobject]@{
                Scenario = $scenario.Id; Tool = $tool; ToolRole = $role; Warmup = $warmup; Run = $number; OrderInRound = $order + 1
                FileCount = $scenario.Inputs.Count; SuccessCount = $result.SuccessCount; FailedCount = $result.FailedCount
                TotalPixels = $scenario.TotalPixels; SuccessPixels = $scenario.SuccessPixels
                WallSeconds = $result.Metrics.WallSeconds; TreeCpuSeconds = $result.Metrics.CpuSeconds; PeakMemoryBytes = $result.Metrics.PeakMemoryBytes
                OutputBytes = $result.OutputBytes; ImagesPerSecond = $result.SuccessCount / $result.Metrics.WallSeconds; MegaPixelsPerSecond = ($scenario.SuccessPixels / 1e6) / $result.Metrics.WallSeconds
                DecodeSeconds = $result.DecodeSeconds; PrepareSeconds = $result.PrepareSeconds; EncodeSeconds = $result.EncodeSeconds
                AvifRgbToYuvSeconds = $result.RgbToYuvSeconds; AvifAddImageSeconds = $result.AddImageSeconds; AvifFinishSeconds = $result.FinishSeconds
                AvifOutputCopySeconds = $result.OutputCopySeconds; WriteSeconds = $result.WriteSeconds
                EncoderThreads = if ($tool -eq 'ffmpeg') { 1 } else { $scenario.AwjEncoderThreads }
                FileConcurrency = if ($tool -eq 'ffmpeg') { $scenario.FfmpegConcurrency } else { $scenario.AwjFileConcurrency }
                AomVersion = $aom; FailureNote = $result.FailureNote
            })
            $Runs | Export-Csv -LiteralPath $RunPath -NoTypeInformation -Encoding utf8NoBOM
            ++$complete
            if ($complete -lt $total -and $CooldownSeconds -gt 0) { Start-Sleep -Seconds $CooldownSeconds }
        }
    }
}

$Measured = @($Runs | Where-Object { -not $_.Warmup })
function Aggregate([string]$ScenarioId, [string]$Tool) {
    $rows = @($Measured | Where-Object { $_.Scenario -eq $ScenarioId -and $_.Tool -eq $Tool })
    $scenario = $Scenarios | Where-Object Id -eq $ScenarioId
    if ($rows.Count -ne $scenario.MeasuredRuns) { throw "Aggregate row mismatch for $ScenarioId/$Tool." }
    $awj = $Tool -ne 'ffmpeg'
    $stage = {
        param([string]$Name)
        if ($awj) { return Get-Median ([double[]]$rows.$Name) }
        return [double]::NaN
    }
    $stage95 = {
        param([string]$Name)
        if ($awj) { return Get-P95 ([double[]]$rows.$Name) }
        return [double]::NaN
    }
    return [pscustomobject]@{
        Scenario = $ScenarioId; Tool = $Tool; ToolRole = $rows[0].ToolRole
        WallMedian = Get-Median ([double[]]$rows.WallSeconds); WallP95 = Get-P95 ([double[]]$rows.WallSeconds)
        CpuMedian = Get-Median ([double[]]$rows.TreeCpuSeconds); CpuP95 = Get-P95 ([double[]]$rows.TreeCpuSeconds)
        PeakMedianMiB = (Get-Median ([double[]]$rows.PeakMemoryBytes)) / 1MB; PeakP95MiB = (Get-P95 ([double[]]$rows.PeakMemoryBytes)) / 1MB
        ImagesPerSecond = Get-Median ([double[]]$rows.ImagesPerSecond); MegaPixelsPerSecond = Get-Median ([double[]]$rows.MegaPixelsPerSecond)
        OutputMedianMiB = (Get-Median ([double[]]$rows.OutputBytes)) / 1MB
        SuccessCount = $rows[0].SuccessCount; FailedCount = $rows[0].FailedCount; FailureNote = $rows[0].FailureNote
        DecodeMedian = & $stage 'DecodeSeconds'; DecodeP95 = & $stage95 'DecodeSeconds'
        PrepareMedian = & $stage 'PrepareSeconds'; PrepareP95 = & $stage95 'PrepareSeconds'
        EncodeMedian = & $stage 'EncodeSeconds'; EncodeP95 = & $stage95 'EncodeSeconds'
        RgbMedian = & $stage 'AvifRgbToYuvSeconds'; RgbP95 = & $stage95 'AvifRgbToYuvSeconds'
        AddMedian = & $stage 'AvifAddImageSeconds'; AddP95 = & $stage95 'AvifAddImageSeconds'
        FinishMedian = & $stage 'AvifFinishSeconds'; FinishP95 = & $stage95 'AvifFinishSeconds'
        CopyMedian = & $stage 'AvifOutputCopySeconds'; CopyP95 = & $stage95 'AvifOutputCopySeconds'
        WriteMedian = & $stage 'WriteSeconds'; WriteP95 = & $stage95 'WriteSeconds'
    }
}

$Aggregates = [Collections.Generic.List[object]]::new()
foreach ($scenario in $Scenarios) {
    foreach ($tool in $scenario.Tools) { $Aggregates.Add((Aggregate $scenario.Id $tool)) }
}
$Aggregates | Export-Csv -LiteralPath (Join-Path $ResultsRoot 'aggregates.csv') -NoTypeInformation -Encoding utf8NoBOM

function Format-Number([double]$Value, [string]$Format = 'F3') {
    if ([double]::IsNaN($Value)) { return 'n/a' }
    return $Value.ToString($Format, $Invariant)
}

$Gates = [Collections.Generic.List[object]]::new()
function Add-Gate([string]$Name, [string]$Status, [string]$Detail) {
    $Gates.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
}
if ($RunStrict) {
    $strictAwj = $Aggregates | Where-Object { $_.Scenario -eq 'strict-210' -and $_.Tool -eq 'AWJ' }
    $strictFfmpeg = $Aggregates | Where-Object { $_.Scenario -eq 'strict-210' -and $_.Tool -eq 'ffmpeg' }
    Add-Gate 'strict AWJ wall median <= ffmpeg' $(if ($strictAwj.WallMedian -le $strictFfmpeg.WallMedian) { 'PASS' } else { 'FAIL' }) "$(Format-Number $strictAwj.WallMedian) s vs $(Format-Number $strictFfmpeg.WallMedian) s"
    if ($Legacy) {
        $legacyStrict = $Aggregates | Where-Object { $_.Scenario -eq 'strict-210' -and $_.Tool -eq 'Legacy0103' }
        $ratio = $strictAwj.CpuMedian / $legacyStrict.CpuMedian
        Add-Gate 'strict AWJ CPU median <= 95% of 0.10.3' $(if ($ratio -le 0.95) { 'PASS' } else { 'FAIL' }) "$(Format-Number ($ratio * 100) 'F2')%"
    } else {
        Add-Gate 'strict AWJ CPU median <= 95% of 0.10.3' 'NOT RUN' 'Pass LegacyAwjExecutable.'
    }
}
if ($DependencyBaseline -and $RunRegression) {
    $current = $Aggregates | Where-Object { $_.Scenario -eq 'batch-613' -and $_.Tool -eq 'AWJ' }
    $baseline = $Aggregates | Where-Object { $_.Scenario -eq 'batch-613' -and $_.Tool -eq 'DependencyBaseline' }
    foreach ($metric in @(
        [pscustomobject]@{ Name = 'wall'; Current = $current.WallMedian; Baseline = $baseline.WallMedian },
        [pscustomobject]@{ Name = 'CPU'; Current = $current.CpuMedian; Baseline = $baseline.CpuMedian },
        [pscustomobject]@{ Name = 'wall P95'; Current = $current.WallP95; Baseline = $baseline.WallP95 }
    )) {
        $ratio = $metric.Current / $metric.Baseline
        Add-Gate "613 $($metric.Name) <= dependency baseline +2%" $(if ($ratio -le 1.02) { 'PASS' } else { 'FAIL' }) "$(Format-Number ($ratio * 100) 'F2')%"
    }
}

$Report = [Collections.Generic.List[string]]::new()
$Report.Add('# AWJimage 0.10.4 CLI AVIF benchmark')
$Report.Add('')
$Report.Add("Generated: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))")
$Report.Add("Power scheme: $($Power.Guid)")
$Report.Add("Hardware threads / AWJ CPU budget: $HardwareThreads / $ThreadBudget")
$Report.Add("Input manifest SHA-256: $ManifestHash")
$Report.Add('')
$Report.Add('AWJ uses only input, output, summary, and no-log options. It therefore exercises the true defaults: AVIF AOM q70, speed 6, source-aware automatic chroma, automatic 10-bit-minimum depth, automatic non-opaque alpha retention at the same quality, and automatic memory.')
$Report.Add('Strict ffmpeg uses QP 23, cpu-used 6, all-intra, still-picture, row-mt, yuv420p10le, one thread per process, and CPU-budget process concurrency. Inputs are pixels, bytes, path ascending. Each strict round runs AWJ before ffmpeg and sleeps between calls; thermal and file-cache bias remains a documented limitation.')
$Report.Add('')
$Report.Add("AWJ: $($Target.Version), $($Target.BuildType), $($Target.Commit), AOM $($Target.Aom), dav1d $($Target.Dav1d)")
if ($Legacy) { $Report.Add("0.10.3 baseline: $($Legacy.Commit), AOM $($Legacy.Aom), explicit 10-bit only") }
if ($DependencyBaseline) { $Report.Add("Dependency baseline: $($DependencyBaseline.Commit), AOM $($DependencyBaseline.Aom)") }
if ($Ffmpeg) { $Report.Add("ffmpeg: $($Ffmpeg.Version), AOM $($Ffmpeg.Aom)") }
$Report.Add('')
$Report.Add('## Results')
$Report.Add('')
$Report.Add('| Scenario | Tool | Success / failed | Threads x files | Wall median / P95 s | Tree CPU median / P95 s | Peak memory median / P95 MiB | Images/s | MP/s | Output MiB |')
$Report.Add('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
foreach ($item in $Aggregates) {
    $row = @($Measured | Where-Object { $_.Scenario -eq $item.Scenario -and $_.Tool -eq $item.Tool })[0]
    $Report.Add("| $($item.Scenario) | $($item.ToolRole) | $($item.SuccessCount) / $($item.FailedCount) | $($row.EncoderThreads) x $($row.FileConcurrency) | $(Format-Number $item.WallMedian) / $(Format-Number $item.WallP95) | $(Format-Number $item.CpuMedian) / $(Format-Number $item.CpuP95) | $(Format-Number $item.PeakMedianMiB 'F1') / $(Format-Number $item.PeakP95MiB 'F1') | $(Format-Number $item.ImagesPerSecond) | $(Format-Number $item.MegaPixelsPerSecond) | $(Format-Number $item.OutputMedianMiB 'F2') |")
}
$Report.Add('')
$Report.Add('## AWJ stage timings')
$Report.Add('')
$Report.Add('| Scenario | Tool | Decode median / P95 | Prepare median / P95 | Encode median / P95 | RGB to YUV median / P95 | AddImage median / P95 | Finish median / P95 | Copy median / P95 | Write median / P95 |')
$Report.Add('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
foreach ($item in $Aggregates | Where-Object { $_.Tool -ne 'ffmpeg' }) {
    $Report.Add("| $($item.Scenario) | $($item.ToolRole) | $(Format-Number $item.DecodeMedian) / $(Format-Number $item.DecodeP95) | $(Format-Number $item.PrepareMedian) / $(Format-Number $item.PrepareP95) | $(Format-Number $item.EncodeMedian) / $(Format-Number $item.EncodeP95) | $(Format-Number $item.RgbMedian) / $(Format-Number $item.RgbP95) | $(Format-Number $item.AddMedian) / $(Format-Number $item.AddP95) | $(Format-Number $item.FinishMedian) / $(Format-Number $item.FinishP95) | $(Format-Number $item.CopyMedian) / $(Format-Number $item.CopyP95) | $(Format-Number $item.WriteMedian) / $(Format-Number $item.WriteP95) |")
}
$Report.Add('')
$Report.Add('Stage values are sums of per-file CSV timings for each batch and are diagnostic spans, not a wall-clock decomposition. CPU and peak memory are queried from a Windows Job Object covering the process tree.')
$Report.Add('')
$Report.Add('## Gates')
$Report.Add('')
$Report.Add('| Gate | Status | Detail |')
$Report.Add('|---|---|---|')
if ($Gates.Count) {
    foreach ($gate in $Gates) { $Report.Add("| $($gate.Name) | $($gate.Status) | $($gate.Detail) |") }
} else {
    $Report.Add('| no cross-version comparator selected | NOT RUN | Select strict mode or provide a baseline executable. |')
}
$Report.Add('')
$Report.Add("Corpus: $($Inventory.Count) files; $($Opaque.Count) opaque; $($Transparent.Count) actual transparent; $(@($Inventory | Where-Object Bytes -eq 0).Count) zero-byte known failure. Strict subset: $StrictCount opaque files without ICC, EXIF, or XMP and no source depth above 10-bit, matching ffmpeg yuv420p10le.")
$Report.Add('Historical q80 8-bit and large-image data are intentionally omitted because they are not comparable. AOM and libavif updates can change encoded bytes; the 10-bit default can increase CPU, memory, and output size.')
$ReportPath = Join-Path $ResultsRoot 'report.md'
$Report | Set-Content -LiteralPath $ReportPath -Encoding utf8NoBOM

$Metadata = [ordered]@{
    generated_at = (Get-Date).ToString('o')
    input_root = $InputRoot
    input_manifest = $ManifestPath
    input_manifest_sha256 = $ManifestHash
    power_scheme = $Power
    hardware_threads = $HardwareThreads
    automatic_thread_budget = $ThreadBudget
    warmups = $WarmupRuns
    measured_runs = $MeasuredRuns
    cooldown_seconds = $CooldownSeconds
    profile = [ordered]@{
        awj = 'AVIF default q70 speed6 420 automatic minimum 10-bit automatic memory'
        ffmpeg = 'QP23 cpu-used6 allintra still-picture row-mt yuv420p10le threads1'
    }
    awj = $Target
    legacy_awj = $Legacy
    dependency_baseline_awj = $DependencyBaseline
    ffmpeg = $Ffmpeg
    gates = @($Gates)
}
$Metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ResultsRoot 'metadata.json') -Encoding utf8NoBOM
Write-Host "Benchmark complete: $ReportPath"
