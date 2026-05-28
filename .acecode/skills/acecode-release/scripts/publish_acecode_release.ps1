param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$')]
    [string]$Version,

    [string]$Repo = (Get-Location).Path,
    [string]$UpdateDir = 'J:\jenkins_green\aupdate',
    [string]$RemoteBaseUrl = 'http://2017studio.imwork.net:82/aupdate/',
    [string]$Configuration = 'Release',
    [string]$Target = 'windows-x64',
    [string[]]$StageFiles = @(),
    [string]$CommitMessage = '',

    [switch]$NoCommit,
    [switch]$NoTag,
    [switch]$Push,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$NoPublish,
    [switch]$AllowDirtyBuild
)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Normalize-RepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (($Path -replace '\\', '/').TrimStart('./'))
}

function Get-DirtyPaths {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $lines = & git -C $RepoRoot status --porcelain
    $paths = @()
    foreach ($line in $lines) {
        if ($line.Length -lt 4) { continue }
        $path = $line.Substring(3)
        if ($path -match ' -> ') {
            $path = ($path -split ' -> ')[-1]
        }
        $path = $path.Trim('"')
        if ($path) {
            $paths += (Normalize-RepoPath $path)
        }
    }
    return $paths
}

function Set-AcecodeVersion {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$NewVersion
    )

    $cmakePath = Join-Path $RepoRoot 'CMakeLists.txt'
    $cmake = [System.IO.File]::ReadAllText($cmakePath)
    $updatedCmake = [regex]::Replace(
        $cmake,
        'project\(acecode VERSION [0-9A-Za-z.\-]+ LANGUAGES C CXX\)',
        "project(acecode VERSION $NewVersion LANGUAGES C CXX)",
        1)
    if ($updatedCmake -eq $cmake -and $cmake -notmatch "project\(acecode VERSION $([regex]::Escape($NewVersion)) LANGUAGES C CXX\)") {
        throw 'Could not update CMakeLists.txt project version.'
    }
    Write-Utf8NoBom $cmakePath $updatedCmake

    $vcpkgPath = Join-Path $RepoRoot 'vcpkg.json'
    if (Test-Path -LiteralPath $vcpkgPath) {
        $vcpkg = Get-Content -LiteralPath $vcpkgPath -Raw | ConvertFrom-Json
        $vcpkg.'version-semver' = $NewVersion
        $json = $vcpkg | ConvertTo-Json -Depth 16
        Write-Utf8NoBom $vcpkgPath ($json + [Environment]::NewLine)
    }
}

function Ensure-ZipMimeConfig {
    param([Parameter(Mandatory = $true)][string]$Directory)
    $webConfig = Join-Path $Directory 'web.config'
    if (Test-Path -LiteralPath $webConfig) {
        $text = [System.IO.File]::ReadAllText($webConfig)
        if ($text -match 'mimeType="application/zip"') {
            return
        }
    }

    $xml = @'
<?xml version="1.0" encoding="UTF-8"?>
<configuration>
    <system.webServer>
        <staticContent>
            <remove fileExtension=".zip" />
            <mimeMap fileExtension=".zip" mimeType="application/zip" />
        </staticContent>
    </system.webServer>
</configuration>
'@
    Write-Utf8NoBom $webConfig ($xml + [Environment]::NewLine)
}

function Update-Manifest {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$NewVersion,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string]$Sha256,
        [Parameter(Mandatory = $true)][UInt64]$Size
    )

    $oldReleases = @()
    if (Test-Path -LiteralPath $ManifestPath) {
        $oldManifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
        if ($oldManifest.releases) {
            foreach ($release in $oldManifest.releases) {
                if ($release.version -ne $NewVersion) {
                    $oldReleases += $release
                }
            }
        }
    }

    $newRelease = [ordered]@{
        version = $NewVersion
        published_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        notes = "ACECode v$NewVersion self-upgrade package."
        packages = @(
            [ordered]@{
                target = $TargetName
                file = $FileName
                sha256 = $Sha256
                size = $Size
            }
        )
    }

    $manifest = [ordered]@{
        schema_version = 1
        latest = $NewVersion
        releases = @($newRelease) + $oldReleases
    }
    Write-Utf8NoBom $ManifestPath (($manifest | ConvertTo-Json -Depth 16) + [Environment]::NewLine)
}

function Verify-HttpPackage {
    param(
        [Parameter(Mandatory = $true)][string]$BaseUrl,
        [Parameter(Mandatory = $true)][string]$VersionToVerify,
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)][UInt64]$ExpectedSize,
        [Parameter(Mandatory = $true)][string]$ExpectedSha
    )

    $base = $BaseUrl.TrimEnd('/') + '/'
    $manifestResponse = Invoke-WebRequest -Uri ($base + 'aceupdate.json') -UseBasicParsing -TimeoutSec 15
    $manifest = $manifestResponse.Content | ConvertFrom-Json
    if ($manifest.latest -ne $VersionToVerify) {
        throw "HTTP manifest latest is '$($manifest.latest)', expected '$VersionToVerify'."
    }

    $tmp = Join-Path $env:TEMP ("acecode-release-verify-" + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Invoke-WebRequest -Uri ($base + $PackageName) -UseBasicParsing -TimeoutSec 60 -Headers @{ Accept = 'application/zip' } -OutFile $tmp
        $actualSize = [UInt64](Get-Item -LiteralPath $tmp).Length
        $actualSha = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualSize -ne $ExpectedSize) {
            throw "HTTP package size mismatch: got $actualSize, expected $ExpectedSize."
        }
        if ($actualSha -ne $ExpectedSha) {
            throw "HTTP package sha256 mismatch: got $actualSha, expected $ExpectedSha."
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

function New-ZipFromDirectoryWithForwardSlashes {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
    $destDir = Split-Path -Parent $DestinationPath
    if ($destDir) {
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    }
    if (Test-Path -LiteralPath $DestinationPath) {
        Remove-Item -LiteralPath $DestinationPath -Force
    }

    $archive = [System.IO.Compression.ZipFile]::Open(
        $DestinationPath,
        [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $files = Get-ChildItem -LiteralPath $sourceRoot -File -Recurse
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\', '/')
            $entryName = $relative -replace '\\', '/'
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive,
                $file.FullName,
                $entryName,
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
}

$Repo = (Resolve-Path -LiteralPath $Repo).Path
if (-not (Test-Path -LiteralPath (Join-Path $Repo 'CMakeLists.txt'))) {
    throw "Not an ACECode repo root: $Repo"
}

$tag = "v$Version"
if (-not $CommitMessage) {
    $CommitMessage = "Release $tag"
}

Write-Host "== ACECode release $Version =="
Write-Host "Repo:      $Repo"
Write-Host "UpdateDir: $UpdateDir"

Set-AcecodeVersion -RepoRoot $Repo -NewVersion $Version

$defaultStage = @('CMakeLists.txt', 'vcpkg.json')
$stageSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
foreach ($path in ($defaultStage + $StageFiles)) {
    if (-not [string]::IsNullOrWhiteSpace($path)) {
        [void]$stageSet.Add((Normalize-RepoPath $path))
    }
}

$dirty = Get-DirtyPaths -RepoRoot $Repo
$outside = @()
foreach ($path in $dirty) {
    if (-not $stageSet.Contains($path)) {
        $outside += $path
    }
}
if ($outside.Count -gt 0 -and -not $AllowDirtyBuild) {
    Write-Host "Dirty files outside release file set:"
    $outside | ForEach-Object { Write-Host "  $_" }
    throw 'Refusing to build/package a release from unrelated dirty files. Add intended files with -StageFiles, clean the tree, or pass -AllowDirtyBuild.'
}

if (-not $SkipBuild) {
    Invoke-Native cmake --build (Join-Path $Repo 'build') --config $Configuration --target acecode acecode-desktop acecode_unit_tests
}

if (-not $SkipTests) {
    $testExe = Join-Path $Repo "build\tests\$Configuration\acecode_unit_tests.exe"
    Invoke-Native $testExe '--gtest_filter=Upgrade*:*ConfigUpgrade*'
}

$exe = Join-Path $Repo "build\$Configuration\acecode.exe"
$desktopExe = Join-Path $Repo "build\$Configuration\acecode-desktop.exe"
$versionOutput = (& $exe --version).Trim()
if ($versionOutput -ne "acecode v$Version") {
    throw "Built executable reports '$versionOutput', expected 'acecode v$Version'."
}
if (-not (Test-Path -LiteralPath $desktopExe)) {
    throw "Desktop executable missing: $desktopExe. Configure the build with -DACECODE_BUILD_DESKTOP=ON before release packaging."
}

if (-not $NoCommit) {
    $filesToAdd = @()
    foreach ($path in $stageSet) {
        $candidate = Join-Path $Repo ($path -replace '/', '\')
        if (Test-Path -LiteralPath $candidate) {
            $filesToAdd += $path
        }
    }
    if ($filesToAdd.Count -gt 0) {
        Invoke-Native -FilePath git -Arguments (@('-C', $Repo, 'add', '--') + $filesToAdd)
    }
    & git -C $Repo diff --cached --quiet
    if ($LASTEXITCODE -eq 1) {
        Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'commit', '-m', $CommitMessage)
    } elseif ($LASTEXITCODE -ne 0) {
        throw 'git diff --cached failed.'
    } else {
        Write-Host 'No staged changes to commit; using current HEAD.'
    }
}

if (-not $NoTag) {
    & git -C $Repo rev-parse -q --verify "refs/tags/$tag" *> $null
    if ($LASTEXITCODE -eq 0) {
        throw "Tag already exists: $tag"
    }
    Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'tag', '-a', $tag, '-m', "ACECode $tag")
}

if ($Push) {
    Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'push', 'origin', 'HEAD')
    if (-not $NoTag) {
        Invoke-Native -FilePath git -Arguments @('-C', $Repo, 'push', 'origin', $tag)
    }
}

if (-not $NoPublish) {
    $pkgName = "acecode-$Version-$Target"
    $packageRoot = Join-Path $Repo 'build\package'
    $stage = Join-Path $packageRoot $pkgName
    $zipPath = Join-Path $UpdateDir "$pkgName.zip"
    $manifestPath = Join-Path $UpdateDir 'aceupdate.json'

    New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
    $packageRootResolved = (Resolve-Path -LiteralPath $packageRoot).Path
    if (Test-Path -LiteralPath $stage) {
        $stageResolved = (Resolve-Path -LiteralPath $stage).Path
        if (-not $stageResolved.StartsWith($packageRootResolved, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove staging path outside package root: $stageResolved"
        }
        Remove-Item -LiteralPath $stageResolved -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'share\acecode') | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'acecode.exe') -Force
    Copy-Item -LiteralPath $desktopExe -Destination (Join-Path $stage 'acecode-desktop.exe') -Force
    Copy-Item -LiteralPath (Join-Path $Repo 'assets\models_dev') -Destination (Join-Path $stage 'share\acecode\models_dev') -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $Repo 'assets\seed') -Destination (Join-Path $stage 'share\acecode\seed') -Recurse -Force

    New-Item -ItemType Directory -Force -Path $UpdateDir | Out-Null
    Ensure-ZipMimeConfig -Directory $UpdateDir
    New-ZipFromDirectoryWithForwardSlashes -SourceDirectory $stage -DestinationPath $zipPath

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = $archive.Entries | ForEach-Object { $_.FullName }
        foreach ($entry in $entries) {
            if ($entry -match '\\') {
                throw "Package entry must use forward slashes: $entry"
            }
        }
        foreach ($required in @('acecode.exe', 'acecode-desktop.exe', 'share/acecode/models_dev/api.json', 'share/acecode/seed/MANIFEST.json')) {
            if ($entries -notcontains $required) {
                throw "Package missing required entry: $required"
            }
        }
    } finally {
        $archive.Dispose()
    }

    $size = [UInt64](Get-Item -LiteralPath $zipPath).Length
    $sha = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Update-Manifest -ManifestPath $manifestPath -NewVersion $Version -TargetName $Target -FileName "$pkgName.zip" -Sha256 $sha -Size $size

    if ($RemoteBaseUrl) {
        Verify-HttpPackage -BaseUrl $RemoteBaseUrl -VersionToVerify $Version -PackageName "$pkgName.zip" -ExpectedSize $size -ExpectedSha $sha
    }

    Write-Host "Package: $zipPath"
    Write-Host "Size:    $size"
    Write-Host "SHA256:  $sha"
}

$head = (& git -C $Repo rev-parse --short HEAD).Trim()
Write-Host "Release complete: $tag at $head"
