# Controlled Agent Python execution

The optional `analysis-core.v1` runtime remains CPython 3.14.10 plus NumPy 2.5.4.
The main C++ application does not embed Python or PyQGIS. Runtime and template
preparation remain operator actions, outside the Agent tool surface.

`paleo.python_compute` accepts bounded source and the same exact `well_curves`
references as the trusted analyses. The host resolves paths, versions, permissions
and policy. Source is at most 16 KiB of complete UTF-8. The input is at most 16
same-well columns and 8192 samples per column, with declared, exactly aligned MD
axes. Units remain per column; there is no implicit interpolation or conversion.

The script receives a little-endian uint32 JSON byte length, that JSON control
object, then little-endian float64 axis, row-major float64 values, and uint8 quality
flags. Bits 1/2/4 mean missing/abnormal/edited. Metadata declares shape and units.
The generated tool's schema describes this protocol and the output contract.

stdout must contain one `paleo.python-output.v1` object with a single REPORT port.
Its `paleo.python-generated-report.v1` payload contains metrics, tables and notes:
at most 64 metrics, 4 tables, 16 columns and 128 rows per table, 8192 total cells,
and 16 notes of at most 2048 UTF-8 bytes. The accepted report is at most 256 KiB.
The host checks structure, duplicate keys, complete encoding and finite numeric
values, then adds source, code and runtime identities. Values and unit declarations
from generated programs are explicitly marked as not independently recomputed.
stderr and failed stdout prefixes are bounded private diagnostics, not publishable
report ports. Chat receives a small summary and operation reference; detailed bytes
use the existing authenticated paging API.

## Independently versioned inputs and outputs

The curve REPORT-only capability above keeps its existing v1 contract. Additional
capabilities select explicit [numeric-table projections](../../docs/PYTHON_NUMERIC_TABLE_INPUT.md)
or [authenticated seismic trace windows](../../docs/PYTHON_SEISMIC_TRACE_INPUT.md).
Table input has at most 16 columns including its row axis and 8192 rows; trace
input has at most 8192 samples. The host freezes exact source references, units,
revisions, read evidence and bytes. No path, SQL, inferred alignment or interpreter
selection is accepted from a tool.

The [numeric-output capability](NUMERIC_OUTPUT.md) accepts REPORT plus a typed
table that the host encodes as CSV. The [joint-output capability](../../docs/PYTHON_GENERATED_OUTPUTS.md)
requires REPORT, TABLE and IMAGE together; the host encodes straight RGBA8 into a
nonspatial PNG. CSV and PNG are each at most 4 MiB; image dimensions are each at
most 1024. The complete frame and final group also obey their 12-MiB protocol
ceiling and the smaller approved execution policy. Any member failure rejects
the group. These are ceilings, not a promise that every maximum-size input or
incompressible image fits simultaneously.

All variants use the existing Host, TaskService, InputBundle, operation,
authenticated paging and grouped publication chain. Old operations keep their
original strict validators and restorers. Generated numerical values and pixels
are not independently recomputed by the host. No GIS layer is automatically
created for a PNG.

The [managed environment registry](CONFIGURATION.md) selects a complete approved
runtime/template/policy identity for new and historical operations. Installation,
integrity, activation and actual execution readiness are distinct states. Trusted
and generated permissions remain independent. The legacy variables below apply
only as the documented complete fallback when no host record exists.

## Admission and platform limits

Set `PALEO_AGENT_PYTHON_ENABLE_GENERATED=1` only in the trusted host configuration,
alongside the installed runtime root and host-approved manifest SHA. This flag is
independent of `PALEO_AGENT_PYTHON_ENABLE_TRUSTED`; neither grants the other mode.
Generated programs require the host's LocalExecute effect. Publishing is a separate
intent and uses the existing Catalog/write-claim contract.

Readiness runs asynchronously and is bound to the configured runtime and policy.
It must prove a fixed NumPy computation through the actual strong backend; API
presence, ordinary runtime import, and a template's existence are separate facts.
Tool listing reads a nonblocking cached snapshot. Execution revalidates runtime,
frozen input and platform limits at each start. Missing prerequisites fail closed.
Generated-mode preparation and environment checks use the execution-free
`inspectIntegrity()` API. A generation lease is not an immutable execution
snapshot: version and NumPy imports are proved inside the locked/sealed strong
backend, without an ordinary interpreter preflight in the generated path.

| Environment | Backend and required facts | Limit of the claim |
|---|---|---|
| Windows x86-64 | LPAC AppContainer, protected runtime/input capability ACLs, atomic Job assignment, process-tree lifetime journal, committed-memory and aggregate CPU accounting | The runtime/OS combination must pass the actual probe. Build/API presence alone is insufficient. Native validation is recorded separately from a complete Windows GUI application build. |
| Linux x86-64, Qt >= 6.7 | Approved root-owned helper/ABI profile, bubblewrap namespaces, sealed runtime/input memfds, seccomp, delegated cgroup v2 CPU/memory/pids and a user manager | Current approved profiles are the explicitly hashed Arch helper stacks. Other distributions remain unavailable until their closure and full matrix are validated. |
| Current WSL validation environment | Helper checks pass, but the user manager lacks the CPU controller | `python_linux_resource_limits_unenforced`; no generated execution. This result does not generalize to every WSL installation. |
| Linux with older Qt | Required process-launch preparation API absent | `python_sandbox_qt_unsupported`; no ordinary-process fallback. |
| Other platforms | No accepted strong backend/runtime combination | Unavailable. |

Both strong backends deny child disk writes, network, new processes and host
credentials. Host-owned input/staging files remain outside child write authority.
Typed input and report/table/image output frames travel through bounded pipes. This is the enforced zero-write workspace
choice; temporary-file workflows and package installation are outside this profile.

The Windows Job allows one process and at most one core of aggregate CPU
parallelism. NumPy thread environment settings are one. Arbitrary native thread
count is not independently limited by a Job; readiness explicitly reports
`threadCountEnforced=false`. Extra threads remain within the same hard CPU,
committed-memory and wall budgets. Linux additionally denies thread/process clone
operations. A numeric-library thread setting is not presented as an OS thread-count
guarantee.

The process-wide execution permit admits one calculation at a time, including
readiness and input materialization. Child memory counters, host numeric staging
reservations and strong-backend snapshot reservations are reported separately.
They do not measure total GUI RSS or physical storage reads. Before SDK I/O,
generated input preparation reserves its bounded vectors, packed payload and
scratch against the host policy and rejects insufficient budgets.

## Persistence and recovery

Each invocation uses a fresh process. Content hashes freeze code, input, runtime
manifest and policy in the existing private-input and operation records. Reusing
a client request ID with different source is rejected. There is no persistent
REPL state shared with generated computations.

prepare/compute-only do not register formal results. The host validates every declared business port
before storing the complete private group. publish references the original operation, keeps exact
parents and uses existing grouped commit, receipts and idempotency. Reconstructing
the host does not rerun the original code to publish an authenticated result.
Diagnostic artifacts cannot be promoted as result ports.

The optional [Pi core](../pi/README.md) is a trusted orchestration component,
not a sandbox. C++ retains model credentials, tool admission, stable invocation
bindings and durable publication facts. A Pi session uses the same capabilities
and permissions; installing Node or selecting Pi grants no Python execution
permission. Default builds and the native conversation engine require no Node.

The existing human console, REPL and MAMCL environment retain their separate
behavior. Shell, arbitrary CLI, GPU, network, package installation, persistent
interpreters and general filesystem access remain deferred. Verification results
and precise OS/compiler/runtime identities belong to the per-batch records in
`docs/progress/`, not to this support-contract description.
