param(
    [string]$ExePath = "",
    [int]$Port = 52117
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

function Wait-PluginAction {
    param(
        [int]$Port,
        [hashtable]$Headers,
        [string]$ExpectedAction,
        [System.Management.Automation.Job]$Job = $null
    )

    Start-Sleep -Milliseconds 500
    if ($null -ne $Job -and $Job.State -ne "Running") {
        $jobOutput = Receive-Job $Job
        throw "job completed before queued action ${ExpectedAction}: $jobOutput"
    }
    $poll = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/poll" -Headers $Headers -ContentType "application/json" -Body "{}"
    if ($true -eq $poll.ok -and $null -ne $poll.data.action) {
        if ($poll.data.action.action -ne $ExpectedAction) {
            throw "expected queued action $ExpectedAction, got $($poll.data.action.action)"
        }
        return $poll
    }
    if ($null -ne $Job -and $Job.State -ne "Running") {
        $jobOutput = Receive-Job $Job
        throw "job completed without queued action ${ExpectedAction}: $jobOutput"
    }
    throw "timed out waiting for queued action $ExpectedAction"
}

function Receive-CompletedJob {
    param(
        [System.Management.Automation.Job]$Job,
        [string]$Name
    )
    $completed = Wait-Job -Job $Job -Timeout 15
    if ($null -eq $completed) {
        throw "$Name job did not finish"
    }
    return (Receive-Job $Job)
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

$missingUrl = Invoke-CliJson -Arguments @("open", "--json", "--port", "$Port")
if ($false -ne $missingUrl.ok -or $missingUrl.error.code -ne "invalid_request") {
    throw "open without url was not rejected"
}

$openStopped = Invoke-CliJson -Arguments @("open", "--json", "--url", "https://example.com", "--port", "$Port")
if ($false -ne $openStopped.ok -or $openStopped.error.code -ne "daemon_not_running") {
    throw "open should fail while daemon is not running"
}

$ensureStarted = Invoke-CliJson -Arguments @("ensure-ready", "--json", "--no-launch-browser", "--timeout-ms", "1000", "--port", "$Port")
try {
    if ($true -ne $ensureStarted.ok -or $true -ne $ensureStarted.data.running -or $false -ne $ensureStarted.data.ready) {
        throw "ensure-ready should start the daemon and report ready=false without a plugin"
    }
    if ($true -ne $ensureStarted.data.host_start_attempted -or $true -eq $ensureStarted.data.browser_launch_attempted) {
        throw "ensure-ready did not report host start/no browser launch diagnostics"
    }
} finally {
    Invoke-CliJson -Arguments @("shutdown", "--json", "--port", "$Port") | Out-Null
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
    if ($false -ne $running.data.ready -or $false -ne $running.data.extension_stale) {
        throw "daemon should not report ready or stale before plugin hello"
    }

    $notReady = Invoke-CliJson -Arguments @("ensure-ready", "--json", "--no-launch-browser", "--timeout-ms", "100", "--port", "$Port")
    if ($true -ne $notReady.ok -or $true -ne $notReady.data.running -or $false -ne $notReady.data.ready) {
        throw "ensure-ready without plugin should report running but not ready"
    }
    if ($notReady.data.browser_launch_attempted -ne $false -or $notReady.data.ready_error -ne "readiness_timeout") {
        throw "ensure-ready did not preserve no-launch readiness diagnostics"
    }

    $blocked = Invoke-CliJson -Arguments @("command", "--json", "--port", "$Port") -InputJson '{"session":"smoke","action":"snapshot","args":{}}'
    if ($false -ne $blocked.ok -or $blocked.error.code -ne "extension_not_connected") {
        throw "daemon did not report extension_not_connected before plugin hello"
    }

    $pluginHeaders = @{ "X-Ace-Browser-Bridge" = "extension" }
    $capabilitiesJson = '"capabilities":{"cdp":true,"devtools":true,"raw_cdp":true,"console":true,"network":true,"emulation":true,"performance":true,"heap_snapshot":true,"pdf":true,"upload":true,"os_pointer":false,"operation_overlay":true,"input_block":true}'
    $mismatchBody = '{"protocol_version":"9.9","extension_version":"0.1-smoke","browser":"chromium",' + $capabilitiesJson + '}'
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/hello" -Headers $pluginHeaders -ContentType "application/json" -Body $mismatchBody | Out-Null
    $mismatch = Invoke-CliJson -Arguments @("command", "--json", "--port", "$Port") -InputJson '{"session":"smoke","action":"snapshot","args":{}}'
    if ($false -ne $mismatch.ok -or $mismatch.error.code -ne "version_mismatch") {
        throw "daemon did not report version_mismatch for incompatible plugin protocol"
    }

    $helloBody = '{"protocol_version":"0.1","extension_version":"0.1-smoke","browser":"chromium",' + $capabilitiesJson + '}'
    $hello = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/hello" -Headers $pluginHeaders -ContentType "application/json" -Body $helloBody
    if ($true -ne $hello.ok) {
        throw "plugin hello failed"
    }

    $connected = Invoke-CliJson -Arguments @("status", "--json", "--port", "$Port")
    if ($true -ne $connected.data.extension_connected -or $connected.data.extension_version -ne "0.1-smoke") {
        throw "status did not reflect plugin hello"
    }
    if ($true -ne $connected.data.ready -or $true -ne $connected.data.version_compatible) {
        throw "status did not report ready after compatible plugin hello"
    }
    if ($null -eq $connected.data.extension_last_seen_ms) {
        throw "status did not report extension freshness"
    }
    if ($null -eq $connected.data.queued_actions -or $null -eq $connected.data.pending_actions) {
        throw "status did not report action queue diagnostics"
    }
    foreach ($capability in @("devtools", "raw_cdp", "console", "emulation", "performance", "heap_snapshot")) {
        if ($true -ne $connected.data.capabilities.$capability) {
            throw "status did not report DevTools capability: $capability"
        }
    }

    $pluginLogBody = '{"level":"info","message":"smoke_log","data":{"id":"act_smoke","action":"snapshot","session":"smoke"}}'
    $pluginLog = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/log" -Headers $pluginHeaders -ContentType "application/json" -Body $pluginLogBody
    if ($true -ne $pluginLog.ok) {
        throw "plugin log endpoint failed"
    }

    $ready = Invoke-CliJson -Arguments @("ensure-ready", "--json", "--no-launch-browser", "--timeout-ms", "100", "--port", "$Port")
    if ($true -ne $ready.ok -or $true -ne $ready.data.ready -or $true -eq $ready.data.browser_launch_attempted) {
        throw "ensure-ready should report ready without launching browser when plugin is connected"
    }

    $assertJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath assert --json --session smoke --condition text_equals --target "#status" --text "Saved" --timeout-ms 5000 --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        $assertPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "assert" -Job $assertJob
        if ($assertPoll.data.action.args.condition -ne "text_equals" -or $assertPoll.data.action.args.target -ne "#status" -or $assertPoll.data.action.args.text -ne "Saved") {
            throw "assert CLI did not preserve condition args"
        }
        $assertResultBody = @{
            id = $assertPoll.data.action.id
            result = @{ ok = $true; data = @{ matched = "text_equals"; observed = "Saved" } }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $assertResultBody | Out-Null
        $assertOutput = Receive-CompletedJob -Job $assertJob -Name "assert"
        Remove-Job $assertJob -Force
        $assertJob = $null
        $assertResult = ($assertOutput | ConvertFrom-Json)
        if ($true -ne $assertResult.ok -or $assertResult.data.observed -ne "Saved") {
            throw "assert command did not return successful envelope"
        }
    } finally {
        if ($null -ne $assertJob) {
            Remove-Job $assertJob -Force -ErrorAction SilentlyContinue
        }
    }

    $batchSteps = '{"vars":{"status":"Saved"},"steps":[{"action":"assert","args":{"condition":"network_idle","timeout_ms":45000},"set":"idle"},{"action":"read_page","args":{"mode":"summary"},"when":{"condition":"text_present","text":"${status}","timeout_ms":250},"retry":{"attempts":2,"delay_ms":100}}],"finally":[{"action":"read_page","args":{"mode":"focused"}}]}'
    $batchJob = Start-Job -ScriptBlock {
        param($ExePath, $Port, $Steps)
        $Steps | & $ExePath batch --json --session smoke --port $Port
    } -ArgumentList $ExePath, $Port, $batchSteps
    try {
        $batchPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "batch" -Job $batchJob
        if ($batchPoll.data.action.args.steps.Count -ne 2 -or $batchPoll.data.action.args.steps[0].action -ne "assert") {
            throw "batch CLI did not preserve steps"
        }
        if ($batchPoll.data.action.args.vars.status -ne "Saved" -or $batchPoll.data.action.args.finally.Count -ne 1) {
            throw "batch CLI did not preserve v2 vars/finally"
        }
        if ($batchPoll.data.action.command_timeout_ms -ne 65100) {
            throw "batch CLI did not compute expected command_timeout_ms: $($batchPoll.data.action.command_timeout_ms)"
        }
        $batchResultBody = @{
            id = $batchPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    ran = 2
                    stopped_at = $null
                    finally_steps = @(@{ index = 0; action = "read_page"; ok = $true; data = @{ text = "Saved" } })
                    steps = @(
                        @{ index = 0; action = "assert"; ok = $true; data = @{ observed = @{ in_flight = 0 } } },
                        @{ index = 1; action = "read_page"; ok = $true; data = @{ text = "Saved" } }
                    )
                }
            }
        } | ConvertTo-Json -Depth 12 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $batchResultBody | Out-Null
        $batchOutput = Receive-CompletedJob -Job $batchJob -Name "batch"
        Remove-Job $batchJob -Force
        $batchJob = $null
        $batchResult = ($batchOutput | ConvertFrom-Json)
        if ($true -ne $batchResult.ok -or $batchResult.data.ran -ne 2) {
            throw "batch command did not return combined envelope"
        }
    } finally {
        if ($null -ne $batchJob) {
            Remove-Job $batchJob -Force -ErrorAction SilentlyContinue
        }
    }

    $screenshotPath = Join-Path ([System.IO.Path]::GetTempPath()) "ace-browser-smoke-element.png"
    Remove-Item -LiteralPath $screenshotPath -Force -ErrorAction SilentlyContinue
    $screenshotJob = Start-Job -ScriptBlock {
        param($ExePath, $Port, $Path)
        & $ExePath screenshot --json --session smoke --target "@e4" --output $Path --port $Port
    } -ArgumentList $ExePath, $Port, $screenshotPath
    try {
        $screenshotPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "screenshot" -Job $screenshotJob
        if ($screenshotPoll.data.action.args.selector -ne "@e4") {
            throw "screenshot CLI did not preserve target args: $($screenshotPoll.data.action.args | ConvertTo-Json -Depth 8 -Compress)"
        }
        $actualScreenshotPath = [System.IO.Path]::GetFullPath([string]$screenshotPoll.data.action.args.output)
        if ([System.IO.Path]::GetFileName($actualScreenshotPath) -ne "ace-browser-smoke-element.png") {
            throw "screenshot CLI did not preserve output filename: actual=$actualScreenshotPath"
        }
        $pngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
        $screenshotResultBody = @{
            id = $screenshotPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    path = $screenshotPath
                    mimeType = "image/png"
                    sizeBytes = 68
                    data = $pngBase64
                    crop = @{ applied = $true; rect = @{ x = 1; y = 2; width = 3; height = 4 } }
                }
            }
        } | ConvertTo-Json -Depth 12 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $screenshotResultBody | Out-Null
        $screenshotOutput = Receive-CompletedJob -Job $screenshotJob -Name "screenshot"
        Remove-Job $screenshotJob -Force
        $screenshotJob = $null
        $screenshotResult = ($screenshotOutput | ConvertFrom-Json)
        $materializedPath = [string]$screenshotResult.data.path
        if ($true -ne $screenshotResult.ok -or !(Test-Path $materializedPath) -or $null -ne $screenshotResult.data.data) {
            throw "screenshot command did not materialize file and strip base64"
        }
    } finally {
        if ($null -ne $screenshotJob) {
            Remove-Job $screenshotJob -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $screenshotPath -Force -ErrorAction SilentlyContinue
        if ($materializedPath) {
            Remove-Item -LiteralPath $materializedPath -Force -ErrorAction SilentlyContinue
        }
    }

    $devtoolsJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath devtools --json --session smoke --cmd console-list --types error,warn --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        $devtoolsPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "devtools" -Job $devtoolsJob
        if ($true -ne $devtoolsPoll.ok -or $devtoolsPoll.data.action.action -ne "devtools") {
            throw "plugin poll did not return queued devtools action"
        }
        if ($devtoolsPoll.data.action.args.cmd -ne "console-list" -or $devtoolsPoll.data.action.args.types[0] -ne "error") {
            throw "devtools CLI did not preserve console-list arguments"
        }
        $devtoolsResultBody = @{
            id = $devtoolsPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    count = 1
                    messages = @(@{ id = 1; type = "error"; text = "boom" })
                }
            }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $devtoolsResultBody | Out-Null
        $devtoolsOutput = Receive-CompletedJob -Job $devtoolsJob -Name "devtools"
        Remove-Job $devtoolsJob -Force
        $devtoolsJob = $null
        $devtoolsResult = ($devtoolsOutput | ConvertFrom-Json)
        if ($true -ne $devtoolsResult.ok -or $devtoolsResult.data.count -ne 1) {
            throw "devtools command did not return successful envelope"
        }
    } finally {
        if ($null -ne $devtoolsJob) {
            Remove-Job $devtoolsJob -Force -ErrorAction SilentlyContinue
        }
    }

    $cdpJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        $params = '{\"expression\":\"document.title\",\"returnByValue\":true}'
        & $ExePath cdp --json --session smoke --method Runtime.evaluate --params $params --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        $cdpPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "raw_cdp" -Job $cdpJob
        if ($true -ne $cdpPoll.ok -or $cdpPoll.data.action.action -ne "raw_cdp") {
            throw "plugin poll did not return queued raw_cdp action"
        }
        if ($cdpPoll.data.action.args.method -ne "Runtime.evaluate" -or $cdpPoll.data.action.args.params.expression -ne "document.title") {
            throw "cdp CLI did not preserve method/params"
        }
        $cdpResultBody = @{
            id = $cdpPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    success = $true
                    method = "Runtime.evaluate"
                    result = @{ result = @{ type = "string"; value = "Demo" } }
                }
            }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $cdpResultBody | Out-Null
        $cdpOutput = Receive-CompletedJob -Job $cdpJob -Name "cdp"
        Remove-Job $cdpJob -Force
        $cdpJob = $null
        $cdpResult = ($cdpOutput | ConvertFrom-Json)
        if ($true -ne $cdpResult.ok -or $cdpResult.data.method -ne "Runtime.evaluate") {
            throw "cdp command did not return successful envelope"
        }
    } finally {
        if ($null -ne $cdpJob) {
            Remove-Job $cdpJob -Force -ErrorAction SilentlyContinue
        }
    }

    $tracePath = Join-Path ([System.IO.Path]::GetTempPath()) ("ace-browser-host-trace-" + [guid]::NewGuid().ToString("N") + ".json")
    $traceJob = Start-Job -ScriptBlock {
        param($ExePath, $Port, $TracePath)
        & $ExePath devtools --json --session smoke --cmd performance-stop --output $TracePath --port $Port
    } -ArgumentList $ExePath, $Port, $tracePath
    try {
        $tracePoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "devtools" -Job $traceJob
        if ($true -ne $tracePoll.ok -or $tracePoll.data.action.action -ne "devtools" -or $tracePoll.data.action.args.cmd -ne "performance-stop") {
            throw "plugin poll did not return queued performance-stop action"
        }
        $traceResultBody = @{
            id = $tracePoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    success = $true
                    event_count = 1
                    trace = @{ traceEvents = @(@{ name = "Smoke"; ph = "X"; ts = 1 }) }
                }
            }
        } | ConvertTo-Json -Depth 12 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $traceResultBody | Out-Null
        $traceOutput = Receive-CompletedJob -Job $traceJob -Name "performance-stop"
        Remove-Job $traceJob -Force
        $traceJob = $null
        $traceResult = ($traceOutput | ConvertFrom-Json)
        if ($true -ne $traceResult.ok -or !(Test-Path $tracePath) -or $null -ne $traceResult.data.trace) {
            throw "performance-stop did not materialize trace output"
        }
    } finally {
        if ($null -ne $traceJob) {
            Remove-Job $traceJob -Force -ErrorAction SilentlyContinue
        }
        Remove-Item $tracePath -Force -ErrorAction SilentlyContinue
    }

    $blockJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath block-input --json --session smoke --watchdog-ms 123456 --message "AI busy" --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        $blockPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "block_input" -Job $blockJob
        if ($true -ne $blockPoll.ok -or $blockPoll.data.action.action -ne "block_input") {
            throw "plugin poll did not return queued block_input action"
        }
        if ($blockPoll.data.action.args.watchdog_ms -ne 123456 -or $blockPoll.data.action.args.message -ne "AI busy") {
            throw "block-input CLI did not preserve arguments"
        }
        $blockResultBody = @{
            id = $blockPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    success = $true
                    blocked = $true
                    pending = $true
                }
            }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $blockResultBody | Out-Null
        $blockOutput = Receive-CompletedJob -Job $blockJob -Name "block-input"
        Remove-Job $blockJob -Force
        $blockJob = $null
        $blockResult = ($blockOutput | ConvertFrom-Json)
        if ($true -ne $blockResult.ok -or $true -ne $blockResult.data.blocked) {
            throw "block-input command did not return successful envelope"
        }
    } finally {
        if ($null -ne $blockJob) {
            Remove-Job $blockJob -Force -ErrorAction SilentlyContinue
        }
    }

    $unblockJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath unblock-input --json --session smoke --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        $unblockPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "unblock_input" -Job $unblockJob
        if ($true -ne $unblockPoll.ok -or $unblockPoll.data.action.action -ne "unblock_input") {
            throw "plugin poll did not return queued unblock_input action"
        }
        $unblockResultBody = @{
            id = $unblockPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    success = $true
                    blocked = $false
                }
            }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $unblockResultBody | Out-Null
        $unblockOutput = Receive-CompletedJob -Job $unblockJob -Name "unblock-input"
        Remove-Job $unblockJob -Force
        $unblockJob = $null
        $unblockResult = ($unblockOutput | ConvertFrom-Json)
        if ($true -ne $unblockResult.ok -or $false -ne $unblockResult.data.blocked) {
            throw "unblock-input command did not return successful envelope"
        }
    } finally {
        if ($null -ne $unblockJob) {
            Remove-Job $unblockJob -Force -ErrorAction SilentlyContinue
        }
    }

    $unicodeValue = [string]([char]0x5F20) + [string]([char]0x4E09)
    $fillJob = Start-Job -ScriptBlock {
        param($ExePath, $Port, $Value)
        & $ExePath fill --json --session smoke --target "@e1" --value $Value --port $Port
    } -ArgumentList $ExePath, $Port, $unicodeValue
    try {
        $fillPoll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "fill" -Job $fillJob
        if ($true -ne $fillPoll.ok -or $fillPoll.data.action.action -ne "fill") {
            throw "plugin poll did not return queued fill action"
        }
        if ($fillPoll.data.action.args.value -ne $unicodeValue) {
            throw "fill CLI did not preserve UTF-8 argument value"
        }
        $fillResultBody = @{
            id = $fillPoll.data.action.id
            result = @{
                ok = $true
                data = @{
                    success = $true
                    tag = "INPUT"
                    mode = "value"
                }
            }
        } | ConvertTo-Json -Depth 8 -Compress
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/result" -Headers $pluginHeaders -ContentType "application/json" -Body $fillResultBody | Out-Null
        $fillOutput = Receive-CompletedJob -Job $fillJob -Name "fill"
        Remove-Job $fillJob -Force
        $fillJob = $null
        $fillResult = ($fillOutput | ConvertFrom-Json)
        if ($true -ne $fillResult.ok -or $true -ne $fillResult.data.success) {
            throw "fill command did not return successful envelope"
        }
    } finally {
        if ($null -ne $fillJob) {
            Remove-Job $fillJob -Force -ErrorAction SilentlyContinue
        }
    }

    $commandJob = Start-Job -ScriptBlock {
        param($ExePath, $Port)
        & $ExePath read-page --json --session smoke --port $Port
    } -ArgumentList $ExePath, $Port
    try {
        $poll = Wait-PluginAction -Port $Port -Headers $pluginHeaders -ExpectedAction "snapshot" -Job $commandJob
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
        $commandOutput = Receive-CompletedJob -Job $commandJob -Name "read-page"
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

    $pollJob = Start-Job -ScriptBlock {
        param($Port)
        Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/plugin/poll" -Headers @{ "X-Ace-Browser-Bridge" = "extension" } -ContentType "application/json" -Body "{}"
    } -ArgumentList $Port
    try {
        Start-Sleep -Milliseconds 500
        Invoke-CliJson -Arguments @("shutdown", "--json", "--port", "$Port") | Out-Null
        $pollAfterShutdown = Receive-CompletedJob -Job $pollJob -Name "poll-after-shutdown"
        if ($true -ne $pollAfterShutdown.ok -or $null -ne $pollAfterShutdown.data.action) {
            throw "plugin poll did not unblock cleanly during daemon shutdown"
        }
        Remove-Job $pollJob -Force
        $pollJob = $null
    } finally {
        if ($null -ne $pollJob) {
            Remove-Job $pollJob -Force -ErrorAction SilentlyContinue
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
