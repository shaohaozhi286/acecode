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

$manifestPath = Join-Path $ExtensionDir "manifest.json"
$serviceWorkerPath = Join-Path $ExtensionDir "service_worker.js"
$contentPath = Join-Path $ExtensionDir "content\virtual-cursor.js"
$popupPath = Join-Path $ExtensionDir "popup.js"

foreach ($path in @($manifestPath, $serviceWorkerPath, $contentPath, $popupPath)) {
    if (!(Test-Path $path)) {
        throw "missing required extension file: $path"
    }
}

$manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
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

$serviceWorker = Get-Content -Raw $serviceWorkerPath
$content = Get-Content -Raw $contentPath

Assert-Contains $serviceWorker "const DEFAULT_PORT = 52007" "service worker must default to port 52007"
Assert-Contains $serviceWorker "protocol_version: PROTOCOL_VERSION" "service worker must send protocol_version in hello"
Assert-Contains $serviceWorker "aceBrowserBridgeSessions" "service worker must persist session tab registry"
Assert-Contains $serviceWorker "restoreSessionRegistry" "service worker must restore session registry on startup"
Assert-Contains $serviceWorker "args.newTab === true" "service worker must only create new tabs on explicit newTab or missing session tab"
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
Assert-Contains $serviceWorker "show_pointer_path" "service worker must route pointer debug visualization"
Assert-Contains $serviceWorker "fallback_reason" "service worker must report fallback reason"
Assert-Contains $content "ace-browser-bridge-operation-label" "content script must include the operation overlay label"
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
