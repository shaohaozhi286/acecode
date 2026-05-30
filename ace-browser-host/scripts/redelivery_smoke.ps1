# ace-browser-host 指令重投(at-least-once)集成测试。
#
# 背景:MV3 service worker 空闲约 30 秒就被浏览器回收。旧设计里 daemon 一旦把指令交给某次
# /plugin/poll 的响应就立刻出队,如果接走它的 worker 此刻被回收,指令既不在队列里(无法被下次
# poll 重新取到)、daemon 又在另一头干等满 30 秒超时 —— 这正是公司环境频繁 bridge_timeout 的根因。
#
# 本测试用裸 HTTP 扮演 service worker,精确复现两种关键场景:
#   场景 1:派发后插件"死掉"(不 ack、不回结果)→ daemon 应在重投窗口(~4s)后把指令重新入队,
#           下一次 poll 能再取到同一条指令,补 ack+结果后命令成功返回(而不是干等超时)。
#   场景 2:已 ack 的指令不应被重投 —— 避免把"还活着只是慢"的操作重复执行。

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
$capabilitiesJson = '"capabilities":{"cdp":true,"devtools":true,"raw_cdp":true,"console":true,"network":true,"emulation":true,"performance":true,"heap_snapshot":true,"pdf":true,"upload":true,"os_pointer":false,"operation_overlay":true,"input_block":true}'

function Invoke-Poll {
    return Invoke-RestMethod -Method Post -Uri "$base/plugin/poll" -Headers $pluginHeaders -ContentType "application/json" -Body "{}"
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

$daemon = Start-Process -FilePath $ExePath -ArgumentList @("serve", "--json", "--port", "$Port") -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
try {
    $helloBody = '{"protocol_version":"0.1","extension_version":"0.1-redeliver","browser":"chromium",' + $capabilitiesJson + '}'
    Invoke-RestMethod -Method Post -Uri "$base/plugin/hello" -Headers $pluginHeaders -ContentType "application/json" -Body $helloBody | Out-Null

    # ---- 场景 1:派发丢失 → 自动重投 ----
    $blockJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath block-input --json --session redeliver --watchdog-ms 60000 --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        Start-Sleep -Milliseconds 500
        # 第一次 poll:取到指令,然后"假装 worker 立刻被回收" —— 不 ack、不回结果。
        $poll1 = Invoke-Poll
        if ($true -ne $poll1.ok -or $null -eq $poll1.data.action -or $poll1.data.action.action -ne "block_input") {
            throw "first poll did not dispatch block_input: $($poll1 | ConvertTo-Json -Compress)"
        }
        $actionId = $poll1.data.action.id

        # 等过重投窗口(kRedeliveryAfterMs=4000)+ 余量。
        Start-Sleep -Seconds 6

        # 第二次 poll:应重新取到"同一条"指令(被 daemon 重投回队列)。
        # 这是关键回归点 —— 旧代码 pop_front 后不再重投,第二次 poll 只会拿到 null,指令永久丢失。
        $poll2 = Invoke-Poll
        if ($true -ne $poll2.ok -or $null -eq $poll2.data.action) {
            throw "redelivery failed: second poll returned no action (lost forever like the old bug)"
        }
        if ($poll2.data.action.id -ne $actionId) {
            throw "redelivery returned a different action id: $($poll2.data.action.id) vs $actionId"
        }

        # 这次正常确认 + 回结果,命令应当成功返回(而不是 bridge_timeout)。
        Post-Ack -Id $actionId
        Post-Result -Id $actionId -Data @{ success = $true; blocked = $true }

        $completed = Wait-Job -Job $blockJob -Timeout 15
        if ($null -eq $completed) {
            throw "block-input job did not finish after redelivery"
        }
        $blockResult = (Receive-Job $blockJob | ConvertFrom-Json)
        if ($true -ne $blockResult.ok -or $true -ne $blockResult.data.blocked) {
            throw "block-input did not succeed after redelivery: $($blockResult | ConvertTo-Json -Compress)"
        }
    } finally {
        Remove-Job $blockJob -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[redelivery] scenario 1 (dispatch lost -> auto redelivered -> success) passed"

    # ---- 场景 2:已 ack 的指令不重投 ----
    $ackJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath block-input --json --session redeliver-acked --watchdog-ms 60000 --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        Start-Sleep -Milliseconds 500
        $pollA = Invoke-Poll
        if ($true -ne $pollA.ok -or $null -eq $pollA.data.action) {
            throw "scenario 2 first poll did not dispatch action"
        }
        $ackedId = $pollA.data.action.id

        # 立刻确认收到 —— 这之后即便迟迟没回结果,也不应被重投(否则慢操作会被重复执行)。
        Post-Ack -Id $ackedId

        # 等过重投窗口。
        Start-Sleep -Seconds 6

        # 用 /status 断言:队列里没有被重投的指令,且命令仍在等待结果。
        $status = Get-Status
        if ($status.data.queued_actions -ne 0) {
            throw "acked action was wrongly redelivered: queued_actions=$($status.data.queued_actions)"
        }
        if ($status.data.pending_actions -ne 1) {
            throw "acked command should still be pending one result, got pending_actions=$($status.data.pending_actions)"
        }

        # 补回结果让命令收尾。
        Post-Result -Id $ackedId -Data @{ success = $true; blocked = $true }
        $completed = Wait-Job -Job $ackJob -Timeout 15
        if ($null -eq $completed) {
            throw "acked block-input job did not finish after result"
        }
        $ackResult = (Receive-Job $ackJob | ConvertFrom-Json)
        if ($true -ne $ackResult.ok -or $true -ne $ackResult.data.blocked) {
            throw "acked block-input did not succeed: $($ackResult | ConvertTo-Json -Compress)"
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
