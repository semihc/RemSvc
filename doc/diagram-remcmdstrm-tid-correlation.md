# Diagram: RemCmdStrm tid Correlation

Shows how transaction IDs (`tid`) are assigned by the trigger, echoed by the
server, and used to correlate out-of-order responses back to their originating
commands.

## Happy path — three commands, responses arrive out of order

```mermaid
sequenceDiagram
    autonumber

    participant W  as _write() coroutine
    participant S  as RemCmdStrm stream
    participant SV as RemSvc server
    participant R  as _read() coroutine
    participant C  as results dict

    Note over W,C: asyncio.gather(_write(), _read()) — both run concurrently

    Note over W: assign tid = 1-based index

    W  ->> S  : write RemCmdMsg(tid=1, cmd="echo hello", hsh=crc32)
    W  ->> S  : write RemCmdMsg(tid=2, cmd="hostname",   hsh=crc32)
    W  ->> S  : write RemCmdMsg(tid=3, cmd="whoami",     hsh=crc32)
    W  ->> S  : done_writing()

    Note over SV: server dispatches commands sequentially<br/>(one child process at a time)

    SV ->> S  : RemResMsg(tid=3, rc=0, out="alice")    ← arrives first (fast cmd)
    R  ->> C  : results[3] = {tid:3, rc:0, out:"alice", cmd:"whoami"}

    SV ->> S  : RemResMsg(tid=1, rc=0, out="hello")
    R  ->> C  : results[1] = {tid:1, rc:0, out:"hello", cmd:"echo hello"}

    SV ->> S  : RemResMsg(tid=2, rc=0, out="myhost")
    R  ->> C  : results[2] = {tid:2, rc:0, out:"myhost", cmd:"hostname"}

    Note over R,C: _read() returns — stream exhausted

    Note over C: post-stream analysis
    C  ->> C  : sent_tids  = {1,2,3}
    C  ->> C  : missing    = sent_tids − results.keys() = ∅
    C  ->> C  : failed_tids = [tid for tid if rc≠0]     = []

    C -->> W  : EVT_STATE=SUCCESS, EVT_RESULTS={1:…,2:…,3:…}
```

## Error path — missing response

```mermaid
sequenceDiagram
    autonumber

    participant W  as _write() coroutine
    participant S  as RemCmdStrm stream
    participant SV as RemSvc server
    participant R  as _read() coroutine
    participant C  as results dict

    W  ->> S  : write RemCmdMsg(tid=1, cmd="echo hello")
    W  ->> S  : write RemCmdMsg(tid=2, cmd="hostname")
    W  ->> S  : done_writing()

    SV ->> S  : RemResMsg(tid=1, rc=0, out="hello")
    R  ->> C  : results[1] = {…}

    Note over SV: server closes stream early (crash / timeout)
    Note over R : stream exhausted — tid=2 never received

    C  ->> C  : missing = {2}
    C -->> W  : EVT_STATE=FAILED, EVT_ERROR="No response for tid(s): [2]"
```

## Error path — non-zero exit code

```mermaid
sequenceDiagram
    autonumber

    participant W  as _write() coroutine
    participant S  as RemCmdStrm stream
    participant SV as RemSvc server
    participant R  as _read() coroutine
    participant C  as results dict

    W  ->> S  : write RemCmdMsg(tid=1, cmd="echo ok")
    W  ->> S  : write RemCmdMsg(tid=2, cmd="exit 1")
    W  ->> S  : done_writing()

    SV ->> S  : RemResMsg(tid=1, rc=0, out="ok")
    R  ->> C  : results[1] = {rc:0, …}

    SV ->> S  : RemResMsg(tid=2, rc=1, err="command failed")
    R  ->> C  : results[2] = {rc:1, …}

    C  ->> C  : failed_tids = [2]
    C -->> W  : EVT_STATE=FAILED, EVT_RESULTS={1:…,2:…},<br/>EVT_ERROR="Non-zero rc for tid(s): [2]"

    Note over C: partial results are still returned in EVT_RESULTS<br/>so the operator can inspect which commands succeeded
```

## tid assignment rules

| Rule | Detail |
|------|--------|
| Assigned by | Python trigger (`_run_stream`) and C++ client (`doRemCmdStrm`) |
| Value | 1-based index in the `commands` list — first command is `tid=1` |
| Echoed by | Server copies `tid` from request into `RemResMsg` unchanged |
| Out-of-order | Responses may arrive in any order; `results[tid]` handles this |
| Duplicate | Last response wins (warning logged); impossible in practice (server echoes each request once) |
| Missing | `sent_tids − results.keys()` detected post-stream → `FAILED` |
| Zero | `tid=0` means unset (proto default); reserved — never assigned by clients |
