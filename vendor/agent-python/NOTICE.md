# Optional runtime source and licence notices

The runtime is an optional, separately prepared extension. No third-party binary
archive is committed here or made a default application dependency. SHA256 pins
and exact supplier URLs are recorded in `runtime.lock.json`.

| Component | Source | Notice/terms |
|---|---|---|
| CPython 3.14.10, Windows | Python Software Foundation, python.org embeddable distribution | Python licence / PSF terms; upstream archive notice files retained |
| CPython 3.14.10, Linux | Astral `python-build-standalone`, immutable release `20261110` | CPython licence and bundled dependency notices retained; this is an Astral-built distribution |
| NumPy 2.5.4, both platforms | NumPy's CPython 3.14 wheels from PyPI | BSD-3-Clause and the wheel's bundled-library notices retained |
| Numerical native libraries | Bundled by the selected NumPy wheel | Preserve the wheel's complete licence text, including OpenBLAS/LAPACK and any compiler-runtime exceptions |

The preparer retains supplier notice files and NumPy `.dist-info` licence content.
The exact native closure is the pinned archive/wheel payload and full normalized
file manifest, not a claim that NumPy has no native dependencies. No system Qt,
QGIS, GDAL or Python packages are imported into this runtime by discovery.
Linux retains an explicit OS ABI dependency on the loader/glibc family, libgcc_s,
libstdc++ and zlib; the README lists measured symbol floors. These are OS supply
requirements, separate from the fully hashed optional-runtime file tree.

Official source references:

- [CPython release](https://www.python.org/downloads/release/python-31410/)
- [Windows embed digest evidence](https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip.sigstore)
- [Astral release](https://github.com/astral-sh/python-build-standalone/releases/tag/20261110)
- [Standalone distribution format](https://gregoryszorc.com/docs/python-build-standalone/main/distributions.html)
- [NumPy release metadata and SHA256](https://pypi.org/pypi/numpy/2.5.4/json)
- [NumPy licensing](https://numpy.org/doc/stable/license.html)

Updating a supplier, patch version, ABI, transform or dependency changes the source
lock and runtime identity. Rebuild the optional payload, verify its full manifest,
run native version/import/numerical checks, review licences, and update host anchors
in the same reviewed delivery. Source metadata obtained over HTTPS is pinning
evidence; this tool does not claim independent Sigstore signature verification.
