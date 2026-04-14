# Installing RemSvc_server as a Windows Service

`RemSvc_server.exe` is a standard console application. The recommended way to
run it as a Windows service is with **NSSM** (Non-Sucking Service Manager), a
free tool that wraps any executable in the Windows Service Control Manager
without requiring changes to the application.

## Automated setup (recommended)

The deployment ZIP includes `bin\install-service.ps1`, which automates all of
the NSSM steps below.  Run it once from an elevated PowerShell prompt:

```powershell
# From the extracted ZIP directory:
.\bin\install-service.ps1

# Point at an explicit config file:
.\bin\install-service.ps1 -ConfigFile C:\ProgramData\RemSvc\remsvc.ini

# Remove the service:
.\bin\install-service.ps1 -Action remove
```

The script installs NSSM settings, opens the firewall port, and starts the
service.  NSSM must be on `PATH` or placed alongside the script.

---

## Manual setup

The steps below are equivalent to what the script performs.  Use them if you
need to customise settings beyond what the script exposes, or if you prefer
to understand each step before running it.

### Prerequisites

- `RemSvc_server.exe` deployed (e.g. extracted from the CPack ZIP to
  `C:\Program Files\RemSvc\bin\`)
- [NSSM](https://nssm.cc/download) — download the 64-bit build and place
  `nssm.exe` anywhere on `PATH` (e.g. `C:\Windows\System32\` or alongside the
  server binary)
- An elevated (Administrator) command prompt for all commands below

### Install the service

```cmd
nssm install RemSvc "C:\Program Files\RemSvc\bin\RemSvc_server.exe"
```

Configure optional server arguments (e.g. config file, port, TLS):

```cmd
nssm set RemSvc AppParameters "--config C:\ProgramData\RemSvc\remsvc.ini"
```

Set a human-readable display name and description:

```cmd
nssm set RemSvc DisplayName "Remote Execution Service"
nssm set RemSvc Description "gRPC-based remote command execution service"
```

Set startup type to automatic (starts on boot):

```cmd
nssm set RemSvc Start SERVICE_AUTO_START
```

Configure graceful shutdown — NSSM sends Ctrl+C, which the server's
`SetConsoleCtrlHandler` already handles cleanly:

```cmd
nssm set RemSvc AppStopMethodConsole 5000
nssm set RemSvc AppStopMethodWindow  0
nssm set RemSvc AppStopMethodThreads 0
```

Configure automatic restart on crash (5 s delay, up to 3 restarts per 60 s):

```cmd
nssm set RemSvc AppRestartDelay 5000
```

Redirect stdout/stderr to log files (optional — the server also writes to its
own rotating log, but NSSM can capture the console output separately):

```cmd
nssm set RemSvc AppStdout "C:\ProgramData\RemSvc\logs\service-stdout.log"
nssm set RemSvc AppStderr "C:\ProgramData\RemSvc\logs\service-stderr.log"
nssm set RemSvc AppRotateFiles 1
nssm set RemSvc AppRotateSeconds 86400
```

Start the service:

```cmd
nssm start RemSvc
```

Verify it is running:

```cmd
sc query RemSvc
```

---

## Day-to-day management

| Action | Command |
|---|---|
| Start | `nssm start RemSvc` |
| Stop | `nssm stop RemSvc` |
| Restart | `nssm restart RemSvc` |
| Status | `sc query RemSvc` |
| Edit settings (GUI) | `nssm edit RemSvc` |

---

## Uninstall the service

```cmd
nssm stop RemSvc
nssm remove RemSvc confirm
```

---

## Notes

- The server must be run under an account with sufficient privileges to bind
  the configured port and execute remote commands. By default NSSM uses
  `LocalSystem`; change via `nssm set RemSvc ObjectName <domain\user>` if
  needed.
- If TLS is enabled, ensure the certificate and key paths in the config file
  are absolute and readable by the service account.
- Windows Firewall: open the server port (default `50051`) inbound:
  ```cmd
  netsh advfirewall firewall add rule ^
      name="RemSvc gRPC" protocol=TCP dir=in localport=50051 action=allow
  ```
