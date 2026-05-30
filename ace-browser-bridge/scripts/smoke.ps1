param(
    [string]$ExtensionDir = ""
)

$ErrorActionPreference = "Stop"

if ($ExtensionDir -eq "") {
    $ExtensionDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
} elseif (Test-Path $ExtensionDir) {
    $ExtensionDir = (Resolve-Path $ExtensionDir).Path
}

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Message
    )
    if ($Text.IndexOf($Needle, [System.StringComparison]::Ordinal) -lt 0) {
        throw $Message
    }
}

function Assert-NotContains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Message
    )
    if ($Text.IndexOf($Needle, [System.StringComparison]::Ordinal) -ge 0) {
        throw $Message
    }
}

$manifestPath = Join-Path $ExtensionDir "manifest.json"
$serviceWorkerPath = Join-Path $ExtensionDir "service_worker.js"
$contentPath = Join-Path $ExtensionDir "content\virtual-cursor.js"
$popupPath = Join-Path $ExtensionDir "popup.js"

foreach ($path in @($manifestPath, $serviceWorkerPath, $contentPath, $popupPath)) {
    if (!(Test-Path $path)) {
        throw "missing required extension file: $path"
    }
}

$manifest = Get-Content -Raw -Encoding UTF8 $manifestPath | ConvertFrom-Json
if ($manifest.manifest_version -ne 3) {
    throw "manifest_version must be 3"
}
if ($manifest.background.service_worker -ne "service_worker.js") {
    throw "manifest service worker must be service_worker.js"
}
foreach ($permission in @("debugger", "scripting", "storage", "tabGroups", "tabs")) {
    if ($manifest.permissions -notcontains $permission) {
        throw "manifest is missing permission: $permission"
    }
}
if ($manifest.host_permissions -notcontains "http://127.0.0.1:52007/*") {
    throw "manifest is missing daemon host permission"
}
if ($null -eq $manifest.content_scripts -or $manifest.content_scripts.Count -lt 1) {
    throw "manifest must declare the content script"
}

$serviceWorker = Get-Content -Raw -Encoding UTF8 $serviceWorkerPath
$content = Get-Content -Raw -Encoding UTF8 $contentPath

Assert-Contains $serviceWorker "const DEFAULT_PORT = 52007" "service worker must default to port 52007"
Assert-Contains $serviceWorker "protocol_version: PROTOCOL_VERSION" "service worker must send protocol_version in hello"
Assert-Contains $serviceWorker "aceBrowserBridgeSessions" "service worker must persist session tab registry"
Assert-Contains $serviceWorker "restoreSessionRegistry" "service worker must restore session registry on startup"
Assert-Contains $serviceWorker "args.newTab === true" "service worker must only create new tabs on explicit newTab or missing session tab"
Assert-Contains $serviceWorker "waitForTabComplete" "service worker must wait for page navigation before returning"
Assert-Contains $serviceWorker "navigation_timeout" "service worker must report navigation timeout"
Assert-Contains $serviceWorker "showOperationOverlay" "service worker must implement automatic operation overlay"
Assert-Contains $serviceWorker "withOperatingOverlay" "service worker must wrap page operations with automatic overlay"
Assert-Contains $serviceWorker "function shortSessionHash" "service worker must derive short group title hashes"
Assert-Contains $serviceWorker 'return `ACE-${shortSessionHash(session)}`' "service worker must use short ACE hash group titles"
Assert-Contains $serviceWorker "isLegacyDefaultGroupTitle" "service worker must migrate legacy default group titles"
Assert-Contains $serviceWorker "chrome.debugger.attach" "service worker must contain CDP attach path"
Assert-Contains $serviceWorker "Input.dispatchMouseEvent" "service worker must dispatch mouse events"
Assert-Contains $serviceWorker 'type: "mousePressed"' "service worker must dispatch mousePressed"
Assert-Contains $serviceWorker 'type: "mouseReleased"' "service worker must dispatch mouseReleased"
Assert-Contains $serviceWorker 'type: "mouseWheel"' "service worker must dispatch mouseWheel"
Assert-Contains $serviceWorker "Input.insertText" "service worker must dispatch text input"
Assert-Contains $serviceWorker "Input.dispatchKeyEvent" "service worker must dispatch key input"
Assert-Contains $serviceWorker "devtools: true" "service worker must advertise DevTools capability"
Assert-Contains $serviceWorker "raw_cdp: true" "service worker must advertise raw CDP capability"
Assert-Contains $serviceWorker "handleConsole" "service worker must implement console DevTools commands"
Assert-Contains $serviceWorker "handleDevtools" "service worker must route semantic DevTools commands"
Assert-Contains $serviceWorker "handleRawCdp" "service worker must route raw CDP commands"
Assert-Contains $serviceWorker "Tracing.start" "service worker must support performance tracing"
Assert-Contains $serviceWorker "HeapProfiler.takeHeapSnapshot" "service worker must support heap snapshots"
Assert-Contains $serviceWorker "Network.getResponseBody" "service worker must support network body detail"
Assert-Contains $serviceWorker "show_pointer_path" "service worker must route pointer debug visualization"
Assert-NotContains $serviceWorker "input_block: true" "service worker must not advertise a manual input block capability"
Assert-Contains $serviceWorker "content_ready" "service worker must accept content script wake messages"
Assert-Contains $serviceWorker "fallback_reason" "service worker must report fallback reason"
Assert-Contains $serviceWorker "/plugin/log" "service worker must forward plugin diagnostics to host"
Assert-Contains $serviceWorker "action_start" "service worker must log action start"
Assert-Contains $serviceWorker "action_finish" "service worker must log action finish"
Assert-Contains $serviceWorker "screenshot_finish" "service worker must log screenshot summary"
Assert-Contains $content "ace-browser-bridge-operation-label" "content script must include the operation overlay label"
Assert-Contains $content "content_ready" "content script must wake the service worker on normal pages"
$overlayText = [string]([char]0x8BF7) + [string]([char]0x6682) + [string]([char]0x65F6) + [string]([char]0x4E0D) + [string]([char]0x8981) + [string]([char]0x64CD) + [string]([char]0x4F5C)
Assert-Contains $content $overlayText "content script must make the operation overlay obvious"
Assert-Contains $content "show_pointer_path" "content script must implement pointer debug visualization"
Assert-Contains $content "cross_origin_iframe" "content script must return a cross-origin iframe boundary error"

$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -ne $node) {
    & $node.Source --check $serviceWorkerPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "node --check failed for service_worker.js"
    }
    & $node.Source --check $contentPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "node --check failed for virtual-cursor.js"
    }
    & $node.Source --check $popupPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "node --check failed for popup.js"
    }
}

Write-Host "ace-browser-bridge smoke passed"
