param(
    [Parameter(Mandatory)]
    [string] $DriverPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DriverPath -PathType Leaf)) {
    throw "server test executable not found: $DriverPath"
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
    $process = Start-Process -FilePath $DriverPath -ArgumentList @('fault', $Mode) -PassThru -WindowStyle Hidden -RedirectStandardError $errorPath
    try {
        if (-not $process.WaitForExit(10000)) {
            $process.Kill($true)
            throw "server fault mode '$Mode' did not retire within 10 seconds"
        }
        if ($process.ExitCode -ne $ExpectedExitCode) {
            throw "server fault mode '$Mode' exited with $($process.ExitCode), expected $ExpectedExitCode"
        }
        $text = if (Test-Path -LiteralPath $errorPath) { [string](Get-Content -LiteralPath $errorPath -Raw) } else { '' }
        if ($ExpectedExitCode -eq 0 -and -not [string]::IsNullOrWhiteSpace($text)) { throw "$Mode unexpectedly wrote stderr: $text" }
        if ($ExpectedStage -ne '') {
            if ($text -notmatch [regex]::Escape($ExpectedStage)) {
                throw "server fault mode '$Mode' did not report stage '$ExpectedStage': $text"
            }
        }
    } finally {
        if (-not $process.HasExited) { $process.Kill($true); if (-not $process.WaitForExit(5000)) { throw "server fault mode '$Mode' cleanup timeout" } }
        Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
        $process.Dispose()
    }
}

Invoke-FaultCase -Mode 'normal' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'handoff_positive' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'handoff_invalid' -ExpectedExitCode 4 -ExpectedStage 'AcceptEx handoff invariant failed: native_error=10038'
Invoke-FaultCase -Mode 'accept_retry_64' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'accept_retry_10054' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'accept_retry_10053' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'notify_failure' -ExpectedExitCode 4 -ExpectedStage 'test notify failure'
Invoke-FaultCase -Mode 'notify_duplicate' -ExpectedExitCode 4 -ExpectedStage 'RIONotify duplicate arm'
Invoke-FaultCase -Mode 'notify_provider_failure' -ExpectedExitCode 4 -ExpectedStage 'RIONotify(server provider)'
Invoke-FaultCase -Mode 'notify_provider_duplicate' -ExpectedExitCode 4 -ExpectedStage 'RIONotify duplicate arm'
Invoke-FaultCase -Mode 'notify_precondition_duplicate' -ExpectedExitCode 4 -ExpectedStage 'duplicate notification arm'
Invoke-FaultCase -Mode 'corrupt_cq' -ExpectedExitCode 4
Invoke-FaultCase -Mode 'completion_positive' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'completion_oversized' -ExpectedExitCode 4 -ExpectedStage 'worker completion exceeds posted range failed: native_error=13'
Invoke-FaultCase -Mode 'completion_stale_generation' -ExpectedExitCode 4 -ExpectedStage 'worker completion generation failed: native_error=996'
Invoke-FaultCase -Mode 'completion_stale_token' -ExpectedExitCode 4 -ExpectedStage 'worker completion identity failed: native_error=13'
Invoke-FaultCase -Mode 'release_positive' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'release_outstanding' -ExpectedExitCode 4 -ExpectedStage 'worker release precondition failed: native_error=996'
Invoke-FaultCase -Mode 'resource_deinit_outstanding' -ExpectedExitCode 4 -ExpectedStage 'worker release precondition failed: native_error=996'
Invoke-FaultCase -Mode 'runtime_deinit_outstanding' -ExpectedExitCode 4 -ExpectedStage 'worker release precondition failed: native_error=996'
Invoke-FaultCase -Mode 'runtime_deinit_positive' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'resource_deinit_positive' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'acceptor_deinit_positive' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'acceptor_deinit_posted' -ExpectedExitCode 4 -ExpectedStage 'TCP acceptor terminal operations failed: native_error=996'
Invoke-FaultCase -Mode 'acceptor_deinit_control' -ExpectedExitCode 4 -ExpectedStage 'TCP acceptor terminal operations failed: native_error=996'

Write-Host 'PASS server fail-fast boundaries'
