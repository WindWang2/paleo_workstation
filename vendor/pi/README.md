# Optional locked Pi core runtime

This is the A1 supply and fixed-handshake boundary from
[the implementation plan](../../docs/planning/PYTHON_DELIVERY_AND_PI_AGENT_PLAN.md).
It does not connect a Pi conversation, model transport, tools or project resources.
The application remains a C++ application and does not need Node to build or start.
See [the protocol](PROTOCOL.md) for the host-only interfaces and current limits.

## Reviewed source and actual closure

`source.lock.json` pins `earendil-works/pi` commit
`c3cc91632d0a1950fb122605b190233f2aa0006e`. Its package version is 1.5.2, but this
commit also contains the unreleased #9986 abort/tool-result fix. It is not replaced
by the npm 1.5.2 release. The full upstream archive SHA, selected-source SHA list,
original npm lock, build recipe, entry source and compiler artifacts are locked.
No local upstream source patch is applied.

The supply build resolves the locked agent, ai, schema and telemetry source
packages directly, with an explicit TypeScript configuration. Esbuild produces
one self-contained module. The reviewed `bundle-graph.lock.json` records the code
that remains in that module; provider/auth/catalog/coding-agent resources and
external imports are rejected by the build. The original upstream npm lock is
retained as provenance; unused provider SDK packages are not installed or shipped.
The only npm archives consumed by the supply compiler are pinned esbuild and its
platform binary, checked against both SHA-256 and the upstream lock's SHA-512 SRI.
No npm command or lifecycle script is run.

Node 22.23.5 Windows/Linux x86-64 archives are pinned against the official
[`SHASUMS256.txt`](https://nodejs.org/dist/v22.23.5/SHASUMS256.txt), retained here
verbatim. The trust basis is that reviewed SHA evidence; no GPG signature
verification is claimed. The extracted Node program and final module also have
payload SHA pins compiled into the C++ verifier. Node's OS ABI dependencies remain
a platform validation requirement, not a promise that the Node archive supplies
the operating system.

Pi's MIT license is preserved in `LICENSE.upstream`. The schema's adapted format
validators have their original third-party notice, and the build compiler's
license is preserved. Each runtime generation also includes Node's complete
platform-archive LICENSE. Do not describe the whole closure as MIT-only.

## Explicit developer/operator supply

The following are deployment tools, never Agent tools or application startup
actions. Use a private cache and a separate runtime store. Their roots must not
contain one another. Source archives and generated runtimes are not committed.

```sh
python tools/prepare_pi_agent.py --fetch --cache /private/pi-cache
python tools/prepare_pi_agent.py --offline --cache /private/pi-cache \
  --store /private/pi-store --platform linux-x86_64 --archive /private/pi-linux.zip
```

Use `windows-x86_64` for the Windows package. Either supported supply host can
prepare either target from the same complete offline cache. The build uses the
locked host Node and esbuild, not PATH, global npm or user configuration.
The generation contains only the fixed Node program, bundled entry, its explicit
module scope, build provenance and licenses. It contains no npm, node_modules,
coding-agent CLI/RPC, filesystem/Shell tools, extensions, MCP, skills or discovery.

A complete generation is verified and sealed before same-parent publication to
`generations/<manifest SHA>`. Only then is `current.json` atomically replaced.
Failed preparation/package creation preserves the previous active pointer and
every old generation. Repeated preparation verifies an existing generation;
it never edits it. Retention and current pointers are not execution permission.
There is no automatic GC, update or online application installation.

The operator must provide the complete root and manifest SHA to trusted host
configuration and explicitly enable Pi. The source manifest does not authorize
itself. A1 has no application settings/Agent-selection wiring: actual model and
tool execution remain unavailable until A2 connects and validates those paths.

## Execution boundary

The fixed entry only handles `hello` and `close`, constructs an Agent with empty
tools and explicit sequential scheduling, and supplies a stream function that
returns `pi_model_transport_unconnected`. It does not create ModelRuntime,
createAgentSession, a provider auth store or a resource loader. Unknown commands
fail; no prompt or arbitrary JavaScript is accepted.

Pi is trusted optional application code, **not a sandbox**. The short A1 process
has bounded IPC, deadlines, cancellation and host-death cleanup mechanisms.
It does not claim OS memory/CPU/thread hard limits, filesystem or network
isolation, or the existing Python sandbox profile. Long A2 sessions require their
own resource/lifetime acceptance. In-flight failures do not trigger a fallback
engine or replay any operation.

## Optional installation component

The default build and default installation do not contain Node or executable JS.
An operator may configure `PALEO_PI_INSTALL_ROOT` to an already prepared generation
and `PALEO_PI_INSTALL_SHA256` to its separately approved manifest SHA, then run
`cmake --install <build> --component AgentPiRuntime`. This component is explicitly
excluded from the default installation. The native CMake guard checks complete
file hashes and the compiled Node/bundle/source-lock pins, copies to same-parent
staging, verifies again, then publishes
`<prefix>/libexec/pi-agent/generations/<manifestSHA>`. Existing generations are
verified and retained. Interrupted staging can remain for manual inspection;
there is no automatic GC. Installation neither executes Node nor writes active
configuration nor enables Pi. A1 has no user-facing activation/Agent command.
