# Optional host Python configuration

Installing an E1 delivery package does not enable execution. The optional native
`paleo_python_environment` deployment executable can also verify and activate a
host-approved environment. It uses QtCore services, without system Python,
network access, package installation, or an interpreter probe during activation.
The executable and runtime remain optional; the default application and NoORT
build work without them. Package production and licensing remain governed by
[DELIVERY.md](DELIVERY.md); configuration supplies no redistribution grant.

The default host file is `paleo/paleo_workbench/agent-python.json` below Qt's
`GenericConfigLocation`. It is independent of the executable name and the open
project. `--configuration` selects an explicit absolute host configuration path.
Project files, operation records and model arguments cannot select this path or
approve new roots.

## Sources and complete identities

Source selection is atomic: an existing host configuration takes priority over
the legacy environment group. A malformed host file fails closed. Only when the
host file is absent are these five named host variables captured together:

* `PALEO_AGENT_PYTHON_ROOT`
* `PALEO_AGENT_PYTHON_MANIFEST_SHA256`
* `PALEO_AGENT_PYTHON_SCRIPTS_ROOT`
* `PALEO_AGENT_PYTHON_ENABLE_TRUSTED`
* `PALEO_AGENT_PYTHON_ENABLE_GENERATED`

Roots and anchors are never filled from different sources. With a host record,
literal `0` in either legacy enable variable remains a deny-only ceiling for
that mode; `1` cannot override a disabled host record. The legacy fallback keeps
the existing raw-template directory mode, checking the three compiled template
SHA values without inventing a scripts manifest or approving unknown scripts.
This mode is explicitly reported as legacy and is not persisted as a newly
approved managed identity. No provider keys or other environment variables are
captured.

An approved host identity hashes its canonical runtime root and manifest SHA,
scripts mode/root/manifest SHA, complete execution policy, and compiled
compatibility contract. Trusted execution requires the complete E1 scripts
manifest and independent anchor. Generated-only execution can explicitly use
`scripts.mode=none`, with both scripts root and SHA empty. It still requires the
strong backend's real sealed-runtime probe. Generated admission never uses an
ordinary Python import probe or turns on the trusted permission.

## Deployment commands

Obtain all SHA values independently from the authorized supply record; hashes
printed by a project or model are not authorization. For example:

```text
paleo_python_environment --status
paleo_python_environment --activate --runtime-root ABSOLUTE_RUNTIME_GENERATION --runtime-sha256 APPROVED_RUNTIME_MANIFEST_SHA --scripts-root ABSOLUTE_SCRIPTS_GENERATION --scripts-sha256 APPROVED_SCRIPTS_MANIFEST_SHA --trusted --generated --expected-revision 0
paleo_python_environment --activate --runtime-root ABSOLUTE_RUNTIME_GENERATION --runtime-sha256 APPROVED_RUNTIME_MANIFEST_SHA --generated --expected-revision 1
paleo_python_environment --rollback APPROVED_IDENTITY_ID --expected-revision 2
paleo_python_environment --revoke APPROVED_IDENTITY_ID --expected-revision 3
paleo_python_environment --disable --expected-revision 4
```

The trusted/generated switches grant each mode independently. Omitting a mode on
explicit activation removes that mode from the permission ceiling and interrupts
its old workers, while preserving the other mode. A normal version change with
unchanged permissions preserves authorized in-flight workers and their providers.
Rollback only changes the selected approved version; it preserves the current
permission ceiling and disabled state rather than restoring wider old grants.

Mutations use the last observed revision as a compare-and-swap value, an
interprocess lock, a bounded canonical record and atomic replacement. POSIX
configuration files are private to the owner/root and reject group/world writes.
Activation verifies complete manifests and compatibility before the save; ordinary
verification or revision failure preserves the previous configuration. A failure
after atomic replacement can report `python_environment_config_commit_uncertain`:
read the installed identity and revision before retrying instead of assuming that
the previous pointer survived.

## App lifecycle and diagnostics

`PythonEnvironmentRegistry` is the stable, host-owned directory shared by the
runner and its execution adapters. App assembly installs adapters once for each
document binding. Configuration refresh, hashing, activation and probes use the
existing TaskService workers. Owner-thread status and provider snapshots report
pending through `tryLock`; they do not wait for hashing, permits or child startup.
No additional worker pool or operation path is introduced. A file watcher
refreshes the existing registry after host configuration replacement, without
rebinding the document or cancelling unrelated tasks.

The bounded, path-free `status()` distinguishes saved `enabled` from
`emergencyDenied` (all execution denied in this process), `inMemoryDeny` (global or
current-identity deny), mode ceilings, source, revision and integrity state.
Installation, integrity and actual execution readiness are separate facts.
Activation does not advertise successful execution until the selected mode's
real backend probe succeeds. A diagnostic spanning two configuration revisions
reports `python_environment_config_changed` rather than combining their proofs.

Host controllers call `denyInMemory()` immediately for stop-all or a selected
identity, before queueing durable disable/revoke through TaskService. It performs
no I/O or task admission. A failed save leaves the in-process deny in force and
the diagnostics show that saved `enabled` can still be true. An activation or
rollback already running when a newer deny arrives cannot clear it. Only a new,
verified explicit activation may grant fresh tokens; previously interrupted
tokens remain interrupted.

Historical operations resolve an approved environment ID and its exact frozen
runtime SHA/policy through this directory, never a root supplied in the operation.
The frozen InputBundle continues to own and verify its original code and data.
Publishing an already computed result does not require the original template
delivery directory and does not re-execute code or reread a newer SDK head.
Current permissions and revocation still apply before an uncommitted publication,
including after the publishing checkpoint. Committed SQL facts retain the existing
reconciliation and replay behavior, even when execution has since been disabled.

Approved/revoked history is bounded (64 identities, 256 deny tombstones). E2 does
not delete generations or implement GC. Existing runtime leases and persistent
operation references remain retention boundaries. UI diagnostics and configuration
controls belong to E3; these host services contain no QtWidgets dependencies.
