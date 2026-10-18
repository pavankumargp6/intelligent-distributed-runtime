# IDR Wire Protocol

This document is the canonical reference for the wire protocol
used between IDR clients, the coordinator, and workers. The
README contains a short summary; changes to a message format
should be made here first.

The document is descriptive, not normative. Statements describe
what the implementation currently does. Where the implementation
has limitations, those are called out explicitly.

## 1. Framing

Every message on the wire is length-prefixed:

```
[4-byte network-order uint32 payload length][payload]
```

- The length header is a big-endian 32-bit unsigned integer.
- The payload is exactly that many bytes of ASCII text.
- `NETWORK_MAX_MESSAGE_SIZE` is 4096. A payload larger than this
  is treated as a framing error by the receiver, which closes
  the connection.
- A payload is not null-terminated on the wire. The receiver
  appends a null byte internally after reading the full payload
  so that string functions can process it.
- Multiple messages on the same connection are not part of the
  current design. Each request/response pair uses its own
  connection; see section 8.

Example. The 12-byte payload `HEARTBEAT|1` occupies 16 bytes on
the wire:

```
00 00 00 0B 48 45 41 52 54 42 45 41 54 7C 31
^---------^  ^----------------------------------^
length = 11  payload = "HEARTBEAT|1"
```

Note: the length is 11, not 12 — the `|` and digits count, the
frame header does not.

## 2. Field encoding

Payloads are ASCII text. Fields within a payload are separated
by the pipe character `|`.

Two kinds of fields exist:

**Fixed fields.** A single token with no internal structure.
Integer fields are decimal digits with an optional leading
minus. No leading zeros unless the value is zero.

**Length-prefixed fields.** A field whose content may contain
`|`, spaces, newlines, or any other byte. The encoding is:

```
<decimal-length>:<that-many-bytes>
```

The length is decimal ASCII, terminated by a literal colon.
The bytes that follow are the field content, exactly as many
as the length says. No escaping is applied.

Example. The command `echo` with one argument `hello world`
encodes as:

```
4:echo|1|11:hello world
```

Broken apart:

```
4:echo        ->  the 4-byte string "echo"
|             ->  delimiter
1             ->  argument count = 1
|             ->  delimiter
11:hello world  ->  the 11-byte string "hello world"
```

The embedded space in `hello world` is preserved. A `|` inside a
command would likewise be preserved because the receiver reads
exactly 11 bytes, not "until the next delimiter".

**Encoding on the wire.** A `SUBMIT` message for the command above
becomes:

```
SUBMIT|4:echo|1|11:hello world
```

And framed:

```
00 00 00 1D [SUBMIT|4:echo|1|11:hello world]
```

Length 29 = 6 + 1 + 6 + 1 + 1 + 1 + 13, counting every byte of the
payload.

## 3. Authentication

Every message in secure mode carries a trailing authentication
suffix. Insecure mode (`IDR_INSECURE=1`, loopback only) omits it;
the receiving side uses the message as-is.

### 3.1 Wire form

```
<original_message>|<timestamp>|<hex_hmac>
```

- `<original_message>` is the payload from section 2, byte-for-byte.
- `<timestamp>` is decimal ASCII: seconds since the Unix epoch,
  no leading zeros.
- `<hex_hmac>` is 64 lowercase hex characters: HMAC-SHA256 of
  the canonical input described below.

The three fields are separated by `|`. Because
`<original_message>` may itself contain `|`, the receiver parses
from the right: the last `|` delimits the MAC, the next one
delimits the timestamp, and everything before that is the
message.

### 3.2 Canonical MAC input

The MAC input is a string, not the same as what goes on the wire:

```
IDR-AUTH-V1|<direction>|<signer_role>|<timestamp>|<original_message>
```

| Field | Value |
|---|---|
| `IDR-AUTH-V1` | fixed literal, protocol version marker |
| `<direction>` | one of `worker-to-coordinator`, `coordinator-to-worker`, `client-to-coordinator`, `coordinator-to-client` |
| `<signer_role>` | one of `worker`, `client`, `coordinator` — the **sender's** role |
| `<timestamp>` | same decimal string that appears on the wire |
| `<original_message>` | same bytes that appear on the wire |

**Direction and signer role are distinct from the secret used.**
A coordinator signs its response to a worker with the *worker
secret* (`IDR_WORKER_SECRET`) but its signer role is
`coordinator` and its direction is `coordinator-to-worker`. The
secret determines who can forge the message; the direction and
role determine what the message is.

### 3.3 Message-to-role mapping

The receiver determines the expected direction and signer role
from the message type before verifying:

| Message type (prefix) | Direction | Signer role | Secret |
|---|---|---|---|
| `REGISTER` | `worker-to-coordinator` | `worker` | worker |
| `HEARTBEAT` | `worker-to-coordinator` | `worker` | worker |
| `TASK_REQUEST` | `worker-to-coordinator` | `worker` | worker |
| `TASK_ACCEPT` | `worker-to-coordinator` | `worker` | worker |
| `TASK_RESULT` | `worker-to-coordinator` | `worker` | worker |
| `SUBMIT` | `client-to-coordinator` | `client` | client |
| `QUERY` | `client-to-coordinator` | `client` | client |
| `STATUS` | `client-to-coordinator` | `client` | client |

Coordinator responses are signed with the direction and role
matching the request that triggered them: a response to a worker
uses `coordinator-to-worker` and role `coordinator`; a response
to a client uses `coordinator-to-client` and role `coordinator`.
The secret is the same role secret the request used.

### 3.4 Timestamp window

The receiver accepts a message whose timestamp is:

- not more than 60 seconds in the past (`AUTH_MAX_AGE_SECONDS`)
- not more than 5 seconds in the future (`AUTH_MAX_FUTURE_SECONDS`)

Values outside this window are rejected before MAC verification,
with no response. The windows assume sender and receiver clocks
are within a few seconds of each other; NTP or equivalent is
required across hosts.

### 3.5 Replay cache

After a message's MAC is verified, its 32-byte MAC is recorded in
a bounded in-process cache (`AUTH_REPLAY_CACHE_SIZE`, 8192
entries). A second verification of a byte-identical message
within the timestamp window is rejected.

A message that arrives with a fresh timestamp but identical
content produces a different MAC and is not rejected by the
cache. This is intentional: HMAC authenticates bytes, not
intentions. Preventing duplicate application-level submissions
requires an idempotency mechanism at the client or scheduler
layer; see the README's limitations section.

If the cache is full of unexpired entries, verification returns
`AUTH_ERR_REPLAY_CACHE_FULL` and the connection is closed.
Nothing is silently dropped from the cache to make room.

### 3.6 Worked signing example

Suppose the coordinator wants to respond `HEARTBEAT_ACK` to a
worker with the current timestamp `1700000000`. The MAC input is:

```
IDR-AUTH-V1|coordinator-to-worker|coordinator|1700000000|HEARTBEAT_ACK
```

The signer computes:

```
mac = HMAC-SHA256(IDR_WORKER_SECRET, mac_input)
```

The value of `mac` depends on `IDR_WORKER_SECRET`, which is
configured per deployment. There is no fixed test vector for the
full system; the HMAC-SHA256 primitive itself is verified against
RFC 4231 in `tests/hmac_test.c`.

The wire form after signing is:

```
HEARTBEAT_ACK|1700000000|<64-hex-mac>
```

Note the two representations:

- **MAC input** includes the `IDR-AUTH-V1|direction|role|`
  prefix. It is never transmitted.
- **Wire form** appends only the timestamp and hex MAC. The
  receiver reconstructs the MAC input locally to verify.

## 4. Message catalog

Payloads below show the *pre-authentication* form. In secure
mode, every message in this section carries the trailing
`|<timestamp>|<hex_hmac>` described in section 3.

### 4.1 Worker to coordinator

**`REGISTER|<id>|<total_cpu>|<total_memory_mb>`**

Sent once per worker agent lifetime, and again after any failed
heartbeat. `total_cpu` is a decimal floating-point value
representing CPU capacity as a percentage of one core
(8 cores = `800.00`). `total_memory_mb` is decimal.

Response: `REGISTER_ACK` or `REGISTER_REJECT`.

**`HEARTBEAT|<id>`**

Sent roughly every 3 seconds while the worker is healthy.
`last_seen` for that worker is refreshed on receipt.

Response: `HEARTBEAT_ACK` or `HEARTBEAT_REJECT` (unknown worker).

**`TASK_REQUEST|<worker_id>`**

Sent on each poll cycle to request work. If the coordinator has
no task for this worker, it replies `NO_TASK`.

Response: `TASK_ASSIGN|...` or `NO_TASK`.

**`TASK_ACCEPT|<task_id>`**

Sent after the worker has received and parsed a `TASK_ASSIGN`,
before forking the executor. Marks the corresponding in-flight
entry as accepted so the dispatch-timeout reaper leaves it alone.

Response: `TASK_ACCEPT_ACK|<task_id>` or
`TASK_ACCEPT_REJECT|<task_id>|<reason>`.

**`TASK_RESULT|<task_id>|<worker_id>|<exit_status>|<state>`**

Sent by the forked task-runner child after execution finishes.
`exit_status` is the process exit code, or `-1` if the task never
produced a status. `state` is the enum in section 6.

Response: `TASK_RESULT_ACK` (always sent, even for a task the
coordinator no longer knows about).

### 4.2 Client to coordinator

**`SUBMIT|<len>:<cmd>|<argc>|<len>:<arg1>|...|[<idempotent>]|[<timeout_seconds>]`**

`<cmd>` is the command name. `<argc>` is a decimal count; each
argument follows as a length-prefixed field. The trailing
`<idempotent>` field is optional and defaults to `0` when
absent. A non-zero value marks the task as safe to requeue on
dispatch timeout; `0` means "if dispatch fails, fail the task".

Response: `SUBMIT_ACK|<task_id>` or `SUBMIT_REJECT`.

**`QUERY|<task_id>`**

Looks up the current state of a task.

Response: one of

- `QUERY_RESULT|<id>|<worker>|<exit>|<state>` — task finished
- `QUERY_PENDING|<id>` — task currently in the in-flight table
- `QUERY_UNKNOWN|<id>` — task never seen, or aged out of the
  completed ring

**`STATUS`**

No fields. Returns a snapshot of runtime counters.

Response: `STATUS_REPLY|<counters>` — see section 5.5.

### 4.3 Coordinator to worker

**`REGISTER_ACK`** — registration accepted.

**`REGISTER_REJECT`** — malformed registration or registry full.

**`HEARTBEAT_ACK`** — heartbeat accepted, `last_seen` refreshed.

**`HEARTBEAT_REJECT`** — unknown worker ID.

**`NO_TASK`** — no work available for this worker right now.

**`TASK_ASSIGN|<id>|<len>:<cmd>|<argc>|<len>:<arg1>|...|<timeout_seconds>`**

Same field layout as `SUBMIT` minus the idempotent flag. Sent in
response to a `TASK_REQUEST`.

**`TASK_ACCEPT_ACK|<task_id>`** — task accepted by worker,
recorded.

**`TASK_ACCEPT_REJECT|<task_id>|<reason>`** — task unknown or
unacceptable. `<reason>` is a short ASCII token, currently
`unknown_task` or `bad_id`.

**`TASK_RESULT_ACK`** — result recorded. Sent even for tasks the
coordinator no longer has in-flight, so a worker that reports
late does not retry.

**`UNKNOWN_COMMAND`** — the received message did not match any
recognized type. No response follows.

### 4.4 Coordinator to client

**`SUBMIT_ACK|<task_id>`** — task accepted and queued.

**`SUBMIT_REJECT`** — task rejected (queue full, WAL append
failed, parse error).

**`QUERY_RESULT|<task_id>|<worker_id>|<exit_status>|<state>`**

**`QUERY_PENDING|<task_id>`**

**`QUERY_UNKNOWN|<task_id>`**

**`STATUS_REPLY|<counters>`** — see section 5.5.

## 5. Message examples

Every example below shows the payload as it appears in the wire
frame. The `[N]` prefix denotes the 4-byte length header; the
text that follows is the payload.

### 5.1 Worker registration

A worker with ID `1`, reporting 8 CPU cores and 7710 MB:

```
[22] REGISTER|1|800.00|7710
```

Length 22 counts every byte of `REGISTER|1|800.00|7710`.

### 5.2 Client submission

The client submits `echo hello world`:

```
[31] SUBMIT|4:echo|2|5:hello|5:world|0|0
```

Decoded:

- `SUBMIT` — message type
- `4:echo` — command
- `2` — two arguments
- `5:hello` — first argument
- `5:world` — second argument

### 5.3 Task assignment

The coordinator assigns task 1 to whichever worker requested it:

```
[39] TASK_ASSIGN|1|4:echo|2|5:hello|5:world|0
```

Length 37 counts `TASK_ASSIGN|` (12) + `1` (1) + `|` (1) +
`4:echo` (6) + `|` (1) + `2` (1) + `|` (1) + `5:hello` (7) +
`|` (1) + `5:world` (7).

### 5.4 Signed submission

The same `SUBMIT` message after signing, with timestamp
`1700000000`:

```
SUBMIT|4:echo|2|5:hello|5:world|1700000000|<64-hex-mac>
```

`<64-hex-mac>` is a placeholder. Its actual value depends on
`IDR_CLIENT_SECRET`, which is configured per deployment. To
produce a reproducible example, run:

```bash
IDR_CLIENT_SECRET="..." ./build/client_submit echo hello world
```

The client logs the signed payload it sends.

### 5.5 Status reply

```
STATUS_REPLY|workers=0|active=0|queue=3|inflight=0|submitted=3|completed=0|failed=0
```

Field meanings:

| Field | Meaning |
|---|---|
| `workers` | total workers in the registry |
| `active` | registry entries with status `WORKER_ACTIVE` |
| `queue` | tasks currently in the FIFO queue |
| `inflight` | entries in the in-flight table |
| `submitted` | cumulative tasks submitted since coordinator start (or since the last WAL replay) |
| `completed` | cumulative tasks completed |
| `failed` | cumulative tasks failed |

## 6. Task state machine

States are defined in `task.h` as `enum TaskState`:

| Value | Name | Meaning |
|---|---|---|
| 0 | `TASK_CREATED` | accepted, in queue or being assigned |
| 1 | `TASK_RUNNING` | (not used by the coordinator; reserved) |
| 2 | `TASK_COMPLETED` | worker reported exit status 0 |
| 3 | `TASK_FAILED` | worker reported nonzero, worker was lost, or dispatch timed out on a non-idempotent task |

A task's journey through the coordinator:

```
SUBMIT received
   |
   v
queued (WAL: SUBMIT|<id>|...)
   |
   v  worker sends TASK_REQUEST, scheduler picks it
in-flight table (worker_id, dispatched_at, accepted=0)
   |
   v  worker sends TASK_ACCEPT
accepted=1, no longer subject to dispatch timeout
   |
   v  worker sends TASK_RESULT
completed ring (WAL: RESULT|<id>|...)
```

Alternative paths:

- **Dispatch timeout on a non-idempotent task.** In-flight entry is
  removed, task is moved to the completed ring with state
  `TASK_FAILED` and exit status `-1`, and a `RESULT` record is
  appended to the WAL.
- **Dispatch timeout on an idempotent task.** In-flight entry is
  removed and the task is requeued. The scheduler may dispatch it
  again on the next `TASK_REQUEST`.
- **Worker loss.** When `mark_stale_workers` marks the assigned
  worker unavailable, the in-flight reaper runs on the next
  coordinator tick. Its behavior is the same as a dispatch
  timeout — idempotent tasks are requeued, non-idempotent tasks
  are failed.

## 7. Errors and rejections

The coordinator sends an explicit rejection when it can identify
what went wrong:

| Rejection | Trigger |
|---|---|
| `REGISTER_REJECT` | malformed registration, or registry/pool capacity exceeded |
| `HEARTBEAT_REJECT` | malformed heartbeat, or unknown worker ID |
| `SUBMIT_REJECT` | malformed submission, queue full, WAL append failure |
| `TASK_ACCEPT_REJECT` | task ID unknown to the in-flight table |
| `UNKNOWN_COMMAND` | message type does not match any handler |

**Authentication failures do not produce a rejection message.**
The connection is closed without a response. This is deliberate:
sending a signed error would reveal whether the MAC, the
timestamp, or the message type was the problem. A silent close
gives an attacker no oracle.

## 8. Connection model

The current coordinator accepts one connection, reads one
message, sends at most one response, and closes the connection.
This is a deliberate simplification, not a limitation of the
protocol.

Consequences:

- Clients and workers open a fresh TCP connection for each
  request. A worker opening a connection to register,
  heartbeat, request work, accept a task, and report a result
  is not unusual.
- No connection holds state between messages. `peer_role`,
  `response_secret`, and `response_direction` are set on a
  connection after successful authentication and are discarded
  when the connection closes.
- The coordinator handles many connections concurrently via
  `poll()`. A single stuck connection does not block others;
  see `tests/hung_client_test.sh`.
- Idle connections are closed after 5 seconds
  (`CONNECTION_IDLE_TIMEOUT_SECONDS`).

A future protocol revision could add persistent connections with
a connection-level identifier. The framing layer as described in
section 1 supports this without change; only the connection
lifecycle would differ.

## 9. Versioning

The literal string `IDR-AUTH-V1` at the start of the canonical
MAC input identifies the current authentication scheme.

A future scheme could introduce `IDR-AUTH-V2` with, for example,
a different timestamp encoding or an additional nonce. To
migrate without breaking existing peers:

1. Add `IDR-AUTH-V2` support to both signer and verifier.
2. Configure new peers to sign with V2 and to accept either V1
   or V2 from incoming messages.
3. Retire V1 when no V1 peers remain.

The version marker is inside the MAC input, so an attacker
cannot downgrade a V2 message to V1 without invalidating the
MAC.

## 10. Configuration

Environment variables read by clients, workers, and the
coordinator:

| Variable | Who reads it | Effect |
|---|---|---|
| `IDR_WORKER_SECRET` | coordinator, worker | shared secret for worker-channel messages; 32 bytes minimum in secure mode |
| `IDR_CLIENT_SECRET` | coordinator, client | shared secret for client-channel messages; 32 bytes minimum in secure mode |
| `IDR_INSECURE` | coordinator, worker, client | `1` disables authentication entirely; the coordinator additionally requires a loopback bind address in this mode |
| `IDR_BIND_ADDR` | coordinator | bind address; default `127.0.0.1` |
| `IDR_WAL_PATH` | coordinator | write-ahead log path; default `./idr-wal.log` |

In secure mode (the default), the coordinator refuses to start
without both secrets. In insecure mode it refuses to start unless
the bind address is loopback.

The coordinator logs its chosen mode at startup
(`auth_mode_secure` or `auth_mode_insecure`).

## 11. What this protocol does not provide

- **Encryption.** The wire is plaintext. TLS is out of scope.
  Authentication detects tampering; it does not hide content.
- **Application-level exactly-once submission.** A client that
  resubmits after a network failure produces a new task with a
  new ID. HMAC plus the replay cache do not prevent this.
- **Key rotation.** Secrets cannot change without restarting
  the affected processes.
- **Per-client identity.** Every holder of `IDR_CLIENT_SECRET`
  has the same authorization.
- **Forward secrecy.** A leaked secret forges any past or
  future message.
