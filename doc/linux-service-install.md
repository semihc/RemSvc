# Running RemSvc_server on Linux

Two approaches are covered:

- **[A] System-wide systemd service** — runs as a dedicated system user,
  starts on boot, managed by root / sudo.
- **[B] User-mode systemd service** — runs under your own login, no root
  required; starts when you log in (or on boot with lingering enabled).

Both approaches rely on `SIGTERM`, which `RemSvc_server` already handles
gracefully via `SignalHandler` (POSIX self-pipe + `QSocketNotifier`).

---

## A. System-wide systemd service (privileged)

### 1. Create a dedicated service account

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin remsvc
```

### 2. Deploy the binary and config

```bash
# Extract the CPack TGZ (adjust version as needed)
sudo tar -xzf RemSvc-1.0.0-linux-x64.tar.gz -C /opt/remsvc --strip-components=1

# Create config and log directories
sudo mkdir -p /etc/remsvc /var/log/remsvc

# Install your config file
sudo cp /path/to/remsvc.ini /etc/remsvc/remsvc.ini

# Ownership — service account only
sudo chown -R remsvc:remsvc /etc/remsvc /var/log/remsvc
sudo chmod 750 /etc/remsvc /var/log/remsvc
sudo chmod 640 /etc/remsvc/remsvc.ini
```

If TLS is enabled, place certs under `/etc/remsvc/` and reference them in the
config using absolute paths or `$VAR` expansion (the config loader expands
`$VAR` and `${VAR}` using the service's environment):

```ini
[tls]
enabled = true
cert = /etc/remsvc/server.crt
key  = /etc/remsvc/server.key
ca   = /etc/remsvc/ca.crt
```

### 3. Create the systemd unit file

```bash
sudo tee /etc/systemd/system/remsvc.service > /dev/null << 'EOF'
[Unit]
Description=Remote Execution Service (gRPC)
After=network.target

[Service]
Type=simple
User=remsvc
Group=remsvc

ExecStart=/opt/remsvc/bin/RemSvc_server.sh --config /etc/remsvc/remsvc.ini
Restart=on-failure
RestartSec=5s

# Give the server 10 s to drain connections on SIGTERM before SIGKILL
TimeoutStopSec=10

# Log to the journal (view with: journalctl -u remsvc -f)
StandardOutput=journal
StandardError=journal
SyslogIdentifier=remsvc

# Hardening (optional but recommended)
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ReadWritePaths=/var/log/remsvc

[Install]
WantedBy=multi-user.target
EOF
```

### 4. Enable and start

```bash
sudo systemctl daemon-reload
sudo systemctl enable remsvc      # start on boot
sudo systemctl start  remsvc
sudo systemctl status remsvc
```

### Day-to-day management

| Action | Command |
|---|---|
| Start | `sudo systemctl start remsvc` |
| Stop | `sudo systemctl stop remsvc` |
| Restart | `sudo systemctl restart remsvc` |
| Status | `sudo systemctl status remsvc` |
| Live logs | `journalctl -u remsvc -f` |
| Logs since boot | `journalctl -u remsvc -b` |
| Disable autostart | `sudo systemctl disable remsvc` |

### Uninstall

```bash
sudo systemctl stop    remsvc
sudo systemctl disable remsvc
sudo rm /etc/systemd/system/remsvc.service
sudo systemctl daemon-reload
sudo userdel remsvc
sudo rm -rf /opt/remsvc /etc/remsvc /var/log/remsvc
```

### Firewall (firewalld)

```bash
sudo firewall-cmd --permanent --add-port=50051/tcp
sudo firewall-cmd --reload
```

---

## B. User-mode service (no root required)

Useful for running the server under a normal user account — typical for a
development host or an Airflow worker node where you have no sudo access.

### 1. Deploy the binary

```bash
# Extract to a user-writable location
tar -xzf RemSvc-1.0.0-linux-x64.tar.gz -C ~/opt/remsvc --strip-components=1

# Config and log dirs
mkdir -p ~/.config/remsvc ~/.local/share/remsvc/logs
cp /path/to/remsvc.ini ~/.config/remsvc/remsvc.ini
```

Example config referencing `$HOME` (the server expands env vars in paths):

```ini
[server]
port = 50051

[log]
file  = $HOME/.local/share/remsvc/logs/remsvc.log
level = info

[tls]
enabled = true
cert = $HOME/.config/remsvc/server.crt
key  = $HOME/.config/remsvc/server.key
```

### 2. Create the user systemd unit file

```bash
mkdir -p ~/.config/systemd/user

cat > ~/.config/systemd/user/remsvc.service << 'EOF'
[Unit]
Description=Remote Execution Service (gRPC) — user mode
After=network.target

[Service]
Type=simple
ExecStart=%h/opt/remsvc/bin/RemSvc_server.sh --config %h/.config/remsvc/remsvc.ini
Restart=on-failure
RestartSec=5s
TimeoutStopSec=10

# %h expands to $HOME in user units
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
EOF
```

### 3. Enable and start

```bash
systemctl --user daemon-reload
systemctl --user enable remsvc      # start on next login
systemctl --user start  remsvc
systemctl --user status remsvc
```

### Start on boot (without interactive login)

By default user services only run while you are logged in. To keep the service
running after you log out (e.g. on a server):

```bash
sudo loginctl enable-linger $USER
```

This is a one-time command. From then on, your user systemd instance starts at
boot and your `remsvc.service` starts with it.

### Day-to-day management

| Action | Command |
|---|---|
| Start | `systemctl --user start remsvc` |
| Stop | `systemctl --user stop remsvc` |
| Restart | `systemctl --user restart remsvc` |
| Status | `systemctl --user status remsvc` |
| Live logs | `journalctl --user -u remsvc -f` |
| Disable autostart | `systemctl --user disable remsvc` |

### Uninstall

```bash
systemctl --user stop    remsvc
systemctl --user disable remsvc
rm ~/.config/systemd/user/remsvc.service
systemctl --user daemon-reload
```

---

## Choosing between A and B

| | System-wide (A) | User-mode (B) |
|---|---|---|
| Requires root | Yes (setup only) | No |
| Starts on boot | Yes | Yes (with `enable-linger`) |
| Isolated service account | Yes | No (runs as your user) |
| Can bind ports < 1024 | Yes | No (use port ≥ 1024) |
| Logs | `journalctl -u remsvc` | `journalctl --user -u remsvc` |
| Suitable for production | Yes | Development / non-root hosts |
