$ErrorActionPreference = 'Stop'

# Named modules removed the include/ tree: every formatted input is a .cpp module interface unit,
# a module implementation part or a plain test translation unit.
$formatterCommand = Get-Command clang-format.exe -ErrorAction SilentlyContinue
if ($null -eq $formatterCommand) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'clang-format.exe was not found in PATH or Visual Studio.'
    }
    $vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($vsPath)) { throw 'Visual Studio with clang-format was not found.' }
    $formatter = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-format.exe'
} else { $formatter = $formatterCommand.Source }
if (-not (Test-Path -LiteralPath $formatter -PathType Leaf)) { throw 'clang-format.exe was not found.' }
$files = Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src'), (Join-Path $PSScriptRoot 'tests') -File -Recurse |
    Where-Object { $_.Extension -eq '.cpp' }
foreach ($file in $files) {
    & $formatter -i --style=file $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "clang-format failed for $($file.FullName)" }
}
