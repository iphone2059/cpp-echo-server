param(
    [Parameter(Mandatory = $true)]
    [string] $ProjectRoot,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Named-module replacement for the header/implementation source policy. Every implementation file is a
# module interface unit (or a plain translation unit for the entry point and the test drivers), so the
# layout, module identity, import direction and forbidden-construct rules are asserted on the same .cpp tree.
$script:CesSourceModules = [ordered]@{
    'types.cpp'    = 'ces.types'
    'contract.cpp' = 'ces.contract'
    'native.cpp'   = 'ces.native'
    'engine.cpp'   = 'ces.engine'
    'internal.cpp' = 'ces.internal'
    'server.cpp'   = 'ces.server'
}
$script:CesPlainUnits = @('main.cpp')
$script:CesTestFiles = @('ces_test_support.cpp', 'ces_test_entry.cpp', 'ces_tests.cpp', 'ces_engine_tests.cpp', 'ces_pure_engine_tests.cpp', 'ces_fault_driver.cpp')
# Allowed import direction. ces.types is the pure vocabulary; the runtime imports the pure engine; the
# coordinator imports the runtime. The graph below is acyclic by construction and is asserted per file.
$script:CesModuleLayers = [ordered]@{
    'ces.types'    = @()
    'ces.contract' = @('ces.types')
    'ces.native'   = @('ces.types', 'ces.contract')
    'ces.engine'   = @('ces.types')
    'ces.internal' = @('ces.types', 'ces.engine', 'ces.contract', 'ces.native')
    'ces.server'   = @('ces.types', 'ces.engine', 'ces.internal', 'ces.native')
}
# Imports the Worker Runtime may only take inside the Native-free (pure engine) guard.
$script:CesGuardedImports = @{ 'ces.internal' = @('ces.contract', 'ces.native') }
$script:CesPureSourceFiles = @('types.cpp', 'engine.cpp')

function Find-CesPattern {
    param(
        [Parameter(Mandatory = $true)][string] $Pattern,
        [Parameter(Mandatory = $true)][string] $Label,
        [Parameter(Mandatory = $true)][System.IO.FileInfo[]] $Files,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]] $Failures
    )

    foreach ($file in $Files) {
        foreach ($match in @(Select-String -LiteralPath $file.FullName -Pattern $Pattern -AllMatches)) {
            $Failures.Add($Label + ': ' + $file.FullName + ':' + $match.LineNumber)
        }
    }
}

function Get-CesPolicyFailures {
    param([Parameter(Mandatory = $true)][string] $Root)

    $failures = [System.Collections.Generic.List[string]]::new()
    $sourceRoot = Join-Path $Root 'src'
    $testRoot = Join-Path $Root 'tests'
    $allFiles = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '[\\/](x64|logs|\.vs|\.git|obj)[\\/]' })

    # 1. Named modules removed every header: a .h/.hpp anywhere is a policy failure.
    $headers = @($allFiles | Where-Object { $_.Extension -in @('.h', '.hpp', '.hxx', '.hh', '.inc') })
    if ($headers.Count -ne 0) {
        $failures.Add('no header files are allowed; found: ' + (($headers | ForEach-Object Name) -join ', '))
    }

    # 2. Flat layout with exactly the seven implementation units and five test units.
    $expectedSources = @($script:CesSourceModules.Keys) + @($script:CesPlainUnits)
    $actualSources = @(Get-ChildItem -LiteralPath $sourceRoot -File | ForEach-Object Name)
    if (@(Compare-Object ($expectedSources | Sort-Object) ($actualSources | Sort-Object)).Count -ne 0 -or
        @(Get-ChildItem -LiteralPath $sourceRoot -Directory).Count -ne 0) {
        $failures.Add('src must contain exactly the flat module sources: ' + ($expectedSources -join ', '))
    }
    $actualTests = @(Get-ChildItem -LiteralPath $testRoot -File | Where-Object { $_.Extension -eq '.cpp' } | ForEach-Object Name)
    if (@(Compare-Object ($script:CesTestFiles | Sort-Object) ($actualTests | Sort-Object)).Count -ne 0) {
        $failures.Add('tests must contain exactly the flat test sources: ' + ($script:CesTestFiles -join ', '))
    }

    # 3. CMake must not return.
    $cmakeFiles = @($allFiles | Where-Object { $_.Name -eq 'CMakeLists.txt' -or $_.Name -eq 'CMakePresets.json' })
    if ($cmakeFiles.Count -ne 0 -or (Test-Path -LiteralPath (Join-Path $Root 'cmake') -PathType Container)) {
        $failures.Add('CMake is not part of the Visual Studio build; CMakeLists.txt and cmake/ must not exist')
    }

    # 4. Module identity, global module fragment and import section placement.
    foreach ($entry in $script:CesSourceModules.GetEnumerator()) {
        $file = Join-Path $sourceRoot $entry.Key
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        $lines = @(Get-Content -LiteralPath $file)
        $expected = 'export module ' + $entry.Value + ';'
        $declarations = @($lines | Where-Object { $_ -match '^\s*export module ' })
        if ($declarations.Count -ne 1 -or $declarations[0].Trim() -ne $expected) {
            $failures.Add($entry.Key + ' must declare exactly "' + $expected + '" (one module per file)')
        }
        $firstCode = @($lines | Where-Object { $_ -match '\S' -and $_ -notmatch '^\s*//' })[0]
        if ($firstCode.Trim() -ne 'module;') {
            $failures.Add($entry.Key + ' must open with the global module fragment "module;" before its module declaration')
        }
        $declarationLine = -1
        foreach ($index in 0..($lines.Count - 1)) {
            if ($lines[$index] -match '^\s*export module ' -and $declarationLine -lt 0) { $declarationLine = $index }
            if ($lines[$index] -notmatch '^\s*import\s') { continue }
            if ($index -lt $declarationLine) {
                $failures.Add($entry.Key + ':' + ($index + 1) + ': import declarations must follow the module declaration')
                continue
            }
            $imported = ($lines[$index] -replace '^\s*import\s+', '' -replace ';.*$', '').Trim()
            if ($imported -eq $entry.Value) {
                $failures.Add($entry.Key + ' must not import itself')
            }
            if (-not $script:CesModuleLayers[$entry.Value].Contains($imported)) {
                $failures.Add($entry.Key + ':' + ($index + 1) + ': ' + $imported + ' is not an allowed dependency of ' + $entry.Value)
            }
            if ($script:CesGuardedImports.ContainsKey($entry.Value) -and
                $script:CesGuardedImports[$entry.Value].Contains($imported)) {
                $guardDepth = 0
                for ($scan = 0; $scan -lt $index; $scan++) {
                    if ($lines[$scan] -match '^\s*#\s*if') { $guardDepth++ }
                    if ($lines[$scan] -match '^\s*#\s*endif') { $guardDepth-- }
                }
                if ($guardDepth -le 0) {
                    $failures.Add($entry.Key + ':' + ($index + 1) + ': ' + $imported + ' must stay inside the CES_PURE_ENGINE_ONLY guard')
                }
            }
        }
        $firstDeclaration = -1
        for ($index = 0; $index -lt $lines.Count; $index++) {
            $line = $lines[$index]
            if ($line -match '^\s*$' -or $line -match '^\s*//' -or $line -match '^\s*#') { continue }
            if ($line -match '^\s*(module|export module|import)\b') { continue }
            $firstDeclaration = $index
            break
        }
        if ($firstDeclaration -ge 0) {
            foreach ($index in $firstDeclaration..($lines.Count - 1)) {
                if ($lines[$index] -match '^\s*import\s') {
                    $failures.Add($entry.Key + ':' + ($index + 1) + ': import declarations must precede every other declaration')
                }
            }
        }
    }

    # 5. Plain translation units import modules instead of headers.
    foreach ($relative in @('src\main.cpp', 'tests\ces_test_entry.cpp', 'tests\ces_tests.cpp', 'tests\ces_engine_tests.cpp', 'tests\ces_pure_engine_tests.cpp', 'tests\ces_fault_driver.cpp')) {
        # The dispatcher only declares the suite entry points; it consumes no module and therefore
        # needs no import of its own.
        if ($relative -eq 'tests\ces_test_entry.cpp') { continue }
        $file = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        $lines = @(Get-Content -LiteralPath $file)
        if (@($lines | Where-Object { $_ -match '^\s*export module ' }).Count -ne 0) {
            $failures.Add($relative + ' must stay a plain translation unit without a module declaration')
        }
        if (@($lines | Where-Object { $_ -match '^\s*import\s' }).Count -eq 0) {
            $failures.Add($relative + ' must import the modules it consumes')
        }
        $firstImport = -1
        foreach ($index in 0..($lines.Count - 1)) {
            if ($lines[$index] -match '^\s*import\s') { $firstImport = $index; break }
        }
        if ($firstImport -ge 0) {
            for ($index = $firstImport + 1; $index -lt $lines.Count; $index++) {
                if ($lines[$index] -match '^\s*#\s*include') {
                    $failures.Add($relative + ':' + ($index + 1) + ': system headers must precede the imports so they attach to the global module')
                }
            }
        }
    }
    $support = Join-Path $testRoot 'ces_test_support.cpp'
    if (Test-Path -LiteralPath $support -PathType Leaf) {
        if (@(Select-String -LiteralPath $support -Pattern '^export module ces\.test\.support;').Count -ne 1) {
            $failures.Add('tests/ces_test_support.cpp must declare exactly "export module ces.test.support;"')
        }
    }

    # 6. Layer direction.
    $pureFiles = @($script:CesPureSourceFiles | ForEach-Object { Get-Item -LiteralPath (Join-Path $sourceRoot $_) })
    Find-CesPattern -Pattern '#\s*include\s*[<"](?:WinSock2|MSWSock|WS2tcpip|Windows)[^>"]*[>"]|import\s+ces\.native|\b(?:SOCKET|HANDLE|OVERLAPPED|RIO_(?:RQ|CQ|BUF|BUFFERID)|GetTickCount64|HeapAlloc|VirtualAlloc|RIOReceive|RIOSend|ERROR_[A-Z_]+|WSA[A-Z]+)\b' -Label 'native dependency in a pure engine module' -Files $pureFiles -Failures $failures
    $engineFile = Get-Item -LiteralPath (Join-Path $sourceRoot 'engine.cpp')
    Find-CesPattern -Pattern 'import\s+ces\.(?:internal|server)' -Label 'runtime dependency inside the pure engine' -Files @($engineFile) -Failures $failures
    $contractFile = Get-Item -LiteralPath (Join-Path $sourceRoot 'contract.cpp')
    Find-CesPattern -Pattern 'import\s+ces\.native' -Label 'native module dependency in the option contract' -Files @($contractFile) -Failures $failures
    $nativeFiles = @(Get-Item -LiteralPath (Join-Path $sourceRoot 'native.cpp'))
    Find-CesPattern -Pattern 'import\s+ces\.(?:engine|internal|server)|\b(?:ces_tcp_engine|ces_udp_engine|ces_acceptor_core|ces_engine_statistics|ces_engine_worker|ces_engine_acceptor|ces_timer_node|ces_timer_heap)\b' -Label 'business Core or Runtime dependency in Native' -Files $nativeFiles -Failures $failures
    $internalFile = Get-Item -LiteralPath (Join-Path $sourceRoot 'internal.cpp')
    Find-CesPattern -Pattern 'import\s+ces\.server' -Label 'coordinator import inside the Worker Runtime' -Files @($internalFile) -Failures $failures

    # 7. Forbidden constructs, project macros and non-RIO payload APIs.
    # Directory enumeration instead of path comparison: $env:TEMP can be handed out as an 8.3 short path.
    $implementation = @(Get-ChildItem -LiteralPath $sourceRoot -File | Where-Object { $_.Extension -eq '.cpp' }) +
        @(Get-ChildItem -LiteralPath $testRoot -File | Where-Object { $_.Extension -eq '.cpp' })
    Find-CesPattern -Pattern '\b(namespace|using|throw|try|catch|goto|dynamic_cast|typeid)\b|std::(function|bind|shared_ptr|async)|\.detach\s*\(' -Label 'forbidden C++ construct' -Files $implementation -Failures $failures
    Find-CesPattern -Pattern '^\s*#\s*define\b' -Label 'project macro in an implementation file' -Files $implementation -Failures $failures
    # The transport API deliberately shares the payload verb names used by Rust, Zig and Swift, so the
    # bare verbs are only scanned where a raw Winsock payload call would live.
    Find-CesPattern -Pattern '\b(sendto|recvfrom|WSASend|WSARecv|WSASendTo|WSARecvFrom)\s*\(' -Label 'non-RIO payload API' -Files $implementation -Failures $failures
    Find-CesPattern -Pattern '(?<![.\w>:])(send|recv)\s*\(' -Label 'non-RIO payload API' -Files $implementation -Failures $failures
    Find-CesPattern -Pattern '\b(CreateFile|CreateEvent|CreateMutex|CreateSemaphore|LoadLibrary|GetModuleHandle|MessageBox)\s*\(' -Label 'non-Unicode Windows API' -Files $implementation -Failures $failures
    Find-CesPattern -Pattern '^\s*#\s*include\s*[<"][^>"]+\.cpp[>"]' -Label 'implementation file included as a header' -Files $implementation -Failures $failures
    # Numeric contract constants are exported constant expressions now that macros cannot cross a module.
    foreach ($fileName in $script:CesSourceModules.Keys) {
        $file = Join-Path $sourceRoot $fileName
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        Find-CesPattern -Pattern '#\s*define\s+CES_' -Label 'numeric contract constant left as a macro' -Files @(Get-Item -LiteralPath $file) -Failures $failures
    }

    # 8. Only a build declaration or a source file can express a cross-project dependency. This
    # project must therefore name no other implementation anywhere.
    $dependencyFiles = @($implementation + @(Get-ChildItem -LiteralPath $Root -File -Recurse -ErrorAction SilentlyContinue | Where-Object {
                $_.FullName -notmatch '[\\/](x64|logs|\.vs|\.git)[\\/]' -and
                $_.FullName -notmatch '[\\/]tools[\\/]verification[\\/]' -and
                $_.FullName -ne $PSCommandPath -and
                ($_.Extension -in @('.ps1', '.vcxproj', '.slnx', '.props', '.targets', '.filters')) }))
    # This project is standalone: it may not name another implementation and nothing may reach
    # outside the project root.
    $ownName = Split-Path -Path $Root -Leaf
    foreach ($file in $dependencyFiles) {
        foreach ($match in @(Select-String -LiteralPath $file.FullName -Pattern '\b[a-z][a-z0-9]*-echo-(?:client|server)\b' -AllMatches)) {
            foreach ($found in $match.Matches) {
                if ($found.Value -ne $ownName) {
                    $failures.Add("foreign implementation reference '$($found.Value)': $($file.FullName):$($match.LineNumber)")
                }
            }
        }
    }
    Find-CesPattern -Pattern '\.\.[\\/]|add_subdirectory\s*\(|cmake\s+-S|cmake_minimum_required' -Label 'cross-project dependency' -Files $dependencyFiles -Failures $failures

    # 9. One project builds the implementation: product, test and pure-engine configurations of
    #    a single .vcxproj. The pure configuration must stay native free and every source must be
    #    listed by the project.
    $projects = @(Get-ChildItem -LiteralPath $Root -File -Filter *.vcxproj)
    if ($projects.Count -ne 1) {
        $failures.Add('expected exactly one Visual Studio project, found ' + $projects.Count)
    }
    $projectText = ($projects | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join [Environment]::NewLine
    $quote = [char]39
    function Get-CesConfigurationMarker {
        param([string] $Configuration, [string] $Kind)
        if ($Kind -eq 'group') {
            return '<ItemGroup Condition="' + $quote + '$(Configuration)|$(Platform)' + $quote + '==' + $quote + $Configuration + '|x64' + $quote + '">'
        }
        return '<ProjectConfiguration Include="' + $Configuration + '|x64">'
    }
    $configurations = @('Debug', 'Release', 'DebugTests', 'ReleaseTests', 'DebugPure', 'ReleasePure')
    foreach ($configuration in $configurations) {
        foreach ($kind in @('project', 'group')) {
            $marker = Get-CesConfigurationMarker -Configuration $configuration -Kind $kind
            if ($projectText.IndexOf($marker) -lt 0) {
                $failures.Add('project does not declare the ' + $kind + ' entry for ' + $configuration + '|x64')
            }
        }
    }
    function Get-CesGroupSources {
        param([string] $Text, [string] $Configuration)
        $marker = Get-CesConfigurationMarker -Configuration $Configuration -Kind 'group'
        $start = $Text.IndexOf($marker)
        if ($start -lt 0) { return @() }
        $end = $Text.IndexOf('</ItemGroup>', $start)
        $group = $Text.Substring($start, $end - $start)
        return @([regex]::Matches($group, 'Include="([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
    }
    $productSources = Get-CesGroupSources -Text $projectText -Configuration 'Release'
    $testSources = Get-CesGroupSources -Text $projectText -Configuration 'ReleaseTests'
    $pureSources = Get-CesGroupSources -Text $projectText -Configuration 'ReleasePure'
    if (@($productSources | Where-Object { $_ -like 'tests\*' }).Count -ne 0) {
        $failures.Add('the product configuration lists test sources')
    }
    if ($productSources -notcontains 'src\main.cpp') {
        $failures.Add('the product configuration does not list src\main.cpp')
    }
    if (@($testSources | Where-Object { $_ -eq 'src\main.cpp' }).Count -ne 0) {
        $failures.Add('the test configuration lists the product entry point src\main.cpp')
    }
    if ($testSources -notcontains 'tests\ces_test_entry.cpp') {
        $failures.Add('the test configuration does not list tests\ces_test_entry.cpp')
    }
    if ($pureSources -notcontains 'tests\ces_test_entry.cpp' -or $pureSources -notcontains 'tests\ces_pure_engine_tests.cpp') {
        $failures.Add('the pure-engine configuration does not list its entry and suite')
    }
    if (@($pureSources | Where-Object { $_ -like '*native*' -or $_ -eq 'src\server.cpp' -or $_ -eq 'src\contract.cpp' }).Count -ne 0) {
        $failures.Add('the pure-engine configuration lists a native or coordinator source')
    }
    if ($projectText -notmatch 'CES_PURE_ENGINE_ONLY') {
        $failures.Add('no configuration defines CES_PURE_ENGINE_ONLY')
    }
    $pureMarker = Get-CesConfigurationMarker -Configuration 'ReleasePure' -Kind 'group'
    $pureStart = $projectText.IndexOf($pureMarker)
    if ($pureStart -ge 0) {
        $pureEnd = $projectText.IndexOf('</ItemDefinitionGroup>', $pureStart)
        if ($pureEnd -gt $pureStart) {
            $segment = $projectText.Substring($pureStart, $pureEnd - $pureStart)
            if ($segment -match 'ws2_32') { $failures.Add('the pure-engine configuration links ws2_32') }
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Root 'src'), (Join-Path $Root 'tests') -File -Filter *.cpp) {
        $relative = [System.IO.Path]::GetRelativePath($Root, $file.FullName)
        if ($projectText -notmatch [regex]::Escape($relative)) {
            $failures.Add('source is not listed in the project: ' + $relative)
        }
    }

    return $failures
}

$cesRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$cesFailures = @(Get-CesPolicyFailures -Root $cesRoot)

if (-not $SelfTest) {
    if ($cesFailures.Count -ne 0) {
        $cesFailures | ForEach-Object { Write-Host $_ }
        throw 'server source policy failed with ' + $cesFailures.Count + ' violation(s)'
    }
    Write-Host 'PASS server source policy'
    exit 0
}

# Negative controls: every mutation must be detected, otherwise the positive result above proves nothing.
if ($cesFailures.Count -ne 0) {
    $cesFailures | ForEach-Object { Write-Host $_ }
    throw 'the working tree already violates the policy; fix it before running the negative controls'
}
$newline = [Environment]::NewLine
$guardBlock = '#if !defined(CES_PURE_ENGINE_ONLY)' + $newline + 'import ces.native;' + $newline + '#endif'
$cesMutations = @(
    @{ Name = 'header file reintroduced'; Apply = { param($r) Set-Content -LiteralPath (Join-Path $r 'src\legacy.h') -Value '#pragma once' } },
    @{ Name = 'wrong module name'; Apply = { param($r) $p = Join-Path $r 'src\engine.cpp'; (Get-Content -Raw $p).Replace('export module ces.engine;', 'export module ces.wrong;') | Set-Content -LiteralPath $p -NoNewline } },
    @{ Name = 'namespace reintroduced'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\engine.cpp') -Value 'namespace ces_probe {}' } },
    @{ Name = 'pure engine imports native'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\engine.cpp') -Value 'import ces.native;' } },
    @{ Name = 'engine imports the runtime'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\engine.cpp') -Value 'import ces.internal;' } },
    @{ Name = 'runtime imports the coordinator'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\internal.cpp') -Value 'import ces.server;' } },
    @{ Name = 'native import left unguarded in the runtime'; Apply = { param($r) $p = Join-Path $r 'src\internal.cpp'; (Get-Content -Raw $p).Replace($guardBlock, 'import ces.native;') | Set-Content -LiteralPath $p -NoNewline } },
    @{ Name = 'project macro reintroduced'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\types.cpp') -Value '#define CES_PROBE 1' } },
    @{ Name = 'non-RIO payload API'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\native.cpp') -Value 'int ces_probe_send(SOCKET socket_value) { return sendto(socket_value, nullptr, 0, 0, nullptr, 0); }' } },
    @{ Name = 'implementation file included as a header'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\main.cpp') -Value '#include "types.cpp"' } },
    @{ Name = 'system header after the import section'; Apply = { param($r) Add-Content -LiteralPath (Join-Path $r 'src\main.cpp') -Value '#include <vector>' } },
    @{ Name = 'CMake reintroduced'; Apply = { param($r) Set-Content -LiteralPath (Join-Path $r 'CMakeLists.txt') -Value 'project(probe)' } }
)
foreach ($mutation in $cesMutations) {
    $temp = Join-Path $env:TEMP ('ces_policy_negative_' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    try {
        Copy-Item -LiteralPath (Join-Path $cesRoot 'src') -Destination $temp -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $cesRoot 'tests') -Destination $temp -Recurse -Force
        Get-ChildItem -LiteralPath $cesRoot -File | Where-Object { $_.Extension -in @('.ps1', '.vcxproj', '.slnx') } |
            Copy-Item -Destination $temp -Force
        & $mutation.Apply $temp
        $detected = @(Get-CesPolicyFailures -Root $temp)
        if ($detected.Count -eq 0) {
            throw 'negative control "' + $mutation.Name + '" was NOT detected; the policy lost its strength'
        }
        Write-Host ('PASS negative control: ' + $mutation.Name)
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Write-Host ('PASS server source policy with ' + $cesMutations.Count + ' negative controls')
