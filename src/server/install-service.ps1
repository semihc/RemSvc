#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install, update, or remove the RemSvc gRPC server as a Windows service
    using NSSM (Non-Sucking Service Manager).

.DESCRIPTION
    Automates the NSSM commands needed to register RemSvc_server.exe as a
    Windows service that starts automatically on boot.  Run from an elevated
    (Administrator) PowerShell prompt.

    NSSM must be on PATH or placed alongside this script.  Download from
    https://nssm.cc/download (64-bit build).

.PARAMETER Action
    install   — Register and start the service (default).
    remove    — Stop and unregister the service.
    restart   — Stop then start the service (e.g. after a config change).

.PARAMETER InstallDir
    Directory where RemSvc_server.exe was extracted.
    Default: the directory containing this script.

.PARAMETER ConfigFile
    Path to the INI configuration file passed to the server via --config.
    Default: <InstallDir>\..\config\remsvc.ini  (adjust as needed).

.PARAMETER ServiceName
    Windows service name.  Default: RemSvc.

.PARAMETER LogDir
    Directory for NSSM stdout/stderr capture logs.
    Default: C:\ProgramData\RemSvc\logs

.PARAMETER Port
    Firewall rule port to open/close.  Default: 50051.

.EXAMPLE
    # Install with defaults (run from the bin\ directory of the extracted ZIP):
    .\install-service.ps1

.EXAMPLE
    # Install pointing at an explicit config file:
    .\install-service.ps1 -ConfigFile C:\ProgramData\RemSvc\remsvc.ini

.EXAMPLE
    # Remove the service:
    .\install-service.ps1 -Action remove
#>

[CmdletBinding()]
param(
    [ValidateSet('install','remove','restart')]
    [string]$Action      = 'install',

    [string]$InstallDir  = $PSScriptRoot,

    [string]$ConfigFile  = (Join-Path $PSScriptRoot '..\config\remsvc.ini'),

    [string]$ServiceName = 'RemSvc',

    [string]$LogDir      = 'C:\ProgramData\RemSvc\logs',

    [int]   $Port        = 50051
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Helpers ───────────────────────────────────────────────────────────────

function Find-Nssm {
    $nssm = Get-Command nssm.exe -ErrorAction SilentlyContinue
    if ($nssm) { return $nssm.Source }

    # Also check alongside this script in case the operator placed it there.
    $local = Join-Path $PSScriptRoot 'nssm.exe'
    if (Test-Path $local) { return $local }

    throw "nssm.exe not found on PATH and not present in $PSScriptRoot. " +
          "Download from https://nssm.cc/download and place nssm.exe on PATH."
}

function Invoke-Nssm {
    param([string]$Nssm, [string[]]$Arguments)
    Write-Host "  nssm $Arguments"
    & $Nssm @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "nssm exited with code $LASTEXITCODE"
    }
}

function Add-FirewallRule {
    param([int]$Port, [string]$ServiceName)
    $ruleName = "$ServiceName gRPC"
    $existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Host "  Firewall rule '$ruleName' already exists — skipping."
        return
    }
    Write-Host "  Adding inbound firewall rule '$ruleName' for port $Port/TCP..."
    New-NetFirewallRule `
        -DisplayName $ruleName `
        -Direction   Inbound `
        -Protocol    TCP `
        -LocalPort   $Port `
        -Action      Allow `
        -Profile     Any | Out-Null
    Write-Host "  Firewall rule added."
}

function Remove-FirewallRule {
    param([int]$Port, [string]$ServiceName)
    $ruleName = "$ServiceName gRPC"
    $existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
    if (-not $existing) {
        Write-Host "  Firewall rule '$ruleName' not found — skipping."
        return
    }
    Remove-NetFirewallRule -DisplayName $ruleName
    Write-Host "  Firewall rule '$ruleName' removed."
}

# ── Resolved paths ────────────────────────────────────────────────────────

$ServerExe  = Join-Path $InstallDir 'RemSvc_server.exe'
$ConfigFile = [System.IO.Path]::GetFullPath($ConfigFile)

# ── Action: remove ────────────────────────────────────────────────────────

if ($Action -eq 'remove') {
    Write-Host "Removing service '$ServiceName'..."
    $nssm = Find-Nssm

    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -ne 'Stopped') {
        Write-Host "  Stopping service..."
        Invoke-Nssm $nssm 'stop', $ServiceName
    }
    Invoke-Nssm $nssm 'remove', $ServiceName, 'confirm'
    Remove-FirewallRule -Port $Port -ServiceName $ServiceName
    Write-Host "Service '$ServiceName' removed."
    exit 0
}

# ── Action: restart ───────────────────────────────────────────────────────

if ($Action -eq 'restart') {
    Write-Host "Restarting service '$ServiceName'..."
    $nssm = Find-Nssm
    Invoke-Nssm $nssm 'restart', $ServiceName
    Write-Host "Service '$ServiceName' restarted."
    exit 0
}

# ── Action: install ───────────────────────────────────────────────────────

Write-Host "Installing RemSvc service..."
Write-Host "  Server exe : $ServerExe"
Write-Host "  Config file: $ConfigFile"
Write-Host "  Log dir    : $LogDir"
Write-Host "  Port       : $Port"

if (-not (Test-Path $ServerExe)) {
    throw "Server binary not found: $ServerExe"
}

$nssm = Find-Nssm
Write-Host "  Using NSSM : $nssm"

# Stop and remove an existing instance so settings are applied cleanly.
$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "  Existing service found — removing before re-install..."
    if ($existing.Status -ne 'Stopped') {
        Invoke-Nssm $nssm 'stop', $ServiceName
    }
    Invoke-Nssm $nssm 'remove', $ServiceName, 'confirm'
}

# Ensure log directory exists.
if (-not (Test-Path $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    Write-Host "  Created log directory: $LogDir"
}

# Register the service.
Invoke-Nssm $nssm 'install', $ServiceName, $ServerExe

# Pass the config file if it exists; warn if it does not.
if (Test-Path $ConfigFile) {
    Invoke-Nssm $nssm 'set', $ServiceName, 'AppParameters', "--config `"$ConfigFile`""
} else {
    Write-Warning "Config file not found at '$ConfigFile'. " +
                  "The server will start with built-in defaults. " +
                  "Copy config\remsvc.ini.example to '$ConfigFile' and edit it."
}

# Set working directory to the bin folder so relative paths in the config
# resolve predictably.
Invoke-Nssm $nssm 'set', $ServiceName, 'AppDirectory', $InstallDir

# Human-readable metadata.
Invoke-Nssm $nssm 'set', $ServiceName, 'DisplayName',  'Remote Execution Service'
Invoke-Nssm $nssm 'set', $ServiceName, 'Description',  'gRPC-based remote command execution service (RemSvc)'

# Start automatically on boot.
Invoke-Nssm $nssm 'set', $ServiceName, 'Start', 'SERVICE_AUTO_START'

# Graceful shutdown: NSSM sends Ctrl+C first (the server handles it cleanly),
# then waits 5 s before escalating to TerminateProcess.
Invoke-Nssm $nssm 'set', $ServiceName, 'AppStopMethodConsole', '5000'
Invoke-Nssm $nssm 'set', $ServiceName, 'AppStopMethodWindow',  '0'
Invoke-Nssm $nssm 'set', $ServiceName, 'AppStopMethodThreads', '0'

# Restart automatically after a crash (5 s delay).
Invoke-Nssm $nssm 'set', $ServiceName, 'AppRestartDelay', '5000'

# Capture stdout/stderr alongside the server's own rotating log.
$stdoutLog = Join-Path $LogDir 'service-stdout.log'
$stderrLog = Join-Path $LogDir 'service-stderr.log'
Invoke-Nssm $nssm 'set', $ServiceName, 'AppStdout',        $stdoutLog
Invoke-Nssm $nssm 'set', $ServiceName, 'AppStderr',        $stderrLog
Invoke-Nssm $nssm 'set', $ServiceName, 'AppRotateFiles',   '1'
Invoke-Nssm $nssm 'set', $ServiceName, 'AppRotateSeconds', '86400'

# Open the firewall port.
Add-FirewallRule -Port $Port -ServiceName $ServiceName

# Start the service.
Write-Host "Starting service '$ServiceName'..."
Invoke-Nssm $nssm 'start', $ServiceName

# Confirm it is running.
Start-Sleep -Seconds 2
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') {
    Write-Host "Service '$ServiceName' is running."
} else {
    Write-Warning "Service '$ServiceName' did not reach Running state. " +
                  "Check the NSSM event log or $stderrLog for details."
}

Write-Host ""
Write-Host "Done.  Useful commands:"
Write-Host "  sc query $ServiceName"
Write-Host "  nssm edit $ServiceName"
Write-Host "  nssm restart $ServiceName"
