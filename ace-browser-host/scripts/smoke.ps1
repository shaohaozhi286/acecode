param(
    [string]$ExePath = "",
    [int]$Port = 52007
)

$ErrorActionPreference = "Stop"

if ($ExePath -eq "") {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\..\build\Debug\ace-browser-host.exe"),
        (Join-Path $PSScriptRoot "..\..\build\Release\ace-browser-host.exe"),
        (Join-Path $PSScriptRoot "..\..\build\RelWithDebInfo\ace-browser-host.exe"),
        (Join-Path $PSScriptRoot "..\..\build\MinSizeRel\ace-browser-host.exe"),
        (Join-Path $PSScriptRoot "..\..\build\ace-browser-host.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            $ExePath = (Resolve-Path $candidate).Path
            break
        }
    }
    if ($ExePath -eq "") {
        $ExePath = "ace-browser-host.exe"
    }
} elseif (Test-Path $ExePath) {
    $ExePath = (Resolve-Path $ExePath).Path
}

function Invoke-CliJson {
    param(
        [string[]]$Arguments,
        [string]$InputJson = $null
    )

    if ($null -eq $InputJson) {
        $output = & $ExePath @Arguments
    } else {
        $output = $InputJson | & $ExePath @Arguments
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed: $ExePath $($Arguments -join ' ')"
    }
    return ($output | ConvertFrom-Json)
}

$status = Invoke-CliJson -Arguments @("status", "--json", "--port", "$Port")
if ($true -ne $status.ok) {
    throw "status did not return ok envelope"
}
if ($status.data.port -ne $Port) {
    throw "status did not report expected port"
}

$command = Invoke-CliJson -Arguments @("command", "--json", "--port", "$Port") -InputJson '{"session":"smoke","action":"snapshot","args":{}}'
if ($false -ne $command.ok) {
    throw "command should fail while daemon is not running"
}
if ($command.error.code -ne "daemon_not_running") {
    throw "expected daemon_not_running, got $($command.error.code)"
}

$bad = Invoke-CliJson -Arguments @("command", "--json", "--port", "$Port") -InputJson '{bad-json'
if ($false -ne $bad.ok -or $bad.error.code -ne "invalid_request") {
    throw "invalid request was not rejected"
}

$daemon = Start-Process -FilePath $ExePath -ArgumentList @("serve", "--json", "--port", "$Port") -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
try {
    $running = Invoke-CliJson -Arguments @("status", "--json", "--port", "$Port")
    if ($true -ne $running.ok -or $true -ne $running.data.running) {
        throw "daemon status did not report running=true"
    }
    if ($false -ne $running.data.extension_connected) {
        throw "daemon should start without a connected browser plugin"
    }

    $blocked = Invoke-CliJson -Arguments @("command", "--json", "--port", "$Port") -InputJson '{"session":"smoke","action":"snapshot","args":{}}'
    if ($false -ne $blocked.ok -or $blocked.error.code -ne "extension_not_connected") {
        throw "daemon did not report extension_not_connected before plugin hello"
    }

    $pluginHeaders = @{ "X-Ace-Browser-Bridge" = "extension" }
    $mismatchBody = '{"protocol_version":"9.9","extension_version":"0.1-smoke","browser":"chromium","capabilities":{"cdp":true,"network":true,"pdf":true,"upload":true,"os_pointer":false,"operation_overlay":true}}'
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/hello" -Headers $pluginHeaders -ContentType "application/json" -Body $mismatchBody | Out-Null
    $mismatch = Invoke-CliJson -Arguments @("command", "--json", "--port", "$Port") -InputJson '{"session":"smoke","action":"snapshot","args":{}}'
    if ($false -ne $mismatch.ok -or $mismatch.error.code -ne "version_mismatch") {
        throw "daemon did not report version_mismatch for incompatible plugin protocol"
    }

    $helloBody = '{"protocol_version":"0.1","extension_version":"0.1-smoke","browser":"chromium","capabilities":{"cdp":true,"network":true,"pdf":true,"upload":true,"os_pointer":false,"operation_overlay":true}}'
    $hello = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/hello" -Headers $pluginHeaders -ContentType "application/json" -Body $helloBody
    if ($true -ne $hello.ok) {
        throw "plugin hello failed"
    }

    $connected = Invoke-CliJson -Arguments @("status", "--json", "--port", "$Port")
    if ($true -ne $connected.data.extension_connected -or $connected.data.extension_version -ne "0.1-smoke") {
        throw "status did not reflect plugin hello"
    }

    $commandRequest = @{ session = "smoke"; action = "snapshot"; args = @{} } | ConvertTo-Json -Depth 8 -Compress
    $commandJob = Start-Job -ScriptBlock {
        param($ExePath, $CommandRequest, $Port)
        $CommandRequest | & $ExePath command --json --port $Port
    } -ArgumentList $ExePath, $commandRequest, $Port
    try {
        Start-Sleep -Milliseconds 500
        $poll = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/poll" -Headers $pluginHeaders -ContentType "application/json" -Body "{}"
        if ($true -ne $poll.ok -or $null -eq $poll.data.action -or $poll.data.action.action -ne "snapshot") {
            throw "plugin poll did not return queued snapshot action"
        }
        $resultBody = @{
            id = $poll.data.action.id
            result = @{
                ok = $true
                data = @{
                    success = $true
                    path = "\tmp\ace-browser-bridge-pdfs\demo.txt"
                    mimeType = "text/plain"
                    data = "aGVsbG8="
                }
            }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $resultBody | Out-Null
        $commandOutput = Receive-Job $commandJob -Wait
        Remove-Job $commandJob -Force
        $commandJob = $null
        $commandResult = ($commandOutput | ConvertFrom-Json)
        if ($true -ne $commandResult.ok -or $true -ne $commandResult.data.success) {
            throw "command did not return successful envelope after plugin result"
        }
        if ($commandResult.data.path -notmatch "ace-browser-bridge-pdfs") {
            throw "command result path was not preserved"
        }
        if ($null -ne $commandResult.data.data) {
            throw "command result leaked base64 data"
        }
        if ($commandResult.data.sizeBytes -ne 5) {
            throw "command result did not report materialized size"
        }
        if (!(Test-Path $commandResult.data.path)) {
            throw "materialized bridge output file does not exist"
        }
    } finally {
        if ($null -ne $commandJob) {
            Remove-Job $commandJob -Force -ErrorAction SilentlyContinue
        }
    }
} finally {
    try {
        Invoke-CliJson -Arguments @("shutdown", "--json", "--port", "$Port") | Out-Null
    } catch {
    }
    if (!$daemon.HasExited) {
        Stop-Process -Id $daemon.Id -Force
    }
}

Write-Host "ace-browser-host smoke passed"
