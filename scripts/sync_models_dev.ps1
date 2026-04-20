# PowerShell sibling of scripts/sync_models_dev.sh. Pulls api.json from models.dev
# and writes it to assets/models_dev/ along with a MANIFEST.json that records
# fetch time + content sha256. No git clone / bun build required.

[CmdletBinding()]
param(
    [string]$Url = "https://models.dev/api.json"
)

$ErrorActionPreference = "Stop"
$ScriptVersion = 2

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Dest = Join-Path $RepoRoot "assets/models_dev"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

$tmp = [IO.Path]::GetTempFileName() + ".json"
Write-Host "Fetching $Url..."
Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing -TimeoutSec 30

$validation = python -c @"
import json, sys
with open(r'$tmp') as f: data = json.load(f)
if not isinstance(data, dict): sys.exit('top-level not an object')
providers = sum(1 for v in data.values() if isinstance(v, dict))
models = 0
for v in data.values():
    if not isinstance(v, dict): continue
    m = v.get('models')
    if isinstance(m, dict): models += len(m)
    elif isinstance(m, list): models += len(m)
if providers < 50: sys.exit(f'provider count too low: {providers}')
if models < 1000: sys.exit(f'model count too low: {models}')
print(providers, models)
"@
if ($LASTEXITCODE -ne 0) {
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    Write-Error "Validation failed: $validation"
}
$parts = $validation.Trim().Split()
$ProviderCount = [int]$parts[0]
$ModelCount = [int]$parts[1]

python -c @"
import json
with open(r'$tmp', encoding='utf-8') as f:
    data = json.load(f)
with open(r'$Dest\api.json', 'w', encoding='utf-8', newline='\n') as out:
    json.dump(data, out, separators=(',', ':'))
"@
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

$ContentSha = python -c "import hashlib; print(hashlib.sha256(open(r'$Dest\api.json','rb').read()).hexdigest())"
$ContentSha = $ContentSha.Trim()
$generatedAt = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")

$manifest = @{
    upstream_remote = $Url
    content_sha256 = $ContentSha
    generated_at = $generatedAt
    provider_count = $ProviderCount
    model_count = $ModelCount
    acecode_sync_script_version = $ScriptVersion
} | ConvertTo-Json -Depth 4
$manifest = $manifest -replace "`r`n", "`n"
[IO.File]::WriteAllText((Join-Path $Dest "MANIFEST.json"), $manifest + "`n", [Text.UTF8Encoding]::new($false))

Write-Host "Sync OK: providers=$ProviderCount models=$ModelCount sha256=$($ContentSha.Substring(0,12))..."
