param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$platform = 'x64'
$project = Join-Path $projectRoot 'cpp-echo-server.vcxproj'
$outputDirectory = Join-Path $projectRoot (Join-Path $platform $Configuration)
$logDirectory = Join-Path $projectRoot 'logs'

function Get-BuildSourceSnapshot {
    param([string] $Root)

    $pending = [System.Collections.Generic.Stack[string]]::new()
    $entries = [System.Collections.Generic.List[string]]::new()
    $pending.Push($Root)
    $excluded = @('build', '.git', '.build', '.swiftpm', '.vs', 'logs', 'x64') |
        ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $Root $_)) }
    while ($pending.Count -ne 0) {
        $directory = $pending.Pop()
        foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
            if ($item.FullName -in $excluded) { continue }
            if ($item.PSIsContainer) {
                $pending.Push($item.FullName)
            } else {
                $relative = [System.IO.Path]::GetRelativePath($Root, $item.FullName)
                $digest = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
                $entries.Add("$relative=$digest")
            }
        }
    }
    return (@($entries | Sort-Object) -join "`n")
}

function Get-MSBuildPath {
    $candidates = @(
        (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'))
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not [string]::IsNullOrWhiteSpace($vsPath)) {
            $candidate = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    throw 'MSBuild.exe was not found for Visual Studio 2026.'
}

# Every suite writes one retained log next to the configuration output. A failing gate keeps its log.
function Invoke-CesGate {
    param(
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][scriptblock] $Command
    )

    $logPath = Join-Path $logDirectory ("$($Configuration.ToLowerInvariant())-$Name.log")
    Write-Host "== $Name"
    $global:LASTEXITCODE = 0
    & $Command *> $logPath
    $exitCode = $LASTEXITCODE
    $tail = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue | Where-Object { $_ -match '\S' })
    if ($tail.Count -gt 0) { Write-Host $tail[-1] }
    if ($exitCode -ne 0) {
        Get-Content -LiteralPath $logPath | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        throw "$Name failed with exit code $exitCode; full log: $logPath"
    }
}

New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null

$sourceBefore = Get-BuildSourceSnapshot -Root $projectRoot
try {
    $formatterCommand = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    $formatter = if ($null -ne $formatterCommand) {
        $formatterCommand.Source
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found.' }
        $vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-format.exe'
    }
    if (-not (Test-Path -LiteralPath $formatter -PathType Leaf)) {
        throw 'clang-format.exe was not found in PATH or the Visual Studio LLVM tools.'
    }
    $formatFiles = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests') -File -Recurse |
        Where-Object { $_.Extension -eq '.cpp' }
    foreach ($file in $formatFiles) {
        & $formatter --dry-run --Werror --style=file $file.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Formatting check failed for $($file.FullName); run format.ps1 explicitly."
        }
    }

    $msbuild = Get-MSBuildPath
    $buildLog = Join-Path $logDirectory ("$($Configuration.ToLowerInvariant())-msbuild.log")
    $buildConfigurations = @($Configuration, ($Configuration + 'Tests'), ($Configuration + 'Pure'))
    foreach ($buildConfiguration in $buildConfigurations) {
        & $msbuild $project /p:Configuration=$buildConfiguration /p:Platform=$platform /m /nologo /v:minimal "/flp:LogFile=$buildLog;Verbosity=normal" /clp:Summary
        if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for $buildConfiguration/$platform; full log: $buildLog" }
    }
    $warnings = @(Select-String -LiteralPath $buildLog -Pattern ': warning|warning MSB|warning LNK|warning C' -ErrorAction SilentlyContinue)
    if ($warnings.Count -ne 0) {
        $warnings | Select-Object -First 10 | ForEach-Object { Write-Host $_.Line }
        throw "MSBuild reported $($warnings.Count) warning(s); /WX builds must stay at zero warnings. Log: $buildLog"
    }

    $testOutput = Join-Path $projectRoot (Join-Path $platform ($Configuration + 'Tests'))
    $pureOutput = Join-Path $projectRoot (Join-Path $platform ($Configuration + 'Pure'))
    $testsExe = Join-Path $testOutput 'cpp-echo-server-tests.exe'
    $pureEngineTestsExe = Join-Path $pureOutput 'cpp-echo-server-pure-tests.exe'
    $serverExe = Join-Path $outputDirectory 'cpp-echo-server.exe'

    Invoke-CesGate -Name 'unit-contract' -Command { & $testsExe unit }
    Invoke-CesGate -Name 'engine' -Command { & $testsExe engine }
    Invoke-CesGate -Name 'pure-engine' -Command { & $pureEngineTestsExe pure }
    Invoke-CesGate -Name 'fault-boundaries' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\ces_fault_process_tests.ps1') -DriverPath $testsExe
    }
    Invoke-CesGate -Name 'source-policy' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\ces_source_policy.ps1') -ProjectRoot $projectRoot
    }
    Invoke-CesGate -Name 'process-loopback' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\ces_process_tests.ps1') -ServerPath $serverExe
    }
    Invoke-CesGate -Name 'reset-storm' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\ces_reset_storm_tests.ps1') -ServerPath $serverExe -Label 'cpp-echo-server'
    }
} finally {
    $sourceAfter = Get-BuildSourceSnapshot -Root $projectRoot
    if ($sourceAfter -cne $sourceBefore) { throw 'Build/test modified source file contents.' }
}

Write-Host "$Configuration/$platform gates passed: MSBuild 0 warnings, unit/contract, engine, pure-engine, fault boundaries, source policy, process loopback and reset storm."
