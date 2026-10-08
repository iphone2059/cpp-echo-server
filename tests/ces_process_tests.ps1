param(
    [Parameter(Mandatory)]
    [string] $ServerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ServerPath -PathType Leaf)) {
    throw "server executable not found: $ServerPath"
}

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return ([System.Net.IPEndPoint] $listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Get-FreeUdpPort {
    $socket = [System.Net.Sockets.UdpClient]::new(0, [System.Net.Sockets.AddressFamily]::InterNetwork)
    try {
        return ([System.Net.IPEndPoint] $socket.Client.LocalEndPoint).Port
    } finally {
        $socket.Dispose()
    }
}

function Wait-TcpReady {
    param([int] $Port, [System.Diagnostics.Process] $Process)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            throw "TCP server exited early with code $($Process.ExitCode)"
        }
        $probe = [System.Net.Sockets.TcpClient]::new()
        try {
            $probe.Connect([System.Net.IPAddress]::Loopback, $Port)
            return
        } catch [System.Net.Sockets.SocketException] {
            Start-Sleep -Milliseconds 20
        } finally {
            $probe.Dispose()
        }
    }
    throw 'TCP server did not become ready'
}

function Get-FinalStatistics {
    param([string] $Text, [string] $Protocol)
    $lines = @($Text -split '\r?\n' | Where-Object { $_ -match "^final protocol=$Protocol " })
    if ($lines.Count -ne 1) { throw "expected one $Protocol final statistics line: $Text" }
    $fields = @{}
    foreach ($match in [regex]::Matches($lines[0], '(?:^| )([a-z_]+)=([0-9]+)(?= |$)')) {
        $fields[$match.Groups[1].Value] = [UInt64]::Parse($match.Groups[2].Value)
    }
    foreach ($name in @('workers', 'completions', 'receives', 'sends', 'bytes', 'received_bytes', 'sent_bytes',
        'network_errors', 'rejected')) {
        if (-not $fields.ContainsKey($name)) { throw "missing $name in final statistics: $Text" }
    }
    return $fields
}

$tcpPort = Get-FreeTcpPort
$tcpOutputPath = [System.IO.Path]::GetTempFileName()
$tcp = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $tcpPort, '/w', '2', '/q',
    '/threads', '2', '/cq', '1024', '/memory', '67108864', '/stats') -RedirectStandardOutput $tcpOutputPath `
    -PassThru -NoNewWindow
try {
    Wait-TcpReady -Port $tcpPort -Process $tcp
    $client = [System.Net.Sockets.TcpClient]::new()
    $client.Connect([System.Net.IPAddress]::Loopback, $tcpPort)
    try {
        $stream = $client.GetStream()
        $payload = [byte[]]::new(131071)
        for ($index = 0; $index -lt $payload.Length; $index++) {
            $payload[$index] = [byte] ($index -band 255)
        }
        $stream.Write($payload)
        $received = [byte[]]::new($payload.Length)
        $offset = 0
        while ($offset -lt $received.Length) {
            $count = $stream.Read($received, $offset, $received.Length - $offset)
            if ($count -eq 0) { throw 'TCP peer closed before full echo' }
            $offset += $count
        }
        if ([Convert]::ToBase64String($payload) -ne [Convert]::ToBase64String($received)) {
            throw 'TCP echo mismatch'
        }
    } finally {
        $client.Dispose()
    }
    $tcp.WaitForExit(7000) | Out-Null
    if (-not $tcp.HasExited -or $tcp.ExitCode -ne 0) { throw 'TCP server did not stop cleanly' }
    $tcpText = Get-Content -LiteralPath $tcpOutputPath -Raw
    if ($tcpText -notmatch 'final protocol=tcp .*accepted=[1-9][0-9]* active=0 outstanding=0 .*bytes=[1-9][0-9]*') {
        throw "TCP final statistics are missing or incomplete: $tcpText"
    }
    $tcpStats = Get-FinalStatistics -Text $tcpText -Protocol 'tcp'
    if ($tcpStats.workers -ne 2 -or $tcpStats.received_bytes -ne $payload.Length -or
        $tcpStats.sent_bytes -ne $payload.Length -or $tcpStats.bytes -ne $tcpStats.sent_bytes -or
        $tcpStats.network_errors -ne 0 -or $tcpStats.rejected -ne 0 -or
        $tcpStats.completions -lt ($tcpStats.receives + $tcpStats.sends)) {
        throw "TCP native byte counts or clean-stop accounting are incorrect: $tcpText"
    }
} finally {
    if (-not $tcp.HasExited) { $tcp.Kill($true) }
    $tcp.Dispose()
    Remove-Item -LiteralPath $tcpOutputPath -Force -ErrorAction SilentlyContinue
}

$capacityPort = Get-FreeTcpPort
$capacityOutputPath = [System.IO.Path]::GetTempFileName()
$capacityServer = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $capacityPort, '/w', '3',
    '/q', '/threads', '1', '/cq', '64', '/memory', '16777216', '/stats') `
    -RedirectStandardOutput $capacityOutputPath -PassThru -NoNewWindow
$capacityClients = [System.Collections.Generic.List[System.Net.Sockets.TcpClient]]::new()
$capacityRejectedClient = $null
try {
    # The first retained client is also the readiness probe, so all 32 slots are deterministic.
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(2)
    for ($index = 0; $index -lt 32; $index++) {
        $connection = $null
        while ($null -eq $connection) {
            $candidate = [System.Net.Sockets.TcpClient]::new()
            try {
                $candidate.Connect([System.Net.IPAddress]::Loopback, $capacityPort)
                $connection = $candidate
            } catch [System.Net.Sockets.SocketException] {
                $candidate.Dispose()
                if ($index -ne 0 -or $capacityServer.HasExited -or [DateTime]::UtcNow -ge $readyDeadline) { throw }
                Start-Sleep -Milliseconds 20
            }
        }
        $capacityClients.Add($connection)
        $connection.ReceiveTimeout = 1000
        $stream = $connection.GetStream()
        $stream.WriteByte(42)
        if ($stream.ReadByte() -ne 42) { throw "capacity connection $index failed its echo" }
    }
    $capacityRejectedClient = [System.Net.Sockets.TcpClient]::new()
    $capacityRejectedClient.ReceiveTimeout = 1000
    $capacityRejectedClient.Connect([System.Net.IPAddress]::Loopback, $capacityPort)
    try {
        if ($capacityRejectedClient.GetStream().ReadByte() -ne -1) {
            throw 'server admitted a TCP connection beyond its CQ capacity'
        }
    } catch [System.IO.IOException] {
        if ($_.Exception.InnerException -isnot [System.Net.Sockets.SocketException] -or
            $_.Exception.InnerException.SocketErrorCode -eq [System.Net.Sockets.SocketError]::TimedOut) { throw }
    }
    $stream = $capacityClients[0].GetStream()
    $stream.WriteByte(43)
    if ($stream.ReadByte() -ne 43) { throw 'capacity rejection stopped an existing connection' }
    $capacityServer.WaitForExit(7000) | Out-Null
    if (-not $capacityServer.HasExited -or $capacityServer.ExitCode -ne 0) {
        throw 'capacity-limited TCP server did not stop cleanly'
    }
    $capacityText = Get-Content -LiteralPath $capacityOutputPath -Raw
    $capacityStats = Get-FinalStatistics -Text $capacityText -Protocol 'tcp'
    if ($capacityStats.accepted -ne 32 -or $capacityStats.rejected -ne 1 -or $capacityStats.network_errors -ne 0 -or
        $capacityStats.received_bytes -ne 33 -or $capacityStats.sent_bytes -ne 33 -or
        $capacityStats.bytes -ne 33 -or $capacityStats.workers -ne 1 -or $capacityStats.active -ne 0) {
        throw "capacity rejection or controlled-stop statistics are incorrect: $capacityText"
    }
} finally {
    foreach ($connection in $capacityClients) { $connection.Dispose() }
    if ($null -ne $capacityRejectedClient) { $capacityRejectedClient.Dispose() }
    if (-not $capacityServer.HasExited) { $capacityServer.Kill($true) }
    $capacityServer.Dispose()
    Remove-Item -LiteralPath $capacityOutputPath -Force -ErrorAction SilentlyContinue
}

$timeoutPort = Get-FreeTcpPort
$timeoutOutputPath = [System.IO.Path]::GetTempFileName()
$timeoutServer = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $timeoutPort, '/t', '1',
    '/w', '3', '/q', '/threads', '1', '/cq', '128', '/memory', '16777216') `
    -RedirectStandardOutput $timeoutOutputPath -PassThru -NoNewWindow
try {
    Wait-TcpReady -Port $timeoutPort -Process $timeoutServer
    $socket = [System.Net.Sockets.Socket]::new([System.Net.Sockets.AddressFamily]::InterNetwork,
        [System.Net.Sockets.SocketType]::Stream, [System.Net.Sockets.ProtocolType]::Tcp)
    try {
        $socket.ReceiveTimeout = 800
        $socket.Connect([System.Net.IPAddress]::Loopback, $timeoutPort)
        Start-Sleep -Milliseconds 1400
        $buffer = [byte[]]::new(1)
        try {
            $count = $socket.Receive($buffer)
            if ($count -ne 0) { throw 'idle TCP connection returned unexpected data' }
        } catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -eq [System.Net.Sockets.SocketError]::TimedOut) {
                throw 'TCP /t did not close the idle connection'
            }
        }
    } finally {
        $socket.Dispose()
    }
    $timeoutServer.WaitForExit(6000) | Out-Null
    if (-not $timeoutServer.HasExited -or $timeoutServer.ExitCode -ne 0) {
        throw 'TCP timeout server did not stop cleanly'
    }
} finally {
    if (-not $timeoutServer.HasExited) { $timeoutServer.Kill($true) }
    $timeoutServer.Dispose()
    Remove-Item -LiteralPath $timeoutOutputPath -Force -ErrorAction SilentlyContinue
}

$stormPort = Get-FreeTcpPort
$stormOutputPath = [System.IO.Path]::GetTempFileName()
$stormServer = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $stormPort, '/w', '2', '/q',
    '/threads', '4', '/cq', '2048', '/memory', '134217728') `
    -RedirectStandardOutput $stormOutputPath -PassThru -NoNewWindow
$stormJobs = @()
try {
    Wait-TcpReady -Port $stormPort -Process $stormServer
    foreach ($worker in 1..4) {
        $stormJobs += Start-Job -ArgumentList $stormPort -ScriptBlock {
            param($Port)
            $deadline = [DateTime]::UtcNow.AddMilliseconds(2300)
            while ([DateTime]::UtcNow -lt $deadline) {
                $socket = [System.Net.Sockets.TcpClient]::new()
                try {
                    $socket.Connect([System.Net.IPAddress]::Loopback, $Port)
                } catch [System.Net.Sockets.SocketException] {
                } finally {
                    $socket.Dispose()
                }
            }
        }
    }
    $stormServer.WaitForExit(7000) | Out-Null
    if (-not $stormServer.HasExited -or $stormServer.ExitCode -ne 0) {
        throw 'TCP connect-storm server did not stop cleanly'
    }
    foreach ($job in $stormJobs) {
        if (-not (Wait-Job -Job $job -Timeout 5)) { throw 'TCP connect-storm job did not converge' }
        Receive-Job -Job $job -ErrorAction Stop | Out-Null
    }
} finally {
    foreach ($job in $stormJobs) {
        Stop-Job -Job $job -ErrorAction SilentlyContinue
        Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    }
    if (-not $stormServer.HasExited) { $stormServer.Kill($true) }
    $stormServer.Dispose()
    Remove-Item -LiteralPath $stormOutputPath -Force -ErrorAction SilentlyContinue
}

$udpPort = Get-FreeUdpPort
$udpOutputPath = [System.IO.Path]::GetTempFileName()
$udp = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'udp', '/s', $udpPort, '/w', '2', '/q',
    '/k', '64', '/cq', '1024', '/memory', '67108864', '/stats') -RedirectStandardOutput $udpOutputPath `
    -PassThru -NoNewWindow
try {
    Start-Sleep -Milliseconds 250
    if ($udp.HasExited) { throw "UDP server exited early with code $($udp.ExitCode)" }
    $client = [System.Net.Sockets.UdpClient]::new(0, [System.Net.Sockets.AddressFamily]::InterNetwork)
    try {
        $client.Client.ReceiveTimeout = 3000
        $client.Connect([System.Net.IPAddress]::Loopback, $udpPort)
        foreach ($size in @(0, 1, 65507)) {
            $payload = [byte[]]::new($size)
            for ($index = 0; $index -lt $payload.Length; $index++) {
                $payload[$index] = [byte] (($index * 17) -band 255)
            }
            [void] $client.Send($payload, $payload.Length)
            $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $received = $client.Receive([ref] $remote)
            if ([Convert]::ToBase64String($payload) -ne [Convert]::ToBase64String($received)) {
                throw "UDP echo mismatch for payload size $size"
            }
        }
    } finally {
        $client.Dispose()
    }
    $udp.WaitForExit(7000) | Out-Null
    if (-not $udp.HasExited -or $udp.ExitCode -ne 0) { throw 'UDP server did not stop cleanly' }
    $udpText = Get-Content -LiteralPath $udpOutputPath -Raw
    if ($udpText -notmatch 'final protocol=udp .*active=0 outstanding=0 completions=[1-9][0-9]* receives=[1-9][0-9]* sends=3 .*bytes=65508') {
        throw "UDP final statistics are missing or incomplete: $udpText"
    }
    $udpStats = Get-FinalStatistics -Text $udpText -Protocol 'udp'
    if ($udpStats.workers -ne 1 -or $udpStats.receives -ne 3 -or $udpStats.sends -ne 3 -or
        $udpStats.received_bytes -ne 65508 -or $udpStats.sent_bytes -ne 65508 -or
        $udpStats.bytes -ne $udpStats.sent_bytes -or $udpStats.network_errors -ne 0 -or $udpStats.rejected -ne 0 -or
        $udpStats.completions -le ($udpStats.receives + $udpStats.sends)) {
        throw "UDP zero-datagram byte counts or cancelled-completion accounting are incorrect: $udpText"
    }
} finally {
    if (-not $udp.HasExited) { $udp.Kill($true) }
    $udp.Dispose()
    Remove-Item -LiteralPath $udpOutputPath -Force -ErrorAction SilentlyContinue
}

$udpTrafficPort = Get-FreeUdpPort
$udpTrafficOutputPath = [System.IO.Path]::GetTempFileName()
$udpTraffic = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'udp', '/s', $udpTrafficPort, '/w', '2',
    '/q', '/k', '64', '/cq', '1024', '/memory', '67108864') `
    -RedirectStandardOutput $udpTrafficOutputPath -PassThru -NoNewWindow
$udpTrafficJob = $null
try {
    Start-Sleep -Milliseconds 250
    if ($udpTraffic.HasExited) { throw "UDP traffic server exited early with code $($udpTraffic.ExitCode)" }
    $udpTrafficJob = Start-Job -ArgumentList $udpTrafficPort -ScriptBlock {
        param($Port)
        $client = [System.Net.Sockets.UdpClient]::new(0, [System.Net.Sockets.AddressFamily]::InterNetwork)
        try {
            $client.Client.ReceiveTimeout = 200
            $client.Connect([System.Net.IPAddress]::Loopback, $Port)
            $payload = [byte[]]::new(1200)
            $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $deadline = [DateTime]::UtcNow.AddMilliseconds(2300)
            while ([DateTime]::UtcNow -lt $deadline) {
                try {
                    [void] $client.Send($payload, $payload.Length)
                    [void] $client.Receive([ref] $remote)
                } catch [System.Net.Sockets.SocketException] {
                }
            }
        } finally {
            $client.Dispose()
        }
    }
    $udpTraffic.WaitForExit(7000) | Out-Null
    if (-not $udpTraffic.HasExited -or $udpTraffic.ExitCode -ne 0) {
        throw 'UDP traffic server did not drain cleanly'
    }
    if (-not (Wait-Job -Job $udpTrafficJob -Timeout 5)) { throw 'UDP traffic job did not converge' }
    Receive-Job -Job $udpTrafficJob -ErrorAction Stop | Out-Null
} finally {
    if ($null -ne $udpTrafficJob) {
        Stop-Job -Job $udpTrafficJob -ErrorAction SilentlyContinue
        Remove-Job -Job $udpTrafficJob -Force -ErrorAction SilentlyContinue
    }
    if (-not $udpTraffic.HasExited) { $udpTraffic.Kill($true) }
    $udpTraffic.Dispose()
    Remove-Item -LiteralPath $udpTrafficOutputPath -Force -ErrorAction SilentlyContinue
}

Write-Host 'PASS server TCP/UDP loopback and stop-under-load scenarios'
