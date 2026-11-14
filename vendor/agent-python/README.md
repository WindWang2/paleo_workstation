# Optional Agent Python supply

`analysis-core` is a separate, optional CPython 3.14.10 + NumPy 2.5.4 runtime.
The application remains C++; this directory does not install Python in the default
application image, add PyQGIS, or change the user's console/REPL interpreter.
The runtime itself is not a security sandbox.

The Linux payload's 34 ELF objects were checked with a clean-environment `ldd`:
no missing dependencies. OS-provided ABI libraries are the ELF loader, libc, libm,
libdl, libpthread, librt, libutil, libgcc_s, libstdc++ and zlib. Their observed symbol
requirements are GLIBC <= 2.27, GLIBCXX <= 3.4.21 and CXXABI <= 1.3.9; the declared
glibc floor is conservatively 2.28. These OS libraries are not hidden in the runtime
file manifest. An isolation backend must supply its verified OS-library closure;
mounting the whole host `/usr` is not part of this runtime's contract.

`runtime.lock.json` is the source of archive URLs, exact sizes/SHA256, platform/ABI,
suppliers, licence notices and the small, explicit normalization transforms.
Windows uses the Python Software Foundation's embeddable ZIP. Linux uses Astral's
`python-build-standalone` release `20261110` (a downstream CPython distribution,
not a python.org Linux binary). NumPy wheels are the project's ordinary-GIL
`cp314` x86-64 wheels published on PyPI. Linux requires glibc >= 2.28 for this wheel.
The Windows embedded distribution uses an explicit `python314._pth`; no user site
or `import site` is enabled. Linux removes standalone distribution packaging
entry points/packages and the interactive terminal database (`share/terminfo`) from
the supplied runtime. The latter has upstream case-folding aliases; it is not used
by the noninteractive numerical profile. No pip is executed.

Run preparation explicitly as a developer/operator, outside Agent execution:

```powershell
python tools/prepare_agent_python.py --platform windows-x86_64 `
  --cache C:/paleo-offline/agent-python --store C:/paleo-runtime-store --fetch --probe
```

```bash
python3 tools/prepare_agent_python.py --platform linux-x86_64 \
  --cache /srv/paleo-offline/agent-python --store /srv/paleo-runtime-store --offline --probe
```

Offline is the default. `--fetch` is an explicit supply operation; it is not exposed
as an Agent tool. The cache filenames are exactly those in the source lock. A
missing, partial or corrupt archive is rejected offline, before extraction. The
tool never installs anything globally and does not copy or relocate a venv.
Use a fresh preparation on the destination host; the complete install-only Python
distribution is relocated, not an environment containing source-machine venv paths.
`--probe` requires the target's native x86-64 OS and checks the exact Python/NumPy
versions plus an actual NumPy dot product under a clean environment.

Preparation validates archive size/SHA, rejects hostile/duplicate paths and special
entries, safely materializes internal tar file links as regular bytes, and produces
`generations/<manifestSHA>/runtime-manifest.json`. The manifest lists every file's
relative `/` path, bytes, SHA256 and executable flag, plus the raw source-lock SHA.
Its encoding is UTF-8 without BOM, keys sorted, compact JSON plus one LF. The lock
also uses fixed LF. No absolute source-machine paths are embedded in the manifest.
Windows/Unix permission differences do not change the declared source file modes.

A process-lifetime OS lock serializes preparation. Files are built in a private
staging directory; only a complete verified generation can replace the atomic
`current-<platform>.json` pointer. Existing generations are never edited or removed.
Failures retain the previous pointer. The returned `manifestSha256` is intended
for trusted operator configuration; a manifest's own claimed hash is not a trust
anchor. The application independently verifies the manifest and the entire tree
against its configured anchor before probing or executing anything.

Set the application's trusted `PALEO_AGENT_PYTHON_ROOT` and
`PALEO_AGENT_PYTHON_MANIFEST_SHA256` configuration to the returned generation root
and manifest SHA. These values are host configuration, not tool arguments. Do not
configure the mutable selection file itself as the generation root.

The preparer **retains all old generations and does not implement GC**. Application
leases live at `generations/.leases/<manifestSHA>.lock`; preparation does not remove
them or infer permission to delete from their absence. Future explicit GC must
coordinate those leases and persistent operation references before deleting a
generation. Do not remove generations while operations or recovery may need them.
The read-only files and content checks here do not replace OS isolation of code.

Supply checks (no download or runtime installation):

```bash
python3 tools/check_agent_python_runtime.py
python3 tools/check_agent_python_runtime.py --selftest
python3 tools/check_agent_python_runtime.py --runtime-root /absolute/generation \
  --manifest-sha256 <trusted-manifest-sha256>
```

The selftests exercise offline missing/corrupt sources, prior-selection retention,
immutable upgrades, archive traversal/case aliases/special entries, internal versus
escaping/cyclic links, byte budgets, full-tree tamper/extra-file detection, process
locking/process death, activation failure and cleanup containment. Actual CPython/NumPy and application readiness
checks are separate runtime tests; a supply selftest does not prove OS isolation.
