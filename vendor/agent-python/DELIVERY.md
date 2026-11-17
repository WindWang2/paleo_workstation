# Optional Python offline delivery stream

E1 separates **producer preparation** from **client installation**. A developer or
operator uses the existing runtime/scripts preparers on the supply side, then
`tools/bundle_agent_python.py` packages an already verified generation. The C++
installer consumes the resulting stream without system Python, pip, tar, unzip,
network access or interpreter execution. This does not alter the default
application installation, enable either execution mode or establish OS readiness.

The existing source locks, normalization transforms and manifest identities remain
unchanged. This is a delivery envelope, not a second environment builder. Runtime
and scripts have separate packages and generations. Installing either must not
implicitly enable trusted or generated execution.

## Producer commands

Prepare the locked runtime with [the existing operator tool](README.md), or the
three fixed templates with [the scripts preparer](scripts.README.md). Then use the
generation root and manifest SHA from that independent preparation record:

```text
python tools/bundle_agent_python.py --kind runtime --generation /operator/runtime-store/generations/<manifestSHA> --manifest-sha256 <manifestSHA> --output /operator/delivery/runtime.paleo-python --purpose internal
python tools/bundle_agent_python.py --kind scripts --generation /operator/scripts-store/generations/<manifestSHA> --manifest-sha256 <manifestSHA> --output /operator/delivery/scripts.paleo-python --purpose internal
python tools/check_agent_python_delivery.py --archive /operator/delivery/runtime.paleo-python --sha256 <independently-recorded-whole-package-SHA>
```

These are producer tools, never application-startup dependencies or Agent tools.
They do not download, install packages, probe the interpreter or execute templates.
Scripts must match this checkout's fixed template hashes and source runtime lock.
Runtime profile, versions, interpreter and source lock must also match this checkout.

The JSON result includes `archiveSha256`, `archiveSize`, `manifestSha256`, `kind`,
`platform`, `licenseStatus`, `purpose=internal`, and `redistributionApproved=false`.
The whole-package SHA must be copied into a **separate trusted host delivery
record**, reviewed through the host's deployment channel. A checksum supplied by
the same untrusted package or by a model does not authorize installation.
The header cannot grant permission or replace the host's compiled compatibility
checks. The checker verifies a stream; it neither extracts it nor grants execution.

Output is deterministic for the same generation. Files copy and hash in 64 KiB
blocks; only bounded metadata is read as a whole. Each source file's opened
identity, size, modification metadata and copied SHA are checked, and the complete
generation is rechecked before output publication. Missing/extra entries, changed
files, links, hard links, reparse points, nonportable paths and case aliases reject.
The output must lie outside the source generation. A different existing output is
never replaced. Publication uses a same-directory atomic no-replace hard link,
then removes its private temporary name; unsupported filesystems fail closed.
This publication mechanism does not put hard links in the runtime payload.

## Version 1 wire format

The extension is descriptive only; consumers check these bytes:

1. Exactly nine bytes: ASCII `PALEOPY1` followed by LF (`50 41 4c 45 4f 50 59 31 0a`).
2. Four bytes: unsigned little-endian header length, between 1 and 4 MiB.
3. That many bytes of canonical JSON: UTF-8/no BOM, `ensure_ascii=False`, sorted
   keys, separators `(',', ':')`, finite values only, and exactly one final LF.
4. Each entry's uncompressed raw bytes, consecutively in header `entries` order.
   No entry header, padding, separator, terminator, ZIP metadata or trailing bytes.

The header has exactly these six keys:

```json
{
  "schema": "paleo.python-delivery.v1",
  "kind": "runtime",
  "platform": "windows-x86_64",
  "manifestSha256": "<external manifest SHA>",
  "licenseStatus": "runtime-licenses-included",
  "entries": [{"path":"LICENSE.txt","size":123,"sha256":"<file SHA>","executable":false}]
}
```

The example is schematic, not a complete installable header. Kind is `runtime` or
`scripts`. Runtime platform is `windows-x86_64` or `linux-x86_64`, with
`licenseStatus=runtime-licenses-included`; scripts use platform `all` and preserve
`notice-only` or `repository-texts-included` from their original manifest.

Entries are strictly sorted by their relative `/` path. Every entry has exactly
`path`, `size`, `sha256`, and `executable`; sizes are JSON integers, not booleans or
floating values, and executable is a boolean. Paths are portable, at most 512
characters, never absolute or `.`/`..`, and cannot contain backslash, colon,
control characters, trailing dots/spaces or Windows device names. Both complete
paths and implicit parent directories reject case-folded aliases and file/directory
collisions. There are at most 16,384 files and 49,152 total implied files/directories.

Each file is at most 1 GiB. Runtime payload bytes, including its manifest, total at
most 1 GiB + 4 MiB; scripts payload totals at most 4 MiB. The original runtime
manifest still limits its own listed content to 1 GiB. Scripts remain flat,
nonexecutable data with the original three-file and notice/licence allowlist.
The original manifest itself is included as a nonexecutable entry:
`runtime-manifest.json` (at most 4 MiB) or `scripts-manifest.json` (at most 64 KiB).
Its entry SHA must equal the header `manifestSha256`. No outer delivery metadata is
added to an installed runtime/scripts generation.

Whole-file SHA authenticates the complete stream; per-entry SHA authenticates the
exact delivered file bytes. Consumers must reject unknown keys, duplicate JSON
keys, duplicate/extra paths, impossible sizes, bad canonical encoding, incomplete
payloads, trailing bytes, wrong platform and incompatible manifests. A linked or
special source is not normalized silently into an authorized package.

## Installation and retention contract

Build the deployment executable explicitly and install its optional component:

```text
cmake --build <build> --target paleo_python_environment -j1
cmake --install <build> --component AgentPythonDeployment --prefix <optional-deployment-prefix>
<optional-deployment-prefix>/bin/paleo_python_environment --package /absolute/path/runtime.paleo-python --sha256 <independent-whole-package-SHA> --store /absolute/path/runtime-store
<optional-deployment-prefix>/bin/paleo_python_environment --package /absolute/path/scripts.paleo-python --sha256 <independent-whole-package-SHA> --store /absolute/path/scripts-store
```

On Windows the executable has the `.exe` suffix and absolute paths use a local
drive, for example `C:/paleo/offline/runtime.paleo-python`. The target and the
`AgentPythonDeployment` installation component are `EXCLUDE_FROM_ALL`; neither is
a default installation dependency. The runtime and scripts stores are separate
dedicated directories. JSON output includes the installed identity and a stable
reason code; it always reports execution disabled and no redistribution approval.

The C++ side checks an externally approved whole-package SHA, compatibility and
budgets, writes only its private staging tree, validates the original manifest and
all copied files, then publishes a complete generation with a same-filesystem
rename. Only afterward may the installation selection be replaced atomically.
Failure/cancellation **before selection commit** preserves the preceding selection;
a complete but unselected generation can remain after its rename. Existing
generations are verified rather than edited. Installation selection is not E2
execution activation. The installer does not invoke either preparer on the client
or execute a probe.

After selection replacement, directory-fsync failure reports
`python_delivery_commit_uncertain`: the new selection may already be committed,
so an error result does **not** promise the previous pointer remains. Check the
current selection and verified installed identity first, then retry the same
package/SHA if needed. Do not delete a generation, blindly roll back, or infer an
uncommitted installation from the nonzero exit code. A late cancellation also
cannot reverse an already committed selection.

The installer uses the producer-compatible OS store locks: `.prepare.lock` for
runtime and `.scripts.lock` for scripts. These are separate from the application's
generation-use leases at `generations/.leases/<manifestSHA>.lock`. A lock file's
existence or absence is not permission to delete a generation. No GC or old-version
removal is introduced. Active, rollback, in-use and persisted operation references
remain retained; a valid installed runtime is not a strong execution snapshot.

## Licence boundary

The repository [NOTICE](../../NOTICE.md) says its own licence is not finalized.
The three templates therefore retain `notice-only` unless actual repository
licence texts are present. `repository-texts-included` records their inclusion;
it does **not** establish ownership, a permissive licence or redistribution rights.
CPython/NumPy licences do not license these repository templates or the application.
Runtime licence status likewise records retained supplier material, not an
independent legal approval.

Within this authorized local checkout, development, internal packaging, installation
tests and operator validation can proceed while preserving all notices. Nothing
here grants rights beyond the owner's authorization. External shipping requires
an independent owner-approved distribution decision and the required component
notices; no such approval format is implemented in E1. Consequently
`--purpose redistribute` **rejects both kinds unconditionally**, including scripts
that contain a file named LICENSE. Renaming the package or changing a manifest's
status is not an approval path. Internal outputs remain explicitly nonapproved
for external redistribution in the producer result.

## Source checks and selftests

```text
python tools/check_agent_python_delivery.py
python tools/check_agent_python_delivery.py --selftest
```

The default check needs only source files. Selftests use tiny nonexecutable runtime
fixtures and the fixed repository template bytes; supplied code is never run.
They cover deterministic output, wrong external anchors, incompatible sources,
same-size changes during copying, malformed headers/paths/aliases/size claims,
hard links/reparse guards, truncation/corruption, existing-output retention and
redistribution denial. Neither command downloads or installs a runtime. Register
both alongside the existing unlabelled `agent_python_supply*`/scripts supply tests
and CI Source gates; supply selftests alone do not prove the C++ installer or an
OS backend.
