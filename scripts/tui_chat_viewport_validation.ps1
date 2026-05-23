[CmdletBinding()]
param(
    [ValidateSet("PrintCommands", "CheckHost", "Preflight", "RunHost", "Report", "CleanupCheck")]
    [string]$Mode = "PrintCommands",

    [ValidateSet("windows-terminal", "classic-conhost", "other", "unspecified")]
    [string]$TerminalHost = "unspecified",

    [string]$ReportDir = "",
    [string]$WindowsTerminalReportDir = (Join-Path $env:TEMP "acecode-chat-wt"),
    [string]$ConHostReportDir = (Join-Path $env:TEMP "acecode-chat-conhost"),
    [string]$TasksPath = "openspec\changes\add-ftxui-chat-viewport\tasks.md",
    [string]$EvidencePath = "openspec\changes\add-ftxui-chat-viewport\manual-validation-evidence.md",
    [string]$SourcePath = "main.cpp",
    [string]$Exe = "build\Release\acecode.exe",
    [string]$Python = "python",
    [string]$BuildDir = "build",
    [string]$CtestConfig = "Debug",

    [switch]$DryRun,
    [switch]$NoRecordResults,
    [switch]$PrintOnly,
    [switch]$Cleanup,
    [switch]$ForceOsc52Copy,
    [switch]$UpdateTasks,
    [switch]$WriteEvidence
)

$ErrorActionPreference = "Stop"

function Quote-Arg {
    param([string]$Value)
    if ($Value -match '^[A-Za-z0-9_./:\\-]+$') {
        return $Value
    }
    return "'" + ($Value -replace "'", "''") + "'"
}

function Format-Command {
    param([string[]]$CommandArgs)
    return ($CommandArgs | ForEach-Object { Quote-Arg $_ }) -join " "
}

function Invoke-Checked {
    param([string]$Command, [string[]]$CommandArgs)
    if ($DryRun) {
        $formattedCommand = Format-Command -CommandArgs @($Command)
        $formattedArgs = Format-Command -CommandArgs $CommandArgs
        Write-Output ($formattedCommand + " " + $formattedArgs)
        return
    }
    & $Command @CommandArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Get-DetectedTerminalHost {
    if ($env:WT_SESSION) {
        return "windows-terminal"
    }
    if ($env:OS -eq "Windows_NT") {
        if ($env:ConEmuPID -or $env:TERM_PROGRAM -or $env:TERM) {
            return "other"
        }
        return "classic-conhost"
    }
    return "non-windows"
}

function Test-TerminalHostMatch {
    param([string]$RequestedHost, [string]$DetectedHost)
    if ($RequestedHost -eq "unspecified" -or $RequestedHost -eq "other") {
        return $true
    }
    if ($RequestedHost -eq "windows-terminal") {
        return $DetectedHost -eq "windows-terminal"
    }
    if ($RequestedHost -eq "classic-conhost") {
        return $DetectedHost -eq "classic-conhost"
    }
    return $false
}

function Assert-TerminalHostMatch {
    param([string]$RequestedHost)
    $detectedHost = Get-DetectedTerminalHost
    Write-Output ("Requested terminal host: " + $RequestedHost)
    Write-Output ("Detected terminal host: " + $detectedHost)
    Write-Output ("WT_SESSION present: " + [bool]$env:WT_SESSION)
    if (-not (Test-TerminalHostMatch -RequestedHost $RequestedHost -DetectedHost $detectedHost)) {
        throw ("requested terminal host '" + $RequestedHost + "' does not match detected host '" +
               $detectedHost + "'. Rerun from the requested terminal.")
    }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $RepoRoot
try {
    $defaultReportDir = if ($TerminalHost -eq "classic-conhost") {
        $ConHostReportDir
    } else {
        $WindowsTerminalReportDir
    }
    $hostReportDir = if ($ReportDir) { $ReportDir } else { $defaultReportDir }

    $runHostArgs = @(
        "scripts\tui_chat_viewport_manual.py",
        "--coverage-matrix",
        "--terminal-host", $TerminalHost,
        "--report-dir", $hostReportDir,
        "--exe", $Exe
    )
    if (-not $NoRecordResults) {
        $runHostArgs += "--record-results"
    }
    if ($PrintOnly) {
        $runHostArgs += "--print-only"
    }
    if ($Cleanup) {
        $runHostArgs += "--cleanup"
    }
    if ($ForceOsc52Copy) {
        $runHostArgs += "--force-osc52-copy"
    }

    $reportArgs = @(
        "scripts\tui_chat_viewport_report.py",
        "--strict",
        $WindowsTerminalReportDir,
        $ConHostReportDir
    )
    if ($UpdateTasks) {
        $reportArgs += @("--update-tasks", $TasksPath)
    }
    if ($WriteEvidence) {
        $reportArgs += @("--write-evidence", $EvidencePath)
    }

    $cleanupArgs = @(
        "scripts\tui_chat_viewport_cleanup_check.py",
        "--strict",
        "--tasks", $TasksPath,
        "--evidence", $EvidencePath,
        "--source", $SourcePath
    )

    $buildReleaseArgs = @(
        "--build", $BuildDir,
        "--target", "acecode",
        "--config", "Release"
    )
    $buildUnitTestsArgs = @(
        "--build", $BuildDir,
        "--target", "acecode_unit_tests",
        "--config", $CtestConfig
    )
    $pyCompileArgs = @(
        "-m", "py_compile",
        "scripts\tui_chat_viewport_manual.py",
        "scripts\tui_chat_viewport_smoke.py",
        "scripts\tui_chat_viewport_report.py",
        "scripts\tui_chat_viewport_cleanup_check.py",
        "tests\scripts\tui_chat_viewport_scripts_test.py"
    )
    $ctestArgs = @(
        "--test-dir", $BuildDir,
        "-C", $CtestConfig,
        "-R", "ChatViewport|tui_chat_viewport_scripts",
        "--output-on-failure"
    )
    $smokeArgs = @("scripts\tui_chat_viewport_smoke.py", "--mode", "both")
    $openspecArgs = @("validate", "add-ftxui-chat-viewport")
    $diffCheckArgs = @("diff", "--check")

    if ($Mode -eq "PrintCommands") {
        Write-Output "Run Windows Terminal host validation:"
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "CheckHost",
            "-TerminalHost", "windows-terminal"
        )))
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "RunHost",
            "-TerminalHost", "windows-terminal",
            "-ReportDir", $WindowsTerminalReportDir
        )))
        Write-Output "Run classic conhost validation from a classic console:"
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "CheckHost",
            "-TerminalHost", "classic-conhost"
        )))
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "RunHost",
            "-TerminalHost", "classic-conhost",
            "-ReportDir", $ConHostReportDir
        )))
        Write-Output "Aggregate reports and update OpenSpec manual tasks:"
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "Report",
            "-UpdateTasks",
            "-WriteEvidence"
        )))
        Write-Output "Run local preflight before or after manual validation:"
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "Preflight"
        )))
        Write-Output "After deleting legacy chat scroll code, verify cleanup:"
        Write-Output ("  " + (Format-Command -CommandArgs @(
            ".\scripts\tui_chat_viewport_validation.ps1",
            "-Mode", "CleanupCheck"
        )))
        exit 0
    }

    if ($Mode -eq "CheckHost") {
        Assert-TerminalHostMatch -RequestedHost $TerminalHost
        exit 0
    }

    if ($Mode -eq "Preflight") {
        Invoke-Checked "cmake" $buildReleaseArgs
        Invoke-Checked "cmake" $buildUnitTestsArgs
        Invoke-Checked $Python $pyCompileArgs
        Invoke-Checked "ctest" $ctestArgs
        Invoke-Checked $Python $smokeArgs
        Invoke-Checked "openspec" $openspecArgs
        Invoke-Checked "git" $diffCheckArgs
        exit 0
    }

    if ($Mode -eq "RunHost") {
        if ($TerminalHost -eq "unspecified") {
            throw "-TerminalHost must be windows-terminal or classic-conhost for RunHost"
        }
        if ($NoRecordResults -and -not $PrintOnly) {
            throw "-NoRecordResults is only allowed with -PrintOnly; real RunHost validation must record checklist results"
        }
        if (-not $PrintOnly) {
            Assert-TerminalHostMatch -RequestedHost $TerminalHost
        }
        New-Item -ItemType Directory -Force -Path $hostReportDir | Out-Null
        Write-Output ("Report directory: " + $hostReportDir)
        Invoke-Checked $Python $runHostArgs
        exit 0
    }

    if ($Mode -eq "Report") {
        Invoke-Checked $Python $reportArgs
        exit 0
    }

    if ($Mode -eq "CleanupCheck") {
        Invoke-Checked $Python $cleanupArgs
        exit 0
    }
}
finally {
    Pop-Location
}
