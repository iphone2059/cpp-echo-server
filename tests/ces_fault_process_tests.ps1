param(
    [Parameter(Mandatory)]
    [string] $DriverPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DriverPath -PathType Leaf)) {
    throw "server fault driver not found: $DriverPath"
}

function Invoke-FaultCase {
    param(
        [Parameter(Mandatory)]
        [string] $Mode,
        [Parameter(Mandatory)]
        [int] $ExpectedExitCode,
        [string] $ExpectedStage = ''
    )

    $errorPath = Join-Path $env:TEMP ("ces_fault_" + [Guid]::NewGuid().ToString('N') + '.err')
    $process = Start-Process -FilePath $DriverPath -ArgumentList @($Mode) -PassThru -Wait -WindowStyle Hidden -RedirectStandardError $errorPath
    try {
        if ($process.ExitCode -ne $ExpectedExitCode) {
            throw "server fault mode '$Mode' exited with $($process.ExitCode), expected $ExpectedExitCode"
        }
        if ($ExpectedStage -ne '') {
            $text = if (Test-Path -LiteralPath $errorPath) { [string](Get-Content -LiteralPath $errorPath -Raw) } else { '' }
            if ($text -notmatch [regex]::Escape($ExpectedStage)) {
                throw "server fault mode '$Mode' did not report stage '$ExpectedStage': $text"
            }
        }
    } finally {
        Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
        $process.Dispose()
    }
}

Invoke-FaultCase -Mode 'normal' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'notify_failure' -ExpectedExitCode 4 -ExpectedStage 'test notify failure'
Invoke-FaultCase -Mode 'notify_duplicate' -ExpectedExitCode 4 -ExpectedStage 'RIONotify duplicate arm'
Invoke-FaultCase -Mode 'notify_provider_failure' -ExpectedExitCode 4 -ExpectedStage 'RIONotify(server provider)'
Invoke-FaultCase -Mode 'notify_provider_duplicate' -ExpectedExitCode 4 -ExpectedStage 'RIONotify duplicate arm'
Invoke-FaultCase -Mode 'notify_precondition_duplicate' -ExpectedExitCode 4 -ExpectedStage 'duplicate notification arm'
Invoke-FaultCase -Mode 'corrupt_cq' -ExpectedExitCode 4

Write-Host 'PASS server fail-fast boundaries'
