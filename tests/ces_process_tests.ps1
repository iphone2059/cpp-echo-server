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

$tcpPort = Get-FreeTcpPort
$tcpOutputPath = [System.IO.Path]::GetTempFileName()
$tcp = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $tcpPort, '/w', '2', '/q',
    '/threads', '2', '/cq', '1024', '/memory', '67108864', '/stats') -RedirectStandardOutput $tcpOutputPath `
    -PassThru -WindowStyle Hidden
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
    if ($tcpText -notmatch 'final protocol=tcp .*accepted=[1-9][0-9]* .*bytes=[1-9][0-9]* .*active=0') {
        throw "TCP final statistics are missing or incomplete: $tcpText"
    }
} finally {
    if (-not $tcp.HasExited) { $tcp.Kill($true) }
    $tcp.Dispose()
    Remove-Item -LiteralPath $tcpOutputPath -Force -ErrorAction SilentlyContinue
}

$timeoutPort = Get-FreeTcpPort
$timeoutServer = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $timeoutPort, '/t', '1',
    '/w', '3', '/q', '/threads', '1', '/cq', '128', '/memory', '16777216') -PassThru -WindowStyle Hidden
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
}

$stormPort = Get-FreeTcpPort
$stormServer = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'tcp', '/s', $stormPort, '/w', '2', '/q',
    '/threads', '4', '/cq', '2048', '/memory', '134217728') -PassThru -WindowStyle Hidden
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
}

$udpPort = Get-FreeUdpPort
$udpOutputPath = [System.IO.Path]::GetTempFileName()
$udp = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'udp', '/s', $udpPort, '/w', '2', '/q',
    '/k', '64', '/cq', '1024', '/memory', '67108864', '/stats') -RedirectStandardOutput $udpOutputPath `
    -PassThru -WindowStyle Hidden
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
    if ($udpText -notmatch 'final protocol=udp .*completions=[1-9][0-9]* .*receives=[1-9][0-9]* .*sends=3 .*bytes=65508 .*outstanding=0') {
        throw "UDP final statistics are missing or incomplete: $udpText"
    }
} finally {
    if (-not $udp.HasExited) { $udp.Kill($true) }
    $udp.Dispose()
    Remove-Item -LiteralPath $udpOutputPath -Force -ErrorAction SilentlyContinue
}

$udpTrafficPort = Get-FreeUdpPort
$udpTraffic = Start-Process -FilePath $ServerPath -ArgumentList @('/p', 'udp', '/s', $udpTrafficPort, '/w', '2',
    '/q', '/k', '64', '/cq', '1024', '/memory', '67108864') -PassThru -WindowStyle Hidden
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
}

Write-Host 'PASS server TCP/UDP loopback and stop-under-load scenarios'
