# Diagram: Deployment Topology

Shows how the RemSvc components are distributed across hosts and what
communication channels exist between them.

```mermaid
graph TB
    subgraph airflow_host ["Airflow Host"]
        direction TB
        SCH["Scheduler\n(task orchestration)"]
        WRK["Worker pool\n(execute / execute_complete)"]
        TRG["Triggerer process\n(asyncio event loop)\nRemSvcTrigger"]
        MDB[("Metadata DB\n(PostgreSQL / MySQL)\nconnection config\ntrigger state\nXCom results")]

        SCH <-->|"task lifecycle\nevents"| MDB
        WRK <-->|"XCom push\nconnection lookup"| MDB
        TRG <-->|"TriggerEvent\nconnection config\n(bearer_token, host, TLS)"| MDB
    end

    subgraph remote_host ["Remote Host (one or more)"]
        direction TB
        SRV["RemSvc_server\n(gRPC server)\nport 50051"]
        PROC["Child processes\n(cmd.exe /C  or  /bin/sh -c\nor  powershell.exe / pwsh)\none per command"]
        INI["server.ini\n[server] [tls] [auth]\n[denylist] [allowlist] [log]"]

        SRV -->|"QProcess::start()\nstdout/stderr pipes"| PROC
        INI -.->|"loaded at startup"| SRV
    end

    subgraph certs_store ["Certificate store (remote host)"]
        CERT["server.crt\nserver.key\nca.pem (optional)"]
    end

    CERT -.->|"read at startup\n$VAR expansion"| SRV

    TRG  <-->|"gRPC bidirectional stream\nRemCmdStrm\n(TLS optional)\nAuthorization: Bearer token"| SRV

    WRK  -->|"gRPC unary RPCs\n(future: Ping, GetStatus)"| SRV

    style airflow_host  fill:#e8f4f8,stroke:#4a90d9,stroke-width:2px
    style remote_host   fill:#f8f4e8,stroke:#d9904a,stroke-width:2px
    style certs_store   fill:#f4f8e8,stroke:#6aa84f,stroke-width:1px,stroke-dasharray:4
```

## Security boundary

```mermaid
graph LR
    subgraph untrusted ["Network (untrusted)"]
        direction LR
        CH["gRPC channel\nTCP 50051"]
    end

    subgraph airflow_side ["Airflow side"]
        AMETA["bearer_token\n(Airflow connection extra)\nnever logged"]
    end

    subgraph server_side ["Server side"]
        AUTH["BearerTokenAuthProcessor\n(runs before every RPC)\nconstant-time comparison"]
        DENY["Command denylist\n(std::regex)\nchecked first — deny wins"]
        ALLOW["Command allowlist\n(std::regex)\nchecked second"]
        EXEC["runInProcess()\ncmdTimeoutMs kill deadline"]
    end

    AMETA -->|"Authorization: Bearer token\n(encrypted if TLS enabled)"| CH
    CH    -->|"gRPC metadata header"| AUTH
    AUTH  -->|"identity stamped on AuthContext\nx-remsvc-identity"| DENY
    DENY  -->|"no deny pattern matched"| ALLOW
    ALLOW -->|"allow pattern matched\n(or allowlist empty)"| EXEC

    AUTH  --x|"UNAUTHENTICATED\n(token mismatch or absent)"| X1[ ]
    DENY  --x|"PERMISSION_DENIED\n(deny pattern matched)"| X2[ ]
    ALLOW --x|"PERMISSION_DENIED\n(no allow pattern matches)"| X3[ ]
    EXEC  --x|"killed + rc≠0\n(timeout exceeded)"| X4[ ]

    style untrusted    fill:#fff0f0,stroke:#cc4444,stroke-width:1px,stroke-dasharray:4
    style airflow_side fill:#e8f4f8,stroke:#4a90d9,stroke-width:2px
    style server_side  fill:#f8f4e8,stroke:#d9904a,stroke-width:2px
```

## Component inventory

| Component | Host | Binary / Package | Notes |
|-----------|------|-----------------|-------|
| Airflow Scheduler | Airflow host | Airflow built-in | Manages task state machine |
| Airflow Worker | Airflow host | Airflow built-in | Holds slot only during validate + XCom push |
| Airflow Triggerer | Airflow host | `airflow-provider-remsvc` | Runs `RemSvcTrigger` coroutines |
| RemSvc gRPC server | Remote host | `RemSvc_server.exe` | Long-running process; config via `server.ini` |
| RemSvc CLI client | Any host | `RemSvc_client.exe` | Ad-hoc / scripting use |
| gRPC stubs | Airflow host | `remsvc_proto/` | Auto-generated at `pip install` time |
