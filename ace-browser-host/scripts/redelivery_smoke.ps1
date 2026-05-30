# ace-browser-host at-least-once redelivery integration smoke.
#
# MV3 service workers can be reclaimed while a /plugin/poll response is in
# flight. This test uses raw HTTP as a fake service worker and covers:
#   Scenario 1: dispatched action is not acked or completed, so the daemon
#               redelivers the same id after the redelivery window.
#   Scenario 2: acked actions are not redelivered while still waiting for the
#               final result.

param(
    [string]$ExePath = "",
    [int]$Port = 52141
)

$ErrorActionPreference = "Stop"

if ($ExePath -eq "") {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\..\build\Release\ace-browser-host.exe"),
        (Join-Path $PSScriptRoot "..\..\build\Debug\ace-browser-host.exe"),
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

$base = "http://127.0.0.1:$Port"
$pluginHeaders = @{ "X-Ace-Browser-Bridge" = "extension" }
$hostHeaders = @{ "X-Ace-Browser-Host" = "1" }
$capabilitiesJson = '"capabilities":{"cdp":true,"devtools":true,"raw_cdp":true,"console":true,"network":true,"emulation":true,"performance":true,"heap_snapshot":true,"pdf":true,"upload":true,"os_pointer":false,"operation_overlay":true}'

function Invoke-BridgePoll {
    $client = New-Object System.Net.WebClient
    $client.Headers.Add("X-Ace-Browser-Bridge", "extension")
    $client.Headers.Add("Content-Type", "application/json")
    try {
        $json = $client.UploadString("$base/plugin/poll", "POST", "{}")
        if ([string]::IsNullOrWhiteSpace($json)) {
            throw "plugin poll returned an empty response"
        }
        return ($json | ConvertFrom-Json)
    } finally {
        $client.Dispose()
    }
}

function Post-Ack {
    param([string]$Id)
    $body = @{ source = "ace-browser-bridge"; id = $Id } | ConvertTo-Json -Compress
    Invoke-RestMethod -Method Post -Uri "$base/plugin/ack" -Headers $pluginHeaders -ContentType "application/json" -Body $body | Out-Null
}

function Post-Result {
    param([string]$Id, $Data)
    $body = @{ id = $Id; result = @{ ok = $true; data = $Data } } | ConvertTo-Json -Depth 8 -Compress
    Invoke-RestMethod -Method Post -Uri "$base/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $body | Out-Null
}

function Get-Status {
    return Invoke-RestMethod -Method Get -Uri "$base/status" -Headers $hostHeaders
}

function Get-ActionField {
    param($Poll, [string]$Name)
    if ($null -eq $Poll -or $null -eq $Poll.data -or $null -eq $Poll.data.action) {
        return $null
    }
    return $Poll.data.action.PSObject.Properties[$Name].Value
}

function Wait-QueuedAction {
    param(
        [System.Management.Automation.Job]$Job,
        [string]$Name,
        [int]$TimeoutMs = 10000
    )
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ($Job.State -ne "Running") {
            $jobOutput = Receive-Job $Job
            throw "$Name job completed before queuing action: $jobOutput"
        }
        $status = Get-Status
        if ($status.data.queued_actions -gt 0) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "$Name command was not queued within $TimeoutMs ms"
}

$daemon = Start-Process -FilePath $ExePath -ArgumentList @("serve", "--json", "--port", "$Port") -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
try {
    $helloBody = '{"protocol_version":"0.1","extension_version":"0.1-redeliver","browser":"chromium",' + $capabilitiesJson + '}'
    Invoke-RestMethod -Method Post -Uri "$base/plugin/hello" -Headers $pluginHeaders -ContentType "application/json" -Body $helloBody | Out-Null

    # ---- Scenario 1: dispatch lost -> redelivered ----
    $readJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath read-page --json --session redeliver --mode summary --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        Wait-QueuedAction -Job $readJob -Name "read-page"
        Start-Sleep -Milliseconds 500
        $poll1 = Invoke-BridgePoll
        # Do not ack or complete this dispatch; simulate a reclaimed worker.
        $poll1Action = Get-ActionField -Poll $poll1 -Name "action"
        if ($true -ne $poll1.ok -or $null -eq $poll1.data.action -or $poll1Action -ne "snapshot") {
            throw "first poll did not dispatch snapshot: action=$poll1Action json=$($poll1 | ConvertTo-Json -Depth 20 -Compress)"
        }
        $actionId = Get-ActionField -Poll $poll1 -Name "id"

        # Wait past kRedeliveryAfterMs=4000 plus margin.
        Start-Sleep -Seconds 6

        $poll2 = Invoke-BridgePoll
        # The second poll must receive the same id again; the old bug returned null.
        if ($true -ne $poll2.ok -or $null -eq $poll2.data.action) {
            throw "redelivery failed: second poll returned no action (lost forever like the old bug)"
        }
        $poll2Id = Get-ActionField -Poll $poll2 -Name "id"
        if ($poll2Id -ne $actionId) {
            throw "redelivery returned a different action id: $poll2Id vs $actionId"
        }

        # Ack and complete the redelivered action.
        Post-Ack -Id $actionId
        Post-Result -Id $actionId -Data @{ success = $true; title = "redelivered"; elements = @() }

        $completed = Wait-Job -Job $readJob -Timeout 15
        if ($null -eq $completed) {
            throw "read-page job did not finish after redelivery"
        }
        $readResult = (Receive-Job $readJob | ConvertFrom-Json)
        if ($true -ne $readResult.ok -or $true -ne $readResult.data.success) {
            throw "read-page did not succeed after redelivery: $($readResult | ConvertTo-Json -Compress)"
        }
    } finally {
        Remove-Job $readJob -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[redelivery] scenario 1 (dispatch lost -> auto redelivered -> success) passed"

    # ---- Scenario 2: acked actions are not redelivered ----
    $ackJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath read-page --json --session redeliver-acked --mode summary --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        Wait-QueuedAction -Job $ackJob -Name "acked read-page"
        Start-Sleep -Milliseconds 500
        $pollA = Invoke-BridgePoll
        if ($true -ne $pollA.ok -or $null -eq $pollA.data.action) {
            throw "scenario 2 first poll did not dispatch action"
        }
        $ackedId = Get-ActionField -Poll $pollA -Name "id"

        Post-Ack -Id $ackedId
        # Acked slow actions must not be redelivered.

        # Wait past the redelivery window.
        Start-Sleep -Seconds 6

        # Status should show no queued redelivery while the result is still pending.
        $status = Get-Status
        if ($status.data.queued_actions -ne 0) {
            throw "acked action was wrongly redelivered: queued_actions=$($status.data.queued_actions)"
        }
        if ($status.data.pending_actions -ne 1) {
            throw "acked command should still be pending one result, got pending_actions=$($status.data.pending_actions)"
        }

        # Complete the pending command.
        Post-Result -Id $ackedId -Data @{ success = $true; title = "acked"; elements = @() }
        $completed = Wait-Job -Job $ackJob -Timeout 15
        if ($null -eq $completed) {
            throw "acked read-page job did not finish after result"
        }
        $ackResult = (Receive-Job $ackJob | ConvertFrom-Json)
        if ($true -ne $ackResult.ok -or $true -ne $ackResult.data.success) {
            throw "acked read-page did not succeed: $($ackResult | ConvertTo-Json -Compress)"
        }
    } finally {
        Remove-Job $ackJob -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[redelivery] scenario 2 (acked action is not redelivered) passed"
} finally {
    try {
        Invoke-RestMethod -Method Post -Uri "$base/shutdown" -Headers $hostHeaders -ContentType "application/json" -Body "{}" | Out-Null
    } catch {
    }
    if (!$daemon.HasExited) {
        Start-Sleep -Milliseconds 300
        if (!$daemon.HasExited) {
            Stop-Process -Id $daemon.Id -Force
        }
    }
}

Write-Host "ace-browser-host redelivery smoke passed"
