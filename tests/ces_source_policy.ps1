param(
    [Parameter(Mandatory)]
    [string] $ProjectRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$cesRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$cesSources = @(Get-ChildItem -LiteralPath (Join-Path $cesRoot 'include'), (Join-Path $cesRoot 'src'),
    (Join-Path $cesRoot 'tests') -File -Recurse | Where-Object {
        $_.Extension -in @('.h', '.hpp', '.c', '.cc', '.cpp', '.cxx')
    })
$cesFailures = [System.Collections.Generic.List[string]]::new()

function Test-CesPattern {
    param([string] $Pattern, [string] $Label, [System.IO.FileInfo[]] $Files)
    foreach ($file in $Files) {
        $matches = @(Select-String -LiteralPath $file.FullName -Pattern $Pattern -AllMatches)
        foreach ($match in $matches) {
            $cesFailures.Add("$Label`: $($file.FullName):$($match.LineNumber)")
        }
    }
}

Test-CesPattern -Pattern '\b(namespace|using|throw|try|catch|goto|dynamic_cast|typeid)\b|std::(function|bind|shared_ptr|async)|\.detach\s*\(' -Label 'forbidden C++ construct' -Files $cesSources
Test-CesPattern -Pattern '^\s*#\s*define\b' -Label 'project macro in implementation file' -Files @($cesSources | Where-Object Extension -In @('.c', '.cc', '.cpp', '.cxx'))
Test-CesPattern -Pattern '^\s*struct\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:\{|;)' -Label 'plain structure declared in implementation file' -Files @($cesSources | Where-Object Extension -In @('.c', '.cc', '.cpp', '.cxx'))
Test-CesPattern -Pattern '\b(send|recv|sendto|recvfrom|WSASend|WSARecv)\s*\(' -Label 'non-RIO payload API' -Files $cesSources
Test-CesPattern -Pattern '\b(CreateFile|CreateEvent|CreateMutex|CreateSemaphore|LoadLibrary|GetModuleHandle|MessageBox)\s*\(' -Label 'non-Unicode Windows API' -Files $cesSources

$cesOwnedFiles = @(Get-ChildItem -LiteralPath $cesRoot -File -Recurse | Where-Object {
        $_.FullName -notmatch '[\\/]build[\\/]' -and $_.FullName -notmatch '[\\/]tools[\\/]verification[\\/]' -and $_.FullName -ne $PSCommandPath
    })
Test-CesPattern -Pattern 'cpp-echo-client|\.\.[\\/].*(common|shared)|add_subdirectory\s*\(' -Label 'cross-project dependency' -Files $cesOwnedFiles

if ($cesFailures.Count -ne 0) {
    $cesFailures | ForEach-Object { Write-Error $_ }
    throw "server source policy failed with $($cesFailures.Count) violation(s)"
}

Write-Host 'PASS server source policy'

