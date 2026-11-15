# A2 host-mediated Pi conversation contract

`paleo.pi-conversation.v1` is distinct from the A1 handshake protocol. A1
`hello`/`close` retains its original meaning and deadlines. This contract is for
the fixed reviewed optional entry only, with `Agent.toolExecution=sequential`, a
custom `streamFn`, and host-proxy tools. It enables no coding-agent tools,
generated JavaScript, extensions, Shell, filesystem APIs or network providers.
Node remains trusted optional orchestration code, not an OS sandbox.

## Host selection

The default engine is the existing native C++ loop. App assembly selects Pi only
when the trusted host provides `PALEO_AGENT_PI_ENABLE=1`,
`PALEO_AGENT_PI_ROOT=<prepared-generation>` and
`PALEO_AGENT_PI_MANIFEST_SHA256=<independently-approved-manifest-SHA>` together.
These are operator configuration, never model/tool/project arguments. No PATH
lookup, npm installation or provider credentials participate. An explicitly
selected missing or incompatible runtime reports failure; it does not replay the
turn through the native engine. Existing model endpoint, key and LlmClient
configuration continue to live exclusively in C++.

Host session JSON may contain `host_turn_id` and bounded `host_tool_bindings`.
The latter commits session/turn/call identity, document and canonical admitted
arguments to a stable host-derived client request ID before dispatch. Neither
field is sent to the model or accepted from Node as authority. Old sessions
without these optional fields remain readable. OperationLog and Catalog remain
the only execution/commit facts; the chat binding is not a second operation log.

## Frames and lifecycle

Each canonical UTF-8 JSON frame uses the A1 uint32LE length codec (1..65536
bytes). Its exact keys are `protocol`, `type`, `sessionId`, `generation`,
`turnId`, `nonce`, `sequence`, `payload`. Session and turn are host-created ASCII
safe IDs, generation and sequence are canonical uint64 decimal strings, and
nonce is 32 lowercase hexadecimal digits. All identities are fixed by the first
host frame and echoed unchanged. Sequence increases separately in each direction
from `1`; unexpected identities, sequence, fields or state reject the exchange.
Payload is an object with exact per-type keys. JSON schemas and tool arguments
are encoded as JSON *strings*, preserving the host's exact bytes rather than
depending on cross-runtime floating-point JSON formatting.

| Direction | Type | Payload | State |
|---|---|---|---|
| host | `open` | `{}` | First frame only |
| peer | `opened` | `upstreamCommit,coreVersion,nodeVersion,toolExecution,modelTransport,sandboxed` | Exact supply identity; sequential, host-only, false |
| host | `tool_definition` | `name,description,parametersJson` | Up to 128 unique host tools; one definition awaiting acknowledgement |
| peer | `configured` | `name` | Echo exactly the outstanding definition |
| host | `start` | `{}` | Configuration finished; one Agent run |
| peer | `model_request` | `round` | Canonical round string, starts at 1; no model credentials, history or tool authority |
| host | `model_delta` | `round,text` | At most 16KiB UTF-8 text per frame |
| host | `model_call` | `round,callId,name,argumentsJson` | Host-normalized admitted call identity; args at most 16KiB |
| host | `model_end` | `round,reason` | Complete validated assistant message; model calls become executable only here |
| host | `model_error` | `round,code` | No raw HTTP error, endpoint or key; terminates the stream with error |
| peer | `tool_request` | `round,callId` | Exact next call from the host-admitted response; no peer-supplied arguments |
| host | `tool_result` | `round,callId,ok,content` | Existing bounded ToolResultProtocol JSON, including references |
| peer | `idle` | `code` | After `prompt()` and `waitForIdle()` including awaited listeners; `ok`, `error`, `cancelled`, or `interrupted` |
| host | `abort` | `reason` | `cancelled` or `interrupted`; settle pending streams/tools, then await idle |
| host | `close` | `{}` | After idle |
| peer | `closed` | `{}` | Final frame, then normal exit zero |

The custom stream function requests a host model round, then produces real
AssistantMessageEventStream text/call/done/error events from the host frames.
Its context is an in-memory orchestration projection. C++ alone builds the
authoritative messages/history window and host tool schemas through the
existing LlmClient. Node never receives endpoints, API keys, caller scope,
project paths, write claims or commit credentials. A fixed internal user-turn
marker is sufficient for the core loop; it is never forwarded to the model.
The declared tool set is fixed for this host turn; new grants become visible on
the next turn. Current permissions/readiness are checked by the same C++ Runner
at dispatch, so a revoked tool is never made executable by its old declaration.

The host admits the complete model response with the same native limits:
5 tool rounds, 8 calls per response, 16 calls per host turn, existing argument
and character-estimated history budgets. Before forwarding calls as executable,
it atomically saves the host session/turn/call binding and canonical admitted
call. Save failure prevents dispatch. Stable host-derived clientRequestId values
route through the existing Runner/Host/OperationLog/Catalog chain. Duplicate
logical requests return a cached terminal result or reject; they never invoke
Runner twice. Node exit cannot undo or repeat a committed operation.

Transport queues are bounded (256KiB per direction, at most 32 pending frames),
stderr is independently bounded to 64KiB, and partial frames never become
partial messages. Host text is coalesced before framing: owner pending text and
the outgoing ring each reserve at most 128KiB and 16 frames, sharing the stated
256KiB/32-frame ceiling. The text reservation includes conservative JSON framing
overhead; it is a message budget, not a process RSS claim. A temporarily full ring
retains the unsent prefix and retries; model calls/end follow all text in order.
An actual total pending-text overflow fails closed instead of truncating JSON.
The QProcess write buffer is separately bounded to 256KiB, plus one <=64KiB
transport frame; neither is an unbounded Qt event queue.
Configuration is bounded to 512KiB in aggregate. Only one conversation process
is admitted per application at a time; a second request fails busy instead of
occupying a TaskService worker waiting for a permit. The host requires at least
two workers so the sidecar cannot occupy the only worker needed by its tools.
Normal completion requires
validated idle, close, closed and normal exit zero. The host turn wall bound is
30 minutes; this is an optional-engine deadline, not an OS CPU/memory quota.
On cancellation/context invalidation the host cancels existing LLM/tools, pads
unanswered tool calls, sends abort and allows at most 2 seconds for shutdown
before bounded kill/reap. Late generation frames cannot dispatch tools or mutate
the host transcript. The selected engine never automatically fails over/replays.

Acceptance includes wrong nonce/session/epoch/turn/sequence; malformed/duplicate
JSON, encoding, partial/oversized/flood frames; duplicate or unadmitted tool
requests; model request while tools are pending; tool/result ID or order mismatch;
configuration overflow; stream errors/cancel; withheld awaited idle; abnormal
exit; source/save failure before dispatch; user cancel versus host interruption;
and real Pi + C++ LlmClient HTTP/SSE + Host operation replay/restart publication.
