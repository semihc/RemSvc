# Diagram: Airflow Deferrable Execution Flow

Shows the three-phase lifecycle of a `RemSvcOperator` task.
The worker slot is held only during the brief validation and XCom-push phases;
all command execution and waiting happens inside the triggerer process.

```mermaid
sequenceDiagram
    autonumber

    participant S  as Airflow Scheduler
    participant W  as Worker (slot held)
    participant DB as Metadata DB
    participant T  as Triggerer process
    participant G  as RemSvc gRPC server

    Note over W: Phase 1 — execute() [worker slot held]

    S  ->> W  : launch task / assign worker slot
    W  ->> W  : validate commands (non-empty, non-blank)
    W  ->> W  : import proto stubs — raise ImportError if missing

    W  ->> DB : persist RemSvcTrigger(commands, grpc_conn_id,<br/>stream_timeout, metadata)
    W  -->> S : raise TaskDeferred → worker slot released

    Note over W: worker slot is FREE for the duration of Phase 2

    Note over T: Phase 2 — RemSvcTrigger.run() [inside triggerer]

    S  ->> T  : hand trigger to triggerer event loop
    T  ->> DB : deserialise RemSvcTrigger via serialize() / constructor

    loop Up to _MAX_ATTEMPTS retries (UNAVAILABLE / RESOURCE_EXHAUSTED, only if sent_ref==0)
        T  ->> T  : sent_ref = [0]  ← mutable write counter for this attempt
        T  ->> T  : _hook() → RemSvcHook (Airflow DB lookup, cached)
        T  ->> T  : _async_channel() → pooled grpc.aio.Channel
        T  ->> T  : _call_metadata() → merge conn auth + operator metadata
        T  ->> G  : open RemCmdStrm(timeout, metadata)

        par asyncio.gather
            T  ->> G  : write RemCmdMsg(tid=1, cmd, hsh) → sent_ref[0]++
            T  ->> G  : write RemCmdMsg(tid=N, cmd, hsh) → sent_ref[0]++
            T  ->> G  : done_writing()
        and
            G -->> T  : RemResMsg(tid=?, rc, out, err) [any order]
            G -->> T  : RemResMsg(tid=?) …
        end

        T  ->> T  : correlate responses by tid
        T  ->> T  : check missing tids / non-zero rc

        alt success
            T  ->> DB : yield TriggerEvent(state=SUCCESS, results={tid:…})
        else transient gRPC error AND sent_ref[0] == 0
            T  ->> T  : sleep(backoff) then retry
        else transient gRPC error AND sent_ref[0] > 0
            T  ->> DB : yield TriggerEvent(state=FAILED, error="…not retried to prevent duplicate execution")
        else timeout / permanent error
            T  ->> DB : yield TriggerEvent(state=CANCELLED|FAILED, error)
        end
    end

    Note over W: Phase 3 — execute_complete() [worker slot held briefly]

    DB ->> S  : TriggerEvent received
    S  ->> W  : resume task / assign worker slot
    W  ->> W  : execute_complete(context, event)
    W  ->> W  : check state — raise AirflowException if FAILED/CANCELLED
    W  ->> W  : result_handler({tid: result_dict}) → sorted list
    W  ->> DB : push RemSvcResult to XCom (return_value)
    W -->> S  : task SUCCESS
```

## Notes

- **Worker slot occupancy:** held only during steps 1–4 (validate + defer) and
  steps 18–22 (resume + XCom push).  The entire gRPC stream runs in the
  triggerer with zero worker slots consumed.
- **Retry policy:** `UNAVAILABLE` and `RESOURCE_EXHAUSTED` are retried up to
  `_MAX_ATTEMPTS` (3) times with exponential backoff (1 s → 2 s → 4 s, capped
  at 16 s), **but only when no writes have been dispatched in that attempt**
  (`sent_ref[0] == 0`).  If the connection drops after one or more commands have
  already been written, the trigger fails immediately with `FAILED` rather than
  retrying — retrying would resend all commands from the top, causing duplicate
  execution of non-idempotent commands on the remote host.
  `DEADLINE_EXCEEDED`, `asyncio.TimeoutError`, and all other errors fail
  immediately without retry regardless of how many commands were sent.
- **Channel pool:** `_async_channel()` returns a module-level pooled
  `grpc.aio.Channel` shared across all concurrent triggers targeting the same
  host/TLS configuration.  The channel is never closed by the trigger.
- **Auth metadata:** the connection-level `bearer_token` always takes precedence
  over any `authorization` header in the operator's `metadata` parameter.
