# Bundled Pi Agent runtime

This is the locked supply and fixed-process boundary from
[the implementation plan](../../docs/planning/PYTHON_DELIVERY_AND_PI_AGENT_PLAN.md).
The workbench uses Pi for every Agent conversation. The A1 handshake and
A2 host-mediated conversation use distinct protocols.
Model transport, tool execution, authorization and durable facts remain in C++.
The application ships its own pinned Node executable and starts it when a
conversation requires the engine. See the [handshake](PROTOCOL.md) and
[conversation contract](CONVERSATION.md) for the interfaces and limits.

## Reviewed source and actual closure

`source.lock.json` pins `earendil-works/pi` commit
`c3cc91632d0a1950fb122605b190233f2aa0006e`. Its package version is 1.5.2, but this
commit also contains the unreleased #9986 abort/tool-result fix. It is not replaced
by the npm 1.5.2 release. The full upstream archive SHA, selected-source SHA list,
original npm lock, build recipe, entry source and compiler artifacts are locked.
The shared `prepare_agent_python.py` import supplying `StoreLock` is pinned as
part of the producer closure, with LF checkout enforced for cross-host hashes.
The fixed `conversation.mjs` source is separately pinned in `build.localModules`
and compiled into the same single-file bundle; it is not a discovered extension.
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
payload SHA pins compiled into the C++ verifier. Linux builds copy
`libgcc_s.so.1` and `libstdc++.so.6` from the managed dependency prefix into
an application-private directory. The host verifies both digests before adding
that directory to the isolated Node environment's library search path. Linux
portable verification checks the dynamic loader resolutions and exercises the
Pi handshake in the relocated package.

Pi's MIT license is preserved in `LICENSE.upstream`. The schema's adapted format
validators have their original third-party notice, and the build compiler's
license is preserved. Each runtime generation also includes Node's complete
platform-archive LICENSE. Do not describe the whole closure as MIT-only.

## Preparing an offline build

The following are deployment tools, never Agent tools or application startup
actions. Use a private cache and a separate runtime store. Their roots must not
contain one another. Source archives and generated runtimes are not committed.

```sh
python tools/fetch_pi_archives.py --cache /private/pi-cache
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
it never edits it. Separate workstations can verify and read the same immutable
generation concurrently. Each Pi process owns its own conversation channel.
Retention and current pointers are not execution permission.
There is no automatic GC, update or online application installation.

Pass the prepared directory to CMake with
`-DPALEO_PI_INSTALL_ROOT=<store>/generations/<manifest SHA>`.
A normal build can also prepare the runtime from the repository's locked archives.
Set `PALEO_PI_SUPPLY_CACHE` to reuse an existing archive cache. Each target platform
has its own approved manifest identity in [runtime-manifests.lock.json](runtime-manifests.lock.json).

## Execution boundary

The A1 `hello`/`close` protocol retains empty tools, sequential scheduling and
`pi_model_transport_unconnected`. The separate
[A2 conversation protocol](CONVERSATION.md) uses a custom stream function and
fixed host-proxy tools. C++ supplies admitted calls and bounded results; Node
does not receive provider keys, endpoints, host scope, paths or commit credentials.
Neither protocol creates ModelRuntime, createAgentSession, a provider auth store
or a resource loader. Unknown commands fail; arbitrary JavaScript is unavailable.

Pi runs trusted application code. The short A1 process
has bounded IPC, deadlines, cancellation and host-death cleanup mechanisms.
It does not claim OS memory/CPU/thread hard limits, filesystem or network
isolation, or the existing Python sandbox profile. A2 sessions have their own
bounded transport, lifetime controls and actual acceptance records. In-flight failures do not trigger a fallback
engine or replay any operation.

## Build and installation layout

The normal application build stages the generation at
`<build>/libexec/pi-agent/generations/<manifest SHA>`. A regular
`cmake --install <build>` includes the same generation at
`<prefix>/libexec/pi-agent/generations/<manifest SHA>`; packagers can also
select the `AgentPiRuntime` component. CMake verifies the manifest, files and
compiled Node/bundle/source-lock pins before publishing the staged generation.
It verifies an already installed generation before reuse. The application
resolves the bundled directory relative to its executable and verifies the
runtime on use. Model endpoint and key configuration stay in the workbench.
