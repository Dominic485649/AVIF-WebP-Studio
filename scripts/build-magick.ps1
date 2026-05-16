param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",

    [ValidateSet("x64")]
    [string]$Arch = "x64",

    [ValidateSet("Static", "Dynamic")]
    [string]$Linkage = "Static",

    [string]$SourceRoot = "",
    [string]$RuntimeRoot = "",
    [string]$RepositoryUrl = "https://github.com/ImageMagick/Windows.git",
    [switch]$FullBuild,
    [switch]$InstallMfc,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

if (-not $SourceRoot) {
    $SourceRoot = Join-Path $Repo "third_party\imagemagick-src"
}
if (-not $RuntimeRoot) {
    $RuntimeRoot = Join-Path $Repo "third_party\imagemagick-runtime\$Arch\$Configuration"
}

function Add-GitProcessConfig([string]$Key, [string]$Value) {
    [int]$GitConfigCount = 0
    if (-not [int]::TryParse($env:GIT_CONFIG_COUNT, [ref]$GitConfigCount)) {
        $GitConfigCount = 0
    }

    Set-Item -Path "Env:GIT_CONFIG_KEY_$GitConfigCount" -Value $Key
    Set-Item -Path "Env:GIT_CONFIG_VALUE_$GitConfigCount" -Value $Value
    $env:GIT_CONFIG_COUNT = [string]($GitConfigCount + 1)
}

function Initialize-GitEnvironment {
    Add-GitProcessConfig "core.longpaths" "true"
    Add-GitProcessConfig "url.https://github.com/.insteadOf" "https://github.com/"
    Add-GitProcessConfig "url.https://github.com/.insteadOf" "git@github.com:"
    Add-GitProcessConfig "url.https://github.com/.insteadOf" "ssh://git@github.com/"
    Add-GitProcessConfig "url.https://github.com/.insteadOf" "ssh://git@ssh.github.com/"
}

function Remove-IncompleteGitClone([string]$Path) {
    if ((Test-Path -LiteralPath $Path) -and
        -not (Test-Path -LiteralPath (Join-Path $Path ".git"))) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Remove-IncompleteGitClonesInContainer([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return
    }

    Get-ChildItem -LiteralPath $Path -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-IncompleteGitClone $_.FullName
    }
}

Initialize-GitEnvironment

function Find-MSBuild {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $Found = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($Found) {
            return $Found
        }
    }

    $Command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($Command) {
        return $Command.Source
    }
    throw "找不到 MSBuild。请确认 Visual Studio 2026 已安装 C++ 桌面工作负载。"
}

function Find-VSInstallationPath {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $Found = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
        if ($Found) {
            return $Found
        }
    }
    throw "找不到 Visual Studio 安装目录。请确认 Visual Studio 2026 已安装。"
}

function Test-MfcInstalled([string]$VSInstallPath) {
    $ToolsRoot = Join-Path $VSInstallPath "VC\Tools\MSVC"
    if (-not (Test-Path -LiteralPath $ToolsRoot)) {
        return $false
    }

    $MfcHeader = Get-ChildItem -LiteralPath $ToolsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "atlmfc\include\afxwin.h" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    return [bool]$MfcHeader
}

function Install-MfcComponent([string]$VSInstallPath) {
    $Installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vs_installer.exe"
    if (-not (Test-Path -LiteralPath $Installer)) {
        throw "找不到 Visual Studio Installer，无法自动安装 MFC。请手动安装组件 Microsoft.VisualStudio.Component.VC.ATLMFC。"
    }

    Write-Host "正在通过 Visual Studio Installer 安装 MFC 组件..."
    & $Installer modify `
        --installPath $VSInstallPath `
        --add Microsoft.VisualStudio.Component.VC.ATLMFC `
        --quiet `
        --norestart `
        --wait
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio Installer 安装 MFC 失败，退出码 $LASTEXITCODE。"
    }
}

function Assert-MfcAvailable([string]$VSInstallPath, [bool]$AllowInstall) {
    if (Test-MfcInstalled $VSInstallPath) {
        return
    }

    if ($AllowInstall) {
        Install-MfcComponent $VSInstallPath
        if (Test-MfcInstalled $VSInstallPath) {
            return
        }
    }

    throw @"
缺少 Visual Studio MFC 组件，ImageMagick/Windows 的 Configure.sln 必须使用 MFC。
请在 Visual Studio Installer 中安装:
  C++ MFC for latest MSVC build tools (x86 & x64)
组件 ID:
  Microsoft.VisualStudio.Component.VC.ATLMFC
也可以重新运行:
  .\scripts\build-magick.ps1 -Configuration $Configuration -Arch $Arch -Linkage $Linkage -InstallMfc
"@
}

function Invoke-CmdScript([string]$ScriptPath) {
    Push-Location (Split-Path -Parent $ScriptPath)
    try {
        & cmd.exe /c "`"$ScriptPath`""
        if ($LASTEXITCODE -ne 0) {
            throw "$ScriptPath 失败，退出码 $LASTEXITCODE。"
        }
    } finally {
        Pop-Location
    }
}

function Invoke-MSBuild([string]$MSBuild, [string]$Solution, [string[]]$ExtraArgs) {
    Write-Host "MSBuild: $Solution"
    & $MSBuild $Solution @ExtraArgs
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild 失败，退出码 $LASTEXITCODE。"
    }
}

function Get-NewestFile([string]$Root, [string]$Filter) {
    Get-ChildItem -Path $Root -Recurse -Filter $Filter -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Copy-TreeIfExists([string]$Path, [string]$Destination) {
    if (Test-Path $Path) {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
        Copy-Item -LiteralPath $Path -Destination $Destination -Recurse -Force
    }
}

function Copy-ChildrenIfExists([string]$Path, [string]$Destination) {
    if (Test-Path -LiteralPath $Path -PathType Container) {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
        Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
        }
    }
}

function Copy-FilesIfExists([string]$Path, [string]$Destination, [string]$Filter) {
    if (Test-Path $Path) {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
        Get-ChildItem -Path $Path -Filter $Filter -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
        }
    }
}

function Copy-NoticeFile([System.IO.FileInfo]$File, [string]$DestinationRoot) {
    $Target = Join-Path $DestinationRoot $File.Name
    if (Test-Path -LiteralPath $Target -PathType Container) {
        $Target = Join-Path $DestinationRoot ("ImageMagick-" + $File.Name)
    }
    Copy-Item -LiteralPath $File.FullName -Destination $Target -Force
}

function Repair-GeneratedProjectToolCommands([string]$ProjectRoot) {
    if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
        return
    }

    Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Filter "*.vcxproj" -File -ErrorAction SilentlyContinue | ForEach-Object {
        $ProjectPath = $_.FullName
        [xml]$ProjectXml = Get-Content -LiteralPath $ProjectPath -Raw
        $Namespace = New-Object System.Xml.XmlNamespaceManager($ProjectXml.NameTable)
        $Namespace.AddNamespace("msb", "http://schemas.microsoft.com/developer/msbuild/2003")

        $Changed = $false
        $CommandNodes = $ProjectXml.SelectNodes("//msb:CustomBuild/msb:Command", $Namespace)
        foreach ($Node in $CommandNodes) {
            $Command = $Node.InnerText
            if ($Command.StartsWith('$(SolutionDir)Configure\Tools\', [System.StringComparison]::OrdinalIgnoreCase)) {
                $ExeEnd = $Command.IndexOf(".exe ", [System.StringComparison]::OrdinalIgnoreCase)
                if ($ExeEnd -ge 0) {
                    $Tool = $Command.Substring(0, $ExeEnd + 4)
                    $Rest = $Command.Substring($ExeEnd + 4)
                    $Node.InnerText = "`"$Tool`"$Rest"
                    $Changed = $true
                }
            }
        }

        if ($Changed) {
            $ProjectXml.Save($ProjectPath)
        }
    }
}

Write-Host "ImageMagick Windows 源码: $SourceRoot"

$MSBuild = $null
if (-not $SkipBuild) {
    $MSBuild = Find-MSBuild
    $VSInstallPath = Find-VSInstallationPath
    Assert-MfcAvailable $VSInstallPath ([bool]$InstallMfc)
    Write-Host "MSBuild: $MSBuild"
}

if (-not (Test-Path (Join-Path $SourceRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SourceRoot) | Out-Null
    git clone $RepositoryUrl $SourceRoot
} else {
    git -C $SourceRoot fetch --prune
}
git -C $SourceRoot checkout main
git -C $SourceRoot pull --ff-only

$CloneScript = Join-Path $SourceRoot "clone-repositories-im7.cmd"
if (Test-Path $CloneScript) {
    Write-Host "同步 ImageMagick / Configure / Dependencies 源码..."
    foreach ($RepoDir in @("ImageMagick", "Configure", "Dependencies")) {
        Remove-IncompleteGitClone (Join-Path $SourceRoot $RepoDir)
    }
    $DependenciesRoot = Join-Path $SourceRoot "Dependencies"
    # These folders are containers created by ImageMagick's dependency script;
    # only their children are git clones.
    foreach ($Container in @("Dependencies", "OptionalDependencies", "NonWindowsDependencies")) {
        Remove-IncompleteGitClonesInContainer (Join-Path $DependenciesRoot $Container)
    }
    Invoke-CmdScript $CloneScript
} else {
    Write-Warning "未找到 clone-repositories-im7.cmd，跳过源码同步。"
}

if (-not $SkipBuild) {
    $ConfigureSolution = Join-Path $SourceRoot "Configure\Configure.sln"
    if (-not (Test-Path $ConfigureSolution)) {
        throw "未找到 Configure\Configure.sln。clone-repositories-im7.cmd 可能没有完整同步 Configure 仓库。"
    }

    Invoke-MSBuild -MSBuild $MSBuild -Solution $ConfigureSolution -ExtraArgs @(
        "/m",
        "/restore",
        "/p:Configuration=Release",
        "/p:Platform=$Arch"
    )

    $ConfigureExe = Get-NewestFile (Join-Path $SourceRoot "Configure") "Configure*.exe"
    if (-not $ConfigureExe) {
        throw "未找到 Configure.exe，无法生成 ImageMagick 解决方案。"
    }

    $ConfigureArgs = @(
        "/noWizard",
        "/VS2026",
        "/$Arch",
        "/Q16",
        "/noHdri",
        "/noOpenMP",
        "/onlyMagick"
    )
    if ($Linkage -eq "Static") {
        $ConfigureArgs += "/static"
        $ConfigureArgs += "/linkRuntime"
    } else {
        $ConfigureArgs += "/dynamic"
    }

    Write-Host "生成 ImageMagick $Linkage 解决方案..."
    Push-Location $ConfigureExe.Directory.FullName
    try {
        & $ConfigureExe.FullName @ConfigureArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Configure.exe 失败，退出码 $LASTEXITCODE。"
        }
    } finally {
        Pop-Location
    }

    $SolutionName = "IM7.$Linkage.$Arch.sln"
    $Solution = Join-Path $SourceRoot $SolutionName
    if (-not (Test-Path $Solution)) {
        throw "未找到生成的解决方案: $Solution"
    }
    Repair-GeneratedProjectToolCommands (Join-Path $SourceRoot "ProjectFiles")

    $BuildArgs = @(
        "/m",
        "/restore",
        "/p:Configuration=$Configuration",
        "/p:Platform=$Arch"
    )

    if ($FullBuild) {
        Invoke-MSBuild -MSBuild $MSBuild -Solution $Solution -ExtraArgs $BuildArgs
    } else {
        $Flavor = if ($Configuration -eq "Debug") { "DB" } else { "RL" }
        $CoderPrefix = if ($Linkage -eq "Static") { "CORE_$Flavor" } else { "IM_MOD_$Flavor" }
        $Targets = @(
            "CORE_${Flavor}_MagickCore_",
            "CORE_${Flavor}_MagickWand_",
            "${CoderPrefix}_heic_",
            "${CoderPrefix}_webp_"
        ) -join ";"

        try {
            Invoke-MSBuild -MSBuild $MSBuild -Solution $Solution -ExtraArgs ($BuildArgs + "/t:$Targets")
        } catch {
            Write-Warning "AVIF/WebP 最小目标构建失败，改为构建完整 ImageMagick 方案。原因: $($_.Exception.Message)"
            Invoke-MSBuild -MSBuild $MSBuild -Solution $Solution -ExtraArgs $BuildArgs
        }
    }
}

Write-Host "提取运行时/开发文件: $RuntimeRoot"
if (Test-Path $RuntimeRoot) {
    Remove-Item -LiteralPath $RuntimeRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $RuntimeRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $RuntimeRoot "include") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $RuntimeRoot "lib") | Out-Null

$Artifacts = Join-Path $SourceRoot "Artifacts"
$BinRoot = Join-Path $Artifacts "bin"
$LibRoot = Join-Path $Artifacts "lib"
$IncludeRoot = Join-Path $Artifacts "include"
$ConfigRoot = Join-Path $Artifacts "config"
$LicenseRoot = Join-Path $Artifacts "license"

if (-not (Test-Path $IncludeRoot)) {
    $Header = Get-NewestFile $SourceRoot "MagickWand.h"
    if (-not $Header) {
        throw "未找到 MagickWand.h，无法提取 include。"
    }
    $IncludeRoot = Split-Path -Parent $Header.Directory.FullName
}

Copy-ChildrenIfExists $IncludeRoot (Join-Path $RuntimeRoot "include")
$ImageMagickRoot = Join-Path $SourceRoot "ImageMagick"
foreach ($Dir in @("MagickWand", "MagickCore", "Magick++")) {
    $SourceDir = Join-Path $ImageMagickRoot $Dir
    if (Test-Path $SourceDir) {
        Copy-Item -LiteralPath $SourceDir -Destination (Join-Path $RuntimeRoot "include") -Recurse -Force
    }
}

Copy-FilesIfExists $BinRoot $RuntimeRoot "*.dll"
Copy-FilesIfExists $BinRoot $RuntimeRoot "*.exe"
Copy-FilesIfExists $BinRoot $RuntimeRoot "*.xml"
Copy-FilesIfExists $BinRoot $RuntimeRoot "*.icc"
Copy-FilesIfExists $BinRoot $RuntimeRoot "*.txt"
Copy-FilesIfExists $ConfigRoot $RuntimeRoot "*.xml"
Copy-FilesIfExists $ConfigRoot $RuntimeRoot "*.icc"
Copy-FilesIfExists $ConfigRoot $RuntimeRoot "*.txt"
Copy-FilesIfExists $LibRoot (Join-Path $RuntimeRoot "lib") "*.lib"

if (-not (Get-ChildItem -Path (Join-Path $RuntimeRoot "lib") -Filter "CORE_*_MagickWand_.lib" -File -ErrorAction SilentlyContinue)) {
    $WandLib = Get-NewestFile $SourceRoot "CORE_*_MagickWand_.lib"
    if ($WandLib) {
        Copy-FilesIfExists $WandLib.Directory.FullName (Join-Path $RuntimeRoot "lib") "*.lib"
    }
}

$ModuleRoot = Join-Path $BinRoot "modules"
if (-not (Test-Path $ModuleRoot)) {
    $HeicModule = Get-NewestFile $SourceRoot "IM_MOD_*_heic_.dll"
    if ($HeicModule) {
        $ModuleRoot = Split-Path -Parent (Split-Path -Parent $HeicModule.Directory.FullName)
    }
}
Copy-TreeIfExists $ModuleRoot $RuntimeRoot
Copy-TreeIfExists $LicenseRoot $RuntimeRoot

foreach ($Notice in @("LICENSE", "LICENSE.txt", "NOTICE", "NOTICE.txt")) {
    $Found = Get-ChildItem -Path $SourceRoot -Recurse -File -Filter $Notice -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($Found) {
        Copy-NoticeFile $Found $RuntimeRoot
    }
}

$CopiedDlls = @(Get-ChildItem -Path $RuntimeRoot -Filter "*.dll" -File -ErrorAction SilentlyContinue).Count
$CopiedLibs = @(Get-ChildItem -Path (Join-Path $RuntimeRoot "lib") -Filter "*.lib" -File -ErrorAction SilentlyContinue).Count

Write-Host ""
Write-Host "ImageMagick $Linkage runtime 已提取到:"
Write-Host "  $RuntimeRoot"
Write-Host "DLL 数量: $CopiedDlls"
Write-Host "LIB 数量: $CopiedLibs"
Write-Host "后续构建:"
Write-Host "  .\release.ps1 -MagickRoot `"$RuntimeRoot`""
Write-Host ""
Write-Host "说明:"
Write-Host "  默认 Static 会尽量减少分发 DLL；如果静态 delegate 链接失败，可改用 -Linkage Dynamic。"
Write-Host "  默认只构建 MagickCore/MagickWand 与 AVIF(WebP/HEIC) 和 WebP coder；需要完整输入格式支持时加 -FullBuild。"
