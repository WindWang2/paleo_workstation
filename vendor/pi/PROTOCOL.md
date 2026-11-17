# A1 fixed Pi handshake

This version is `paleo.pi-agent.v1`. It is a short-lived supply/ABI handshake, not
the A2 conversation protocol. The only accepted commands are `hello` and `close`.
There is no model request, tool dispatch, operation publication or project path.

## C++ integration seam

- [`piruntime.h`](../../src/services/piruntime.h): `RuntimeConfig` defaults disabled;
  its root and manifest SHA are one trusted host identity. `inspectRuntime` is
  worker-only and performs full file verification without starting Node. The
  returned opaque `VerifiedRuntime` retains the generation but is not permission
  to execute a model or tool. `status.executionReady` remains false in A1.
- [`piprocess.h`](../../src/services/piprocess.h): `HandshakeRequest` separately
  defaults disabled, takes that verified identity, host session/epoch and a
  nonblocking cancellation callback. `runHandshake` revalidates before launch,
  uses a fixed program/entry, and rejects application-owner-thread execution.
- [`piprotocol.h`](../../src/services/piprotocol.h): bounded canonical frame codec.
  Reuse the byte boundary in A2, but introduce an explicit conversation contract
  rather than silently granting new commands to this handshake.

No caller selects executable arguments, working directory, environment or a
module path. Node runs in a private temporary directory with a constructed
environment: no NODE_OPTIONS, NODE_PATH, compile cache, provider keys, proxy or
user auth configuration. Its supplied module scope avoids surrounding project
package configuration. Stdout is protocol only; stderr is bounded private
diagnostic data, never authority or a successful tool result.

## Frames and identities

Each frame is a uint32 little-endian byte length followed by 1–65536 bytes of
canonical compact JSON. Object keys are sorted, UTF-8 is strict, and the body
has no BOM, whitespace padding, duplicate keys or trailing text. Diagnostics are
limited separately to 65536 bytes. Partial input never yields a partial object.

The host issues all identities. `generation` is a canonical unsigned 64-bit
decimal **string**, including values above JavaScript's safe integer range.
`sessionId` is 1–128 ASCII letters/digits/underscore/hyphen. `nonce` is 32 lowercase
hexadecimal characters. `sequence` is a string fixed by this state machine.
They are echoed unchanged; Pi does not mint epochs, scopes or operation IDs.

1. Host sends exactly `protocol`, `type:"hello"`, `sessionId`, `generation`,
   `sequence:"1"`, `nonce`.
2. Peer echoes them with `type:"hello_result"` and adds the exact locked
   `upstreamCommit`, `coreVersion`, `nodeVersion`, `toolExecution:"sequential"`,
   `modelTransport:"host-only-unconnected"`, `sandboxed:false`.
3. After validating that response, host sends the original identity with
   `type:"close"`, `sequence:"2"`.
4. Peer returns only that identity with `type:"closed"` and exits normally.

Extra fields/frames, wrong identities, reordering or an unexpected exit fail the
exchange. Success requires both responses and normal exit zero. Any failure
clears the peer result. Closing stdin terminates the fixed entry, including
after host death; partial input then exits with failure. Node also has its own
five-second lifetime watchdog after loading the entry. C++ checks a five-second deadline including identity
verification/permit wait, followed by bounded kill/reap cleanup.

## Mechanisms and limits

The implementation uses a Windows kill-on-close Job with one active process,
attached before the first request. Linux uses no-new-privileges and a checked
parent-death signal; Qt >=6.6 is required there to close unrelated descriptors.
These mechanisms require real native acceptance; the Node-only protocol probe
does not establish the C++ launcher or Job behavior. Linux does not claim an
arbitrary process-tree sandbox: the fixed reviewed entry does not expose child
creation. No OS CPU/memory/thread hard limits are claimed on either platform.
Thread/JIT behavior must not be described using Python's process profile.

A2 must retain one engine per host run, the existing host budgets/admission and
durable call-to-operation identity, and distinguish user cancel from context
interruption. `agent_end` alone is not idle: awaited listeners settle before
`waitForIdle`. A process exit is not rollback and must never cause repeat publish.
